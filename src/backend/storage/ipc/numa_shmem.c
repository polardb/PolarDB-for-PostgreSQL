/*-------------------------------------------------------------------------
 *
 * numa_shmem.c
 *	  Apply a NUMA memory policy to PostgreSQL's main shared memory segment,
 *	  so that physically-backed pages are spread across NUMA nodes instead
 *	  of being concentrated on the postmaster's local node (the
 *	  "first-touch" problem that can exhaust a single node's memory and
 *	  trigger OOM while other nodes stay idle).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/numa_shmem.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "storage/pg_numa.h"
#include "utils/elog.h"
#include "utils/guc.h"

#ifdef HAVE_LIBNUMA
#include <numa.h>
#include <numaif.h>
#endif

char	   *NumaShmem = "off";

/*
 * Validate the "numa" GUC.  Non-"off" values require libnuma at build time;
 * without it we reject anything but "off" so the operator gets a clear error
 * instead of a silent no-op.
 */
bool
check_numa_shmem(char **newval, void **extra, GucSource source)
{
#ifdef HAVE_LIBNUMA
	if ((*newval)[0] == '@' || (*newval)[0] == '=')
	{
		struct bitmask *mask = numa_parse_nodestring(*newval + 1);

		if (mask == NULL)
		{
			GUC_check_errdetail("\"%s\" is not a valid NUMA node list.",
								*newval + 1);
			return false;
		}
		numa_bitmask_free(mask);
		return true;
	}
#else
	if (strcmp(*newval, "off") != 0)
	{
		GUC_check_errdetail("NUMA support is not compiled into this server (libnuma missing).");
		return false;
	}
#endif

	if (strcmp(*newval, "off") != 0 && strcmp(*newval, "all") != 0)
	{
		GUC_check_errdetail("\"numa\" must be \"off\", \"all\", or start with '@' or '=' followed by a NUMA node list.");
		return false;
	}
	return true;
}

/*
 * Apply the configured NUMA memory policy to the main shared memory segment
 * [addr, addr+size).  Called from the postmaster, before the segment is first
 * touched, so that demand-paged faults are placed according to the policy.
 */
void
pg_numa_apply_shmem_policy(void *addr, Size size)
{
#ifndef HAVE_LIBNUMA
	if (strcmp(NumaShmem, "off") != 0)
		elog(WARNING, "numa = \"%s\" is ignored: this server was built without NUMA support",
			 NumaShmem);
	return;
#else
	long		pagesize = sysconf(_SC_PAGESIZE);
	size_t		aligned_size;

	if (pagesize <= 0)
		pagesize = 4096;
	aligned_size = (size / (Size) pagesize) * (Size) pagesize;
	if (aligned_size == 0)
		return;

	if (strcmp(NumaShmem, "off") == 0)
		return;

	if (numa_available() < 0)
	{
		elog(WARNING, "numa = \"%s\" is ignored: libnuma reports no NUMA support on this system",
			 NumaShmem);
		return;
	}

	if (strcmp(NumaShmem, "all") == 0)
	{
		numa_interleave_memory(addr, aligned_size, numa_all_nodes_ptr);
	}
	else if (NumaShmem[0] == '@')
	{
		struct bitmask *mask = numa_parse_nodestring(NumaShmem + 1);

		if (mask == NULL)
		{
			elog(WARNING, "numa = \"%s\" ignored: invalid NUMA node list", NumaShmem);
			return;
		}
		mbind(addr, aligned_size, MPOL_BIND, mask->maskp, mask->size, 0);
		numa_bitmask_free(mask);
	}
	else if (NumaShmem[0] == '=')
	{
		struct bitmask *mask = numa_parse_nodestring(NumaShmem + 1);

		if (mask == NULL)
		{
			elog(WARNING, "numa = \"%s\" ignored: invalid NUMA node list", NumaShmem);
			return;
		}
		mbind(addr, aligned_size, MPOL_PREFERRED, mask->maskp, mask->size, 0);
		numa_bitmask_free(mask);
	}
#endif							/* HAVE_LIBNUMA */
}
