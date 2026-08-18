/*-------------------------------------------------------------------------
 *
 * polar_worker.c
 *	  Do some backgroud things for PolarDB periodically. Such as:
 *	  (1) auto prealloc wal files,
 * 	  (2) auto clean core dump files,
 *	  (3) auto clean xlog temp files.
 *
 * Copyright (c) 2020, Alibaba Group Holding Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * IDENTIFICATION
 *	  external/polar_worker/polar_worker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "miscadmin.h"
#include "pgstat.h"
#include "polar_worker.h"

#include "access/polar_logindex_redo.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "common/file_perm.h"
#include "common/pg_compress.h"
#include "funcapi.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "postmaster/startup.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/polar_fd.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/timestamp.h"


PG_MODULE_MAGIC;

#define POLAR_NUM_FILE_INTERVAL 32
#define POLAR_XLOG_TEMP_FILE_SUFFIX "xlogtemp"

#define POLAR_FILE_PATH(path, orign) \
	snprintf((path), MAXPGPATH, "%s/%s", POLAR_DATA_DIR(), (orign));

#define POLAR_CORE_DUMP_PATTERN_FILE "/proc/sys/kernel/core_pattern"

typedef struct
{
	time_t		mtime;
	char		file_path[MAXPGPATH];
}			polar_file_descriptor;

typedef struct compression_info
{
	int64		slen;
	int64		clen;
}			compression_info;

typedef struct compression_info_entry
{
	int64		identyfier;
	int			segments;
	compression_info info;
}			compression_info_entry;

typedef struct compression_calculate_arg
{
	/* required */
	char		compression_method[NAMEDATALEN];
	char		sub_directory[MAXPGPATH];
	int			unit_size;
	int			unit_count;
	int			step_factor;
	int			fit_nblocks;
	int			lowerbound;
	int64		extra_clen;
	int64		extra_slen;
	int64		ignore_size;
	bool		ignore_catalog_heap;
	bool		ignore_extended_heap;

	/* not required */
	char	   *chunk_buffer;
	char	   *compression_buffer;
	HTAB	   *file_table;
}			compression_calculate_arg;

typedef struct polar_worker_shm
{
	compression_calculate_arg prev_arg;
	compression_calculate_arg current_arg;
	bool		current_arg_is_valid;
	LWLock		lock;
	compression_info info;
}			polar_worker_shm;

polar_worker_shm *worker_shm = NULL;

void		_PG_init(void);
void		_PG_fini(void);
PGDLLEXPORT void polar_worker_handler_main(Datum main_arg);

static void polar_worker_shmem_request(void);
static void start_polar_worker(void);
static void prealloc_wal_files(void);
static void polar_worker_sighup_handler(SIGNAL_ARGS);
static void polar_worker_sigterm_handler(SIGNAL_ARGS);
static void polar_worker_sigusr2_handler(SIGNAL_ARGS);
static void polar_xlog_temp_file_clean(void);
static void polar_remove_old_core_files(void);
static bool polar_remove_file(const char *path);
static void polar_coredump_handler(void);
static int	cmp_file_mtime(const void *a, const void *b);
static bool polar_compression_calculate_arg_equals(compression_calculate_arg * arg1, compression_calculate_arg * arg2);
static compression_info polar_calculate_compression_info(compression_calculate_arg * arg);

/* Flags set by signal handlers */
static volatile sig_atomic_t got_sigterm = false;
static volatile sig_atomic_t got_sighup = false;

/* GUC variables. */
static bool enable_polar_worker = true; /* Start prealloc wal file worker? */
static int	polar_worker_check_interval;	/* Check interval */
static int	prealloc_wal_file_num;	/* How many files needed to prealloc */
static char *polar_core_file_path;
static char *core_name_suffix;
static int	core_file_outdate_time;
static int	num_corefile_reserved_old;
static int	num_corefile_reserved_new;
static int	xlog_temp_outdate_time;

static bool calculate_compression_ratio;
static int	calculate_compression_ratio_interval;

static shmem_request_hook_type prev_shmem_request_hook = NULL;

/*
 * Module load callback.
 */
void
_PG_init(void)
{
	DefineCustomBoolVariable(
							 "polar_worker.calculate_compression_ratio",
							 "Whether to calculate compression ratio.",
							 NULL,
							 &calculate_compression_ratio,
							 false,
							 PGC_USERSET,
							 POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGEABLE,
							 NULL,
							 NULL,
							 NULL);

	if (!process_shared_preload_libraries_in_progress)
		return;

	DefineCustomIntVariable(
							"polar_worker.calculate_compression_ratio_interval",
							"Sets the interval between calculating compression ratio",
							"If set to zero, calculate compression ratio in background worker is disabled.",
							&calculate_compression_ratio_interval,
							300,
							0, INT_MAX / 1000,
							PGC_SIGHUP,
							GUC_UNIT_S | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGEABLE,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable(
							"polar_worker.polar_worker_check_interval",
							"Sets the interval between polar worker check",
							"If set to zero, polar worker is disabled.",
							&polar_worker_check_interval,
							2,
							0, INT_MAX / 1000,
							PGC_SIGHUP,
							GUC_UNIT_S | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGEABLE,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable(
							"polar_worker.prealloc_wal_file_num",
							"Sets the num of how many prealloc wal file",
							NULL,
							&prealloc_wal_file_num,
							2,
							1, INT_MAX / 1000,
							PGC_SIGHUP,
							POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGEABLE,
							NULL,
							NULL,
							NULL);

	/* can't define PGC_POSTMASTER variable after startup */
	DefineCustomBoolVariable(
							 "polar_worker.enable_polar_worker",
							 "Starts worker or not.",
							 NULL,
							 &enable_polar_worker,
							 true,
							 PGC_POSTMASTER,
							 POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGEABLE,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomStringVariable(
							   "polar_worker.core_file_path",
							   "path of core file",
							   "path of core file",
							   &polar_core_file_path,
							   ".",
							   PGC_SIGHUP,
							   POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGEABLE,
							   NULL,
							   NULL,
							   NULL
		);

	DefineCustomStringVariable(
							   "polar_worker.core_name_suffix",
							   "To assign corefile name.",
							   "To assign corefile name.",
							   &core_name_suffix,
							   "core",
							   PGC_SIGHUP,
							   POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGEABLE,
							   NULL,
							   NULL,
							   NULL
		);
	DefineCustomIntVariable(
							"polar_worker.core_file_outdate_time",
							"outdate time of core file.",
							NULL,
							&core_file_outdate_time,
							-1,
							-1, INT_MAX / 1000,
							PGC_SIGHUP,
							POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGEABLE,
							NULL,
							NULL,
							NULL
		);
	DefineCustomIntVariable(
							"polar_worker.num_corefile_reserved_old",
							"num of oldest reserverd corefile.",
							NULL,
							&num_corefile_reserved_old,
							32,
							0, INT_MAX / 1000,
							PGC_SIGHUP,
							POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGEABLE,
							NULL,
							NULL,
							NULL
		);
	DefineCustomIntVariable(
							"polar_worker.num_corefile_reserved_new",
							"num of newest reserverd corefile.",
							NULL,
							&num_corefile_reserved_new,
							32,
							0, INT_MAX / 1000,
							PGC_SIGHUP,
							POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGEABLE,
							NULL,
							NULL,
							NULL
		);
	DefineCustomIntVariable(
							"polar_worker.xlog_temp_outdate_time",
							"outdate time of xlog temp.",
							NULL,
							&xlog_temp_outdate_time,
							-1,
							-1, INT_MAX / 1000,
							PGC_SIGHUP,
							POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGEABLE,
							NULL,
							NULL,
							NULL
		);
	EmitWarningsOnPlaceholders("polar_worker");

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = polar_worker_shmem_request;

	/* Register polar worker. */
	start_polar_worker();
}

/*
 * Requests any additional shared memory required for polar worker.
 */
static void
polar_worker_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(MAXALIGN(sizeof(polar_worker_shm)));
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	/* Uninstall hooks. */
	shmem_request_hook = prev_shmem_request_hook;
}

/*
 * shmem init: allocate and initialize polar worker shared memory,
 */
static void
polar_worker_shmem_init(void)
{
	bool		found = false;

	/*
	 * Create or attach to the shared memory state
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	worker_shm = ShmemInitStruct("polar_worker_shm", sizeof(polar_worker_shm), &found);

	if (!found)
	{
		/* First time through ... */
		memset(worker_shm, 0, sizeof(polar_worker_shm));

		LWLockInitialize(&worker_shm->lock, LWLockNewTrancheId());
	}

	LWLockRelease(AddinShmemInitLock);

	LWLockRegisterTranche(worker_shm->lock.tranche, "polar worker");
}

/*
 * Start autoprewarm master worker process.
 */
static void
start_polar_worker(void)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status;
	pid_t		pid;

	MemSet(&worker, 0, sizeof(BackgroundWorker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_PostmasterStart;
	worker.bgw_restart_time = 30;
	strcpy(worker.bgw_library_name, "polar_worker");
	strcpy(worker.bgw_function_name, "polar_worker_handler_main");
	strcpy(worker.bgw_name, POLAR_WORKER_PROCESS_NAME);
	strcpy(worker.bgw_type, POLAR_WORKER_PROCESS_NAME);

	if (process_shared_preload_libraries_in_progress)
	{
		RegisterBackgroundWorker(&worker);
		return;
	}

	/* must set notify PID to wait for startup */
	worker.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not register background process"),
				 errhint("You may need to increase max_worker_processes.")));

	status = WaitForBackgroundWorkerStartup(handle, &pid);

	if (status != BGWH_STARTED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not start background process"),
				 errhint("More details may be available in the server log.")));
}

/*
 * Preallocate wal files beyond last redolog endpoint.
 */
static void
prealloc_wal_files(void)
{
	XLogRecPtr	insert_ptr = GetXLogInsertRecPtr();
	XLogSegNo	_log_seg_no;
	char		path[MAXPGPATH];
	int			lf;
	bool		added;
	int			count = 0;

	if (!IsInstallXLogFileSegmentActive())
		return;					/* unlocked check says no */

	XLByteToPrevSeg(insert_ptr, _log_seg_no, wal_segment_size);
	while (count < prealloc_wal_file_num)
	{
		_log_seg_no++;
		count++;
		lf = XLogFileInitInternal(_log_seg_no, GetWALInsertionTimeLine(), &added, path);
		if (lf >= 0)
			polar_close(lf);
		if (added)
			CheckpointStats.ckpt_segs_added++;
	}
}

/*
 * Main entry point for polar worker process.
 */
void
polar_worker_handler_main(Datum main_arg)
{
	TimestampTz last_time = GetCurrentTimestamp();

	/* Establish signal handlers; once that's done, unblock signals. */
	pqsignal(SIGTERM, polar_worker_sigterm_handler);
	pqsignal(SIGHUP, polar_worker_sighup_handler);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, polar_worker_sigusr2_handler);

	/* ignore the child signal in polar_worker */
	pqsignal(SIGCHLD, SIG_IGN);

	BackgroundWorkerUnblockSignals();

	/*
	 * POLAR: Add polar worker into backends for showing them in
	 * pg_stat_activity, and participate in ProcSignal. Do not forget to call
	 * ProcessProcSignalBarrier() in the main loop to process Barriers. Now we
	 * use CHECK_FOR_INTERRUPTS() to process Barriers.
	 */
	polar_init_dynamic_bgworker_in_backends();

	polar_worker_shmem_init();

	/*
	 * POLAR: if some process coredump happens, polar worder will be reset
	 * too, so do it at the begining.
	 */
	polar_coredump_handler();

	/* Periodically prealloc wal file until terminated. */
	while (!got_sigterm)
	{
		int			rc = 0;

		/* In case of a SIGHUP, just reload the configuration. */
		if (got_sighup)
		{
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * check for any other interesting events that happened while we
		 * slept.
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * POLAR: To ensure effective synchronization of dynamic shared
		 * memory, a Barriers synchronization mechanism has been implemented.
		 * We put CHECK_FOR_INTERRUPTS() in process's main loop to process
		 * Barriers.
		 */
		CHECK_FOR_INTERRUPTS();

		if (!enable_polar_worker)
		{
			/* We don't want to prealloc wal file now, so just wait forever. */
			rc = WaitLatch(&MyProc->procLatch,
						   WL_LATCH_SET | WL_POSTMASTER_DEATH,
						   -1L,
						   PG_WAIT_EXTENSION);
		}
		/* POLAR: in replica mode, polar worker only do fullpage snapshot work */
		else
		{
			long		delay_in_ms = 1000;

			if (POLAR_LOGINDEX_FULLPAGE_CTL_EXIST() && polar_is_replica())
			{
				polar_bgworker_fullpage_snapshot_replay(polar_logindex_redo_instance->fullpage_ctl);

				if (polar_enable_fullpage_snapshot)
					polar_fullpage_bgworker_wait_notify(polar_logindex_redo_instance->fullpage_ctl);
			}

			/* we only calculate compression ratio on primary */
			if (polar_is_primary() && calculate_compression_ratio && worker_shm->current_arg_is_valid)
			{
				static TimestampTz prev_ts = 0;
				static XLogRecPtr prev_lsn = InvalidXLogRecPtr;
				TimestampTz current_ts = GetCurrentTimestamp();
				XLogRecPtr	current_lsn = polar_max_valid_lsn();

				LWLockAcquire(&worker_shm->lock, LW_EXCLUSIVE);

				/*
				 * update compression ratio when timeouts exceeds interval and
				 * LSN diff exceeds 1GB
				 */
				if (!polar_compression_calculate_arg_equals(&worker_shm->prev_arg, &worker_shm->current_arg) ||
					(TimestampDifferenceExceeds(prev_ts, GetCurrentTimestamp(), calculate_compression_ratio_interval * 1000) &&
					 (current_lsn - prev_lsn) >= 1024 * 1024 * 1024))
				{
					compression_calculate_arg arg = worker_shm->current_arg;
					compression_info info;

					LWLockRelease(&worker_shm->lock);

					info = polar_calculate_compression_info(&arg);

					LWLockAcquire(&worker_shm->lock, LW_EXCLUSIVE);

					worker_shm->info = info;
					worker_shm->prev_arg = arg;
					prev_ts = current_ts;
					prev_lsn = current_lsn;
				}

				LWLockRelease(&worker_shm->lock);
			}

			/* POLAR: coredump/wal prefetch only in master/standby take affect */
			if (!(polar_is_replica() && polar_enable_shared_storage_mode))
			{
				TimestampTz next_time = 0;
				long		secs = 0;
				int			usecs = 0;


				/* Compute the next prealloc wal file time. */
				next_time =
					TimestampTzPlusMilliseconds(last_time,
												polar_worker_check_interval * 1000);
				TimestampDifference(GetCurrentTimestamp(), next_time,
									&secs, &usecs);
				delay_in_ms = secs * 1000 + (usecs / 1000);

				/* If we are in Recovery mode, sleep util consistant */
				if (RecoveryInProgress())
					delay_in_ms = polar_worker_check_interval * 1000;
				else if (delay_in_ms <= 0)
				{
					last_time = GetCurrentTimestamp();

					/*
					 * Perform a prealloc wal and fullpage file operation if
					 * it's time.
					 */
					if (!RecoveryInProgress())
					{
						prealloc_wal_files();

						if (POLAR_LOGINDEX_ENABLE_FULLPAGE())
							polar_prealloc_fullpage_files(polar_logindex_redo_instance->fullpage_ctl);
					}

					/* Perform xlog temp file clean */
					if (xlog_temp_outdate_time >= 0)
						polar_xlog_temp_file_clean();

					continue;
				}
			}

			rc = WaitLatch(&MyProc->procLatch,
						   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
						   delay_in_ms,
						   PG_WAIT_EXTENSION);
		}

		/* Reset the latch, bail out if postmaster died, otherwise loop. */
		ResetLatch(&MyProc->procLatch);

		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);
	}

	/*
	 * Don't allow to exit in signal handler,maybe process is IO pending now,
	 * file cannot close normally, so exit here for safety
	 */
	if (got_sigterm)
	{
		/*
		 * From here on, elog(ERROR) should end with exit(1), not send control
		 * back to the sigsetjmp block above
		 */
		ExitOnAnyError = true;
		proc_exit(0);
	}
}

/*
 * Signal handler for SIGTERM
 */
static void
polar_worker_sigterm_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sigterm = true;

	if (MyProc)
		SetLatch(&MyProc->procLatch);

	SignalHandlerForShutdownRequest(postgres_signal_arg);

	errno = save_errno;
}

/*
 * Signal handler for SIGHUP
 */
static void
polar_worker_sighup_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sighup = true;
	ConfigReloadPending = true;

	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

/*
 * Signal handler for SIGUSR2
 */
static void
polar_worker_sigusr2_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	polar_fullpage_set_online_promote(true);

	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

/*
 * POLAR: handler for coredump
 */
static void
polar_coredump_handler()
{
	/*
	 * POLAR: process_coredump_info: clean outdate core file
	 */
	if (polar_enable_coredump_cleanup)
		polar_remove_old_core_files();
}

/*
 * POLAR: remove old system core files
 * These files are ordered by create-time.
 */
static void
polar_remove_old_core_files(void)
{
	struct dirent *corede;
	struct stat st;
	polar_file_descriptor *file_set;
	char		current_core_path[MAXPGPATH] = {0};
	char		core_file_path[MAXPGPATH] = {0};
	DIR		   *coredir;
	int			num_core_files = 0;
	int			num_alloc_files = POLAR_NUM_FILE_INTERVAL;
	time_t		timestamp;

	/* get the core-file path from the system core-pattern file */
	if (!polar_read_core_pattern(POLAR_CORE_DUMP_PATTERN_FILE, core_file_path))
		snprintf(core_file_path, MAXPGPATH, "%s", polar_core_file_path);

	/* traverse the core file directory */
	time(&timestamp);
	file_set = (polar_file_descriptor *) palloc(num_alloc_files * sizeof(polar_file_descriptor));
	coredir = AllocateDir(core_file_path);

	while ((corede = ReadDir(coredir, core_file_path)) != NULL)
	{
		snprintf(current_core_path, MAXPGPATH, "%s/%s", core_file_path, corede->d_name);

		/* Skip special stuff */
		if (strcmp(corede->d_name, ".") == 0 || strcmp(corede->d_name, "..") == 0)
			continue;

		/* skip file could not open valid */
		if (stat(current_core_path, &st))
			continue;

		/* skip file not belong to itself */
		if (st.st_uid != geteuid())
			continue;

		/* skip dir */
		if (S_ISDIR(st.st_mode))
			continue;

		/* skip file name invalid */
		if (strncmp(corede->d_name, core_name_suffix, strlen(core_name_suffix)))
			continue;

		/* clean file when the file is out of date */
		if (core_file_outdate_time >= 0 && timestamp - st.st_mtime >= core_file_outdate_time)
		{
			elog(DEBUG2, "attempting to remove core file %s", current_core_path);
			polar_remove_file(current_core_path);
		}

		/* expand the file set when necessary */
		if (num_core_files >= num_alloc_files)
		{
			num_alloc_files += POLAR_NUM_FILE_INTERVAL;
			file_set = (polar_file_descriptor *) repalloc(file_set,
														  num_alloc_files * sizeof(polar_file_descriptor));
		}

		/* save [m_time, file_name] to file_set */
		file_set[num_core_files].mtime = st.st_mtime;
		strncpy(file_set[num_core_files].file_path, current_core_path, MAXPGPATH);
		num_core_files++;
	}

	FreeDir(coredir);

	if (num_core_files > 0)
	{
		int			i = 0;

		qsort(file_set, num_core_files, sizeof(polar_file_descriptor), cmp_file_mtime);

		for (i = 0; i < num_core_files; i++)
		{
			if (i >= num_corefile_reserved_old &&
				i < num_core_files - num_corefile_reserved_new)
			{
				elog(DEBUG2, "attempting to remove core file %s", file_set[i].file_path);
				polar_remove_file(file_set[i].file_path);
			}
		}
	}

	pfree(file_set);
}

/*
 * POLAR: remove file function
 * The fucntion is same as drop of replication slot on disk.
 */
static bool
polar_remove_file(const char *path)
{
	int			rc = 0;

#ifdef WIN32
	char		newpath[MAXPGPATH] = {0};

	snprintf(newpath, MAXPGPATH, "%s.deleted", path);

	if (rename(path, newpath) != 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename deleted file \"%s\": %m",
						path)));
		return;
	}

	rc = unlink(newpath);
#else
	rc = unlink(path);
#endif

	if (rc != 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not remove old file \"%s\": %m",
						path)));
		return false;
	}

	return true;
}

/*
 * Compare mtime of file in order to sort array in descending order.
 */
static int
cmp_file_mtime(const void *a, const void *b)
{
	polar_file_descriptor st1 = *((const polar_file_descriptor *) a);
	polar_file_descriptor st2 = *((const polar_file_descriptor *) b);

	if (st1.mtime < st2.mtime)
		return -1;
	else if (st1.mtime == st2.mtime)
		return 0;
	else
		return 1;
}

/*
 * POLAR: remove xlog temp file if it is outdate.
 */
static void
polar_xlog_temp_file_clean(void)
{
	DIR		   *waldir;
	struct dirent *walde;
	struct stat st;
	char		current_xlog_temp_file_path[MAXPGPATH] = {0};
	char		current_dir[MAXPGPATH] = {0};
	time_t		timestamp;
	static time_t last_clean_timestamp;

	time(&timestamp);

	/* to scan disk periodically */
	if (last_clean_timestamp && timestamp - last_clean_timestamp < xlog_temp_outdate_time)
		return;

	last_clean_timestamp = timestamp;

	/* search xlog info in the PG WAL DIR */
	POLAR_FILE_PATH(current_dir, "pg_wal");
	waldir = AllocateDir(current_dir);

	while ((walde = polar_readdir(waldir)) != NULL)
	{
		snprintf(current_xlog_temp_file_path, MAXPGPATH, "%s/%s", current_dir, walde->d_name);

		/* Skip special stuff */
		if (strcmp(walde->d_name, ".") == 0 || strcmp(walde->d_name, "..") == 0)
			continue;

		/* skip file could not open valid */
		if (stat(current_xlog_temp_file_path, &st))
			continue;

		/* skip file not belong to itself */
		if (st.st_uid != geteuid())
			continue;

		/* skip dir */
		if (S_ISDIR(st.st_mode))
			continue;

		/* skip file name invalid */
		if (strncmp(walde->d_name, POLAR_XLOG_TEMP_FILE_SUFFIX, strlen(POLAR_XLOG_TEMP_FILE_SUFFIX)))
			continue;

		/* delete file if it is outdate */
		if (timestamp - st.st_mtime >= xlog_temp_outdate_time)
			polar_unlink(current_xlog_temp_file_path);
	}

	FreeDir(waldir);
}

/*
 * POLAR: Get the core-file path from the system core-pattern file
 */
bool
polar_read_core_pattern(const char *core_pattern_path, char *core_file_path)
{
	int			fd = 0;
	char	   *ret = NULL;
	struct stat st;
	char		cwd[MAXPGPATH] = {0};
	char		buf[MAXPGPATH] = {0};

	/* open system file and read core pattern path */
	fd = open(core_pattern_path, O_RDONLY | PG_BINARY);

	if (fd < 0)
	{
		ereport(LOG, (errcode_for_file_access(),
					  errmsg("could not open file \"%s\".", core_pattern_path)));
		return false;
	}

	if (read(fd, (char *) buf, sizeof(buf)) < 0)
	{
		ereport(LOG, (errcode_for_file_access(),
					  errmsg("could not read file \"%s\".", core_pattern_path)));
		close(fd);
		return false;
	}

	close(fd);

	/* get the current working path */
	if (!getcwd(cwd, MAXPGPATH))
	{
		elog(LOG, "could not determine current directory");
		return false;
	}

	/* get the rightmost / if possible */
	ret = strrchr(buf, '/');

	/*
	 * first case: / did not exsit in path (e.g. core) the practical path is
	 * the current working path
	 */
	if (ret == NULL)
	{
		snprintf(core_file_path, MAXPGPATH, "%s", cwd);
		return true;
	}

	/* get the core directory from the core pattern */
	if (ret - buf + 1 > MAXPGPATH)
	{
		elog(LOG, "core pattern length exceeds MAXPGPATH");
		return false;
	}

	buf[ret - buf + 1] = '\0';

	/* second case: absolute path (e.g. /tmp/corefile/core) */
	if (buf[0] == '/')
		snprintf(core_file_path, MAXPGPATH, "%s", buf);
	else
	{
		/* third case: relative path (e.g. corefile/core) */
		snprintf(core_file_path, MAXPGPATH, "%s/%s", cwd, buf);
	}

	/* judge whether core_file_path could be opened valid */
	if (stat(core_file_path, &st))
		return false;

	return true;
}

static bool
polar_compression_calculate_arg_equals(compression_calculate_arg * arg1,
									   compression_calculate_arg * arg2)
{
	return strcmp(arg1->sub_directory, arg2->sub_directory) == 0 &&
		strcasecmp(arg1->compression_method, arg2->compression_method) == 0 &&
		arg1->unit_size == arg2->unit_size &&
		arg1->unit_count == arg2->unit_count &&
		arg1->step_factor == arg2->step_factor &&
		arg1->fit_nblocks == arg2->fit_nblocks &&
		arg1->lowerbound == arg2->lowerbound &&
		arg1->extra_clen == arg2->extra_clen &&
		arg1->extra_slen == arg2->extra_slen &&
		arg1->ignore_size == arg2->ignore_size &&
		arg1->ignore_catalog_heap == arg2->ignore_catalog_heap &&
		arg1->ignore_extended_heap == arg2->ignore_extended_heap;
}

static int64
polar_calculate_unit_compression_size(compression_calculate_arg * arg,
									  void *source, int slen,
									  void *dest, int dlen)
{
	int			clen;

	if (strcasecmp(arg->compression_method, "lz4") == 0)
	{
#ifdef USE_LZ4
		clen = LZ4_compress_default(source, dest, slen, dlen);
		if (clen <= 0)
			clen = slen;
#else
		NO_LZ4_SUPPORT();
#endif
	}
	else if (strcasecmp(arg->compression_method, "zstd") == 0)
	{
#ifdef USE_ZSTD
		clen = ZSTD_compress(dest, dlen, source, slen, ZSTD_CLEVEL_DEFAULT);
		if (ZSTD_isError(clen))
			clen = slen;
#else
		NO_ZSTD_SUPPORT();
#endif
	}
	else if (strcasecmp(arg->compression_method, "pglz") == 0)
	{
		clen = pglz_compress(source, slen, dest, PGLZ_strategy_default);
		if (clen <= 0)
			clen = slen;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("compression method %s not supported", arg->compression_method)));

	if (arg->fit_nblocks > 0)
	{
		int			ratio = slen / clen;

		if (ratio == 0)
			clen = slen;
		else if (ratio > arg->fit_nblocks)
			clen = slen / arg->fit_nblocks;
		else
			clen = slen / ratio;
	}

	return clen;
}

#define BYTES_PER_SEGMENT ((int64) BLCKSZ * RELSEG_SIZE)

static compression_info
polar_calculate_file_compression_info(char *path, int64 size, Oid dboid, Oid oid,
									  compression_calculate_arg * arg)
{
#define STEP_SIZE (1024 * 1024)
	off_t		step = 0;
	compression_info info = {0};
	int			fd = polar_open(path, O_RDONLY, 0);
	int			chunk_size = arg->unit_size * arg->unit_count;

	if (fd < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

		goto ret;
	}

	while ((step * STEP_SIZE + chunk_size) < size)
	{
		CHECK_FOR_INTERRUPTS();

		if (polar_pread(fd, arg->chunk_buffer, chunk_size, step * STEP_SIZE) != chunk_size)
		{
			info.clen = info.slen = size;
			break;
		}

		for (int i = 0; i < arg->unit_count; i++)
		{
			int			clen =
				polar_calculate_unit_compression_size(arg,
													  (char *) arg->chunk_buffer + i * arg->unit_size,
													  arg->unit_size,
													  arg->compression_buffer,
													  COMPRESS_BUFSIZE(arg->unit_size));

			info.clen += clen;
			info.slen += arg->unit_size;
		}

		step *= arg->step_factor;
		if (step == 0)
			step = arg->step_factor;
	}

	polar_close(fd);

ret:
	if (info.slen == 0)
		info.slen = info.clen = size;
	else
	{
		info.clen = size * info.clen / info.slen;
		info.slen = size;
	}

	if (arg->ignore_extended_heap && OidIsValid(dboid) && OidIsValid(oid))
	{
		bool		found;
		compression_info_entry *entry;
		int64		identyfier = oid + ((int64) dboid << 32);

		entry = hash_search(arg->file_table, &identyfier, HASH_ENTER, &found);

		if (found)
		{
			Assert(entry->info.clen == BYTES_PER_SEGMENT);
			Assert(entry->info.slen == BYTES_PER_SEGMENT);
		}
		else
			entry->segments = 1;

		entry->info = info;
		info.clen = info.slen = 0;
	}

	return info;
}

static compression_info
polar_calculate_directory_compression_info(char *path, compression_calculate_arg * arg)
{
	compression_info total_info = {0};
	struct dirent *direntry;
	DIR		   *dirdesc;
	Oid			dboid = InvalidOid;
	Oid			oid = InvalidOid;
	char	   *base;

	if ((base = strstr(path, "base/")))
	{
		if (sscanf(base, "base/%d", &dboid) != 1)
			dboid = InvalidOid;
	}

	dirdesc = AllocateDir(path);

	while ((direntry = ReadDir(dirdesc, path)) != NULL)
	{
		struct stat fst;
		char		entry_path[MAXPGPATH];
		compression_info info;

		CHECK_FOR_INTERRUPTS();

		if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
			continue;

		if (strstr(path, "base/"))
		{
			char	   *endptr;
			bool		pure_oid = false;

			oid = strtou64(direntry->d_name, &endptr, 10);

			if (!endptr)
				oid = InvalidOid;
			else if (*endptr == '\0')
				pure_oid = true;

			if (arg->ignore_catalog_heap && OidIsValid(dboid) &&
				pure_oid && OidIsValid(oid) && oid < FirstNormalObjectId)
				continue;

			if (arg->ignore_extended_heap)
			{
				if (strstr(direntry->d_name, "_fsm") ||
					strstr(direntry->d_name, "_vm"))
					continue;

				if (strstr(direntry->d_name, ".") && OidIsValid(dboid))
				{
					int64		identyfier;
					int64		relid;
					int64		segment;
					bool		found;
					int			n;
					compression_info_entry *entry;

					n = sscanf(direntry->d_name, "%ld.%ld", &relid, &segment);

					if (n != 2)
						continue;

					segment++;
					identyfier = relid + ((int64) dboid << 32);

					entry = hash_search(arg->file_table, &identyfier, HASH_ENTER, &found);

					if (found)
					{
						if (entry->segments < segment)
							entry->segments = segment;
					}
					else
					{
						entry->segments = segment;
						entry->info.clen = entry->info.slen = BYTES_PER_SEGMENT;
					}

					continue;
				}

				if (!pure_oid)
					continue;
			}
		}

		snprintf(entry_path, MAXPGPATH, "%s/%s", path, direntry->d_name);

		if (polar_stat(entry_path, &fst) < 0)
		{
			if (errno == ENOENT)
				continue;
			else
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", entry_path)));
		}

		if (S_ISDIR(fst.st_mode))
			info = polar_calculate_directory_compression_info(entry_path, arg);
		else if (fst.st_size <= arg->ignore_size)
			info.clen = info.slen = fst.st_size;
		else
			info = polar_calculate_file_compression_info(entry_path, fst.st_size, dboid, oid, arg);

		total_info.clen += info.clen;
		total_info.slen += info.slen;
	}
	FreeDir(dirdesc);

	return total_info;
}

static compression_info
polar_calculate_compression_info(compression_calculate_arg * arg)
{
	compression_info info = {0};
	HASHCTL		hash_ctl;
	char		path[MAXPGPATH];
	HASH_SEQ_STATUS status;
	compression_info_entry *entry;

	if (arg->unit_size < 1024 || arg->unit_size > 1048576)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"unit_size\" should be in [1024, 1048576]")));

	if (arg->unit_count < 1 || arg->unit_count > 1024)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"unit_count\" should be in [1, 1024]")));

	if (arg->step_factor < 2 || arg->step_factor > 512)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"step_factor\" should be in [2, 512]")));

	if (arg->fit_nblocks < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"fit_nblocks\" should be in [0, inf)")));

	if (arg->lowerbound < 0 || arg->lowerbound > 100)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"lowerbound\" should be in [0, 100]")));

	if (arg->extra_clen < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"extra_clen\" should be in [0, inf)")));

	if (arg->extra_slen < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"extra_slen\" should be in [0, inf)")));

	arg->chunk_buffer = (char *) palloc(arg->unit_size * arg->unit_count);
	arg->compression_buffer = (char *) palloc(COMPRESS_BUFSIZE(arg->unit_size));
	hash_ctl.keysize = sizeof(int64);
	hash_ctl.entrysize = sizeof(compression_info_entry);
	arg->file_table = hash_create("file table", 16, &hash_ctl, HASH_ELEM | HASH_BLOBS);

	snprintf(path, MAXPGPATH, "%s/%s", POLAR_DATA_DIR(), arg->sub_directory);
	info = polar_calculate_directory_compression_info(path, arg);

	hash_seq_init(&status, arg->file_table);
	while ((entry = (compression_info_entry *) hash_seq_search(&status)) != NULL)
	{
		if (entry->segments == 1)
		{
			info.clen += entry->info.clen;
			info.slen += entry->info.slen;
		}
		else
		{
			info.clen += (float) entry->info.clen / entry->info.slen * BYTES_PER_SEGMENT * entry->segments;
			info.slen += BYTES_PER_SEGMENT * entry->segments;
		}
	}

	pfree(arg->chunk_buffer);
	pfree(arg->compression_buffer);
	hash_destroy(arg->file_table);

	info.clen += arg->extra_clen;
	info.slen += arg->extra_slen;

	return info;
}

PG_FUNCTION_INFO_V1(polar_compression_info);
Datum
polar_compression_info(PG_FUNCTION_ARGS)
{
#define NUM_COMPRESSION_INFO_COLS	3
	TupleDesc	tupdesc;
	Datum		values[NUM_COMPRESSION_INFO_COLS];
	bool		nulls[NUM_COMPRESSION_INFO_COLS];
	compression_calculate_arg arg;
	compression_info info = {0};
	char	   *sub_directory = PG_ARGISNULL(0) ? "base" : text_to_cstring(PG_GETARG_TEXT_P(0));
	char	   *compression_method = PG_ARGISNULL(1) ? "lz4" : text_to_cstring(PG_GETARG_TEXT_P(1));
	float8		compression_ratio;

	strcpy(arg.sub_directory, sub_directory);
	strcpy(arg.compression_method, compression_method);
	arg.unit_size = PG_GETARG_INT64(2);
	arg.unit_count = PG_GETARG_INT64(3);
	arg.step_factor = PG_GETARG_INT64(4);
	arg.fit_nblocks = PG_GETARG_INT64(5);
	arg.lowerbound = PG_GETARG_INT64(6);
	arg.extra_clen = PG_GETARG_INT64(7);
	arg.extra_slen = PG_GETARG_INT64(8);
	arg.ignore_size = PG_GETARG_INT64(9);
	arg.ignore_catalog_heap = PG_GETARG_BOOL(10);
	arg.ignore_extended_heap = PG_GETARG_BOOL(11);

	if (!superuser())
		elog(ERROR, "Only superuser can execute polar_compression_info function");

	polar_worker_shmem_init();

	if (calculate_compression_ratio && !worker_shm)
		info = polar_calculate_compression_info(&arg);
	else if (calculate_compression_ratio)
	{
		LWLockAcquire(&worker_shm->lock, LW_EXCLUSIVE);
		if (polar_compression_calculate_arg_equals(&arg, &worker_shm->prev_arg))
		{
			info = worker_shm->info;
			LWLockRelease(&worker_shm->lock);
		}
		else
		{
			worker_shm->current_arg = arg;
			worker_shm->current_arg_is_valid = true;
			LWLockRelease(&worker_shm->lock);
			info = polar_calculate_compression_info(&arg);
		}
	}

	if (info.slen <= 0 || info.clen <= 0 || info.clen > info.slen)
		compression_ratio = 1;
	else if ((100 * info.clen / info.slen) < arg.lowerbound)
		compression_ratio = (double) arg.lowerbound / 100;
	else
		compression_ratio = (float8) info.clen / info.slen;

	tupdesc = CreateTemplateTupleDesc(NUM_COMPRESSION_INFO_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "original_size", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "compression_size", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "compression_ratio", FLOAT8OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	MemSet(nulls, 0, sizeof(nulls));

	values[0] = Int64GetDatum(info.slen);
	values[1] = Int64GetDatum(info.clen);
	values[2] = Float8GetDatum(compression_ratio);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
