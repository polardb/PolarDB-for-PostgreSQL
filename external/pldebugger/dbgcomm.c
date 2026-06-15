/**********************************************************************
 * dbgcomm.c
 *
 * This file contains some helper functions for the proxy - target
 * communication.
 * Copyright (c) 2004-2024 EnterpriseDB Corporation. All Rights Reserved.
 *
 * Licensed under the Artistic License v2.0, see
 *		https://opensource.org/licenses/artistic-license-2.0
 * for full details
 *
 **********************************************************************/

#include "postgres.h"

#include <unistd.h>
#include <netdb.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "miscadmin.h"
#if (PG_VERSION_NUM < 170000)
#include "storage/backendid.h"
#endif
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
#include "storage/sinvaladt.h"

#include "dbgcomm.h"
#include "pldebugger.h"

#if (PG_VERSION_NUM < 90200)
/* before 9.2, PostmasterIsAlive() had one parameter */
#define PostmasterIsAlive()	PostmasterIsAlive(false)
#endif

/*
 * Shared memory structure. This is used for authenticating debugger
 * connections. Each backend has a dedicated slot.
 *
 * Whenever a target backend is waiting for a proxy to connect to it
 * (pldbg_oid_debug()), or trying to connect to a proxy (when it hits a
 * global breakpoint), it advertises the connection attempt in shared memory.
 * Each backend has a slot of its own.
 *
 * When the proxy initiates the connection and target backend listens
 * (pldbg_oid_debug()), the backend first sets its status to
 * LISTENING_FOR_PROXY, and the port it's listening on in 'port'. When the
 * proxy wants to connect to it, it changes the status to PROXY_CONNECTING
 * and sets the port to the port it's connecting from. When the target backend
 * accept()s the connection, it checks that the remote port of the connection
 * matches the one in the slot. This makes the communication secure, because
 * only a legitimate proxy backend can access shared memory.
 *
 * Target backend connecting to a proxy (when a global breakpoint is hit) works
 * similarly, except that the LISTENING step is not needed. The backend sets
 * the port it's connecting from in its slot's port field, and connects.
 * The proxy accept()s the connection, and scans all the slots for a match
 * on the port number the connection came from. If it finds the port number
 * in one of the slots, the connection came from a legitimate target backend.
 */
#define DBGCOMM_IDLE				0
#define DBGCOMM_LISTENING_FOR_PROXY	1	/* target is listening for a proxy */
#define DBGCOMM_PROXY_CONNECTING	2	/* proxy is connecting to our port */
#define DBGCOMM_CONNECTING_TO_PROXY	3	/* target is connecting to a proxy */

typedef struct
{
	BackendId		backendid;
	int			status;
	int			pid;
	int			port;
} dbgcomm_target_slot_t;

static dbgcomm_target_slot_t *dbgcomm_slots = NULL;

/*
 * Each in-progress connection attempt between proxy and target require
 * a slot. 50 should be plenty.
 */
#define NumTargetSlots 50

/**********************************************************************
 * Prototypes for static functions
 **********************************************************************/
static void dbgcomm_init(void);
static uint32 resolveHostName(const char *hostName);
static int findFreeTargetSlot(void);
static int findTargetSlot(BackendId backendid);

/**********************************************************************
 * Initialization routines
 **********************************************************************/

/*
 * Reserves the right amount of shared memory, when the library is
 * preloaded by shared_preload_libraries.
 */
void
dbgcomm_reserve(void)
{
	RequestAddinShmemSpace(sizeof(dbgcomm_target_slot_t) * NumTargetSlots);
}

/*
 * Initialize slots in shared memory.
 */
static void
dbgcomm_init(void)
{
	bool found;

	if (dbgcomm_slots)
		return;

	LWLockAcquire(getPLDebuggerLock(), LW_EXCLUSIVE);
	dbgcomm_slots = ShmemInitStruct("Debugger Connection slots", sizeof(dbgcomm_target_slot_t) * NumTargetSlots, &found);
	if (dbgcomm_slots == NULL)
		elog(ERROR, "out of shared memory");

	if (!found)
	{
		int i;
		for (i = 0; i < NumTargetSlots; i++)
		{
			dbgcomm_slots[i].backendid = InvalidBackendId;
			dbgcomm_slots[i].status = DBGCOMM_IDLE;
		}
	}
	LWLockRelease(getPLDebuggerLock());
}


/**********************************************************************
 * Routines called by debugging target
 *
 * These routines use ereport(COMMERROR, ...) for errors, so that they
 * are logged but not sent to the client, and don't abort the
 * transaction.
 *
 **********************************************************************/

/*
 * dbcomm_connect_to_proxy
 *
 * This does socket() + connect(), to connect to a listener. The connection
 * is authenticated. Returns the file descriptor of the open socket.
 *
 * We assume that the proxyPort came from a breakpoint or some other reliable
 * source, so that we don't allow connecting to any random port in the system.
 */
int
dbgcomm_connect_to_proxy(int proxyPort)
{
	int			sockfd;
	struct sockaddr_in   remoteaddr = {0};
	struct sockaddr_in   localaddr = {0};
	socklen_t	addrlen 	= sizeof( remoteaddr );
	int			reuse_addr_flag = 1;
	int			slot;

	dbgcomm_init();

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
	{
		ereport(COMMERROR,
				(errcode_for_socket_access(),
				 errmsg("could not create socket for connecting to proxy: %m")));
		return -1;
	}
	/* Sockets seem to be non-blocking by default on Windows.. */
	if (!pg_set_block(sockfd))
	{
		closesocket(sockfd);
		ereport(COMMERROR,
			(errmsg("could not set socket to blocking mode: %m")));
		return -1;
	}

	/*
	 * We have to bind the socket before connecting, so that we know the
	 * local port number it will use. We have to store it in shared memory
	 * before connecting, so that the target knows the connection is legit.
	 */
	localaddr.sin_family      = AF_INET;
	localaddr.sin_port        = htons( 0 );
	localaddr.sin_addr.s_addr = resolveHostName( "127.0.0.1" );

	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
			   (const char *) &reuse_addr_flag, sizeof(reuse_addr_flag));

	if (bind(sockfd, (struct sockaddr *) &localaddr, sizeof(localaddr)) < 0)
	{
		ereport(COMMERROR,
				(errcode_for_socket_access(),
				 errmsg("could not bind local port: %m")));
		closesocket(sockfd);
		return -1;
	}
	/* Get the port number selected by the TCP/IP stack */
	getsockname(sockfd, (struct sockaddr *) &localaddr, &addrlen);

	LWLockAcquire(getPLDebuggerLock(), LW_EXCLUSIVE);
	slot = findFreeTargetSlot();
	if (slot < 0)
	{
		closesocket(sockfd);
		LWLockRelease(getPLDebuggerLock());
		ereport(COMMERROR,
				(errcode_for_socket_access(),
				 errmsg("could not find a free target slot")));
		return -1;
	}
	dbgcomm_slots[slot].port = ntohs(localaddr.sin_port);
	dbgcomm_slots[slot].status = DBGCOMM_CONNECTING_TO_PROXY;
	dbgcomm_slots[slot].backendid = MyBackendId;
	dbgcomm_slots[slot].pid = MyProcPid;
	LWLockRelease(getPLDebuggerLock());

	remoteaddr.sin_family 	   = AF_INET;
	remoteaddr.sin_port        = htons(proxyPort);
	remoteaddr.sin_addr.s_addr = resolveHostName( "127.0.0.1" );

	/* Now connect to the other end. */
	if (connect(sockfd, (struct sockaddr *) &remoteaddr,
				sizeof(remoteaddr)) < 0)
	{
		ereport(COMMERROR,
				(errcode_for_socket_access(),
				 errmsg("could not connect to debugging proxy at port %d: %m", proxyPort)));
		/*
		 * Reset our entry in the array. On success, this will be done by
		 * the proxy.
		 */
		LWLockAcquire(getPLDebuggerLock(), LW_EXCLUSIVE);
		dbgcomm_slots[slot].status = DBGCOMM_IDLE;
		dbgcomm_slots[slot].backendid = InvalidBackendId;
		dbgcomm_slots[slot].port = 0;
		LWLockRelease(getPLDebuggerLock());
		return -1;
	}

	return sockfd;
}

/*
 * dbcomm_connect_to_proxy
 *
 * This does listen() + accept(), to wait for a proxy to connect to us.
 */
int
dbgcomm_listen_for_proxy(void)
{
	struct sockaddr_in   remoteaddr = {0};
	struct sockaddr_in   localaddr = {0};
	socklen_t	addrlen 	= sizeof( remoteaddr );
	int			sockfd;
	int			serverSocket;
	int			localport;
	bool		done;
	int			slot;

	dbgcomm_init();

	sockfd = socket( AF_INET, SOCK_STREAM, 0 );
	if (sockfd < 0)
	{
		ereport(COMMERROR,
				(errcode_for_socket_access(),
				 errmsg("could not create socket for connecting to proxy: %m")));
		return -1;
	}
	/* Sockets seem to be non-blocking by default on Windows.. */
	if (!pg_set_block(sockfd))
	{
		closesocket(sockfd);
		ereport(COMMERROR,
			(errmsg("could not set socket to blocking mode: %m")));
		return -1;
	}

	/* Bind the listener socket to any available port */
	localaddr.sin_family	  = AF_INET;
	localaddr.sin_port		  = htons( 0 );
	localaddr.sin_addr.s_addr = resolveHostName( "127.0.0.1" );
	if (bind( sockfd, (struct sockaddr *) &localaddr, sizeof(localaddr)) < 0)
	{
		ereport(COMMERROR,
				(errcode_for_socket_access(),
				 errmsg("could not bind socket for listening for proxy: %m")));
		closesocket(sockfd);
		return -1;
	}

	/* Get the port number selected by the TCP/IP stack */
	getsockname(sockfd, (struct sockaddr *) &localaddr, &addrlen);
	localport = ntohs(localaddr.sin_port);

	/* Get ready to wait for a client. */
	if (listen(sockfd, 2) < 0)
	{
		ereport(COMMERROR,
				(errcode_for_socket_access(),
				 errmsg("could not listen() for proxy: %m")));
		return -1;
	}

	LWLockAcquire(getPLDebuggerLock(), LW_EXCLUSIVE);
	slot = findFreeTargetSlot();
	if (slot < 0)
	{
		closesocket(sockfd);
		LWLockRelease(getPLDebuggerLock());
		ereport(COMMERROR,
				(errcode_for_socket_access(),
				 errmsg("could not find a free target slot")));
		return -1;
	}
	dbgcomm_slots[slot].port = localport;
	dbgcomm_slots[slot].status = DBGCOMM_LISTENING_FOR_PROXY;
	dbgcomm_slots[slot].backendid = MyBackendId;
	dbgcomm_slots[slot].pid = MyProcPid;
	LWLockRelease(getPLDebuggerLock());

	/* Notify the client application that this backend is waiting for a proxy. */
	elog(NOTICE, "PLDBGBREAK:%d", MyBackendId);

	/* wait for the other end to connect to us */
	done = false;
	while (!done)
	{
		serverSocket = accept(sockfd, (struct sockaddr *) &remoteaddr, &addrlen);
		if (serverSocket < 0)
			ereport(ERROR,
					(errmsg("could not accept connection from debugging proxy")));

		/*
		 * Authenticate the connection. We do this by checking that the remote
		 * end's port number matches what's posted in the shared memory slot.
		 */
		LWLockAcquire(getPLDebuggerLock(), LW_EXCLUSIVE);
		if (dbgcomm_slots[slot].status == DBGCOMM_PROXY_CONNECTING &&
			dbgcomm_slots[slot].port == ntohs(remoteaddr.sin_port))
		{
			dbgcomm_slots[slot].backendid = InvalidBackendId;
			dbgcomm_slots[slot].status = DBGCOMM_IDLE;
			done = true;
		}
		else
			closesocket(serverSocket);
		LWLockRelease(getPLDebuggerLock());
	}

	closesocket(sockfd);

	return serverSocket;
}

/**********************************************************************
 * Routines called by debugging proxy
 **********************************************************************/

/*
 * dbgcomm_connect_to_target
 *
 * Connect to given target backend that's waiting for us. Returns a socket
 * that is open for communication. Uses ereport(ERROR) on error.
 */
int
dbgcomm_connect_to_target(BackendId targetBackend)
{
	int			sockfd;
	struct sockaddr_in   remoteaddr = {0};
	struct sockaddr_in   localaddr = {0};
	socklen_t	addrlen 	= sizeof( remoteaddr );
	int			reuse_addr_flag = 1;
	int			localport;
	int			remoteport;
	int			slot;

	dbgcomm_init();

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
		ereport(ERROR,
				(errcode_for_socket_access(),
				 errmsg("could not create socket for connecting to target: %m")));
	/* Sockets seem to be non-blocking by default on Windows.. */
	if (!pg_set_block(sockfd))
	{
		int save_errno = errno;
		closesocket(sockfd);
		errno = save_errno;
		ereport(ERROR,
				(errmsg("could not set socket to blocking mode: %m")));
	}

	/*
	 * We have to bind the socket before connecting, so that we know the
	 * local port number it will use. We have to store it in shared memory
	 * before connecting, so that the target knows the connection is legit.
	 */
	localaddr.sin_family      = AF_INET;
	localaddr.sin_port        = htons( 0 );
	localaddr.sin_addr.s_addr = resolveHostName( "127.0.0.1" );

	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
			   (const char *) &reuse_addr_flag, sizeof(reuse_addr_flag));

	if (bind(sockfd, (struct sockaddr *) &localaddr, sizeof(localaddr)) < 0)
		elog(ERROR, "pl_debugger: could not bind local port: %m");

	/* Get the port number selected by the TCP/IP stack */
	getsockname(sockfd, (struct sockaddr *) &localaddr, &addrlen);
	localport = ntohs(localaddr.sin_port);

	/*
	 * Find the target backend's slot. Check which port it's listening on, and
	 * let it know we're connecting to it from this port.
	 */
	LWLockAcquire(getPLDebuggerLock(), LW_EXCLUSIVE);
	slot = findTargetSlot(targetBackend);
	if (slot < 0 || dbgcomm_slots[slot].status != DBGCOMM_LISTENING_FOR_PROXY)
	{
		closesocket(sockfd);
		ereport(ERROR,
				(errmsg("target backend is not listening for a connection")));
	}
	remoteport = dbgcomm_slots[slot].port;
	dbgcomm_slots[slot].port = localport;
	dbgcomm_slots[slot].status = DBGCOMM_PROXY_CONNECTING;
	LWLockRelease(getPLDebuggerLock());

	/* Now connect to the other end. */
	remoteaddr.sin_family 	   = AF_INET;
	remoteaddr.sin_port        = htons(remoteport);
	remoteaddr.sin_addr.s_addr = resolveHostName( "127.0.0.1" );
	if (connect(sockfd, (struct sockaddr *) &remoteaddr,
				sizeof(remoteaddr)) < 0)
	{
		ereport(ERROR,
				(errmsg("could not connect to target backend: %m")));
		/* XXX: should we do something about the slot on error? */
	}

	return sockfd;
}

/*
 * dbgcomm_accept_target
 *
 * Waits for one connection from a target backend to the given socket. Returns
 * a socket that is open for communication. Uses ereport(ERROR) on error.
 */
int
dbgcomm_accept_target(int sockfd, int *targetPid)
{
	int			serverSocket;
	int			i;
	struct sockaddr_in remoteaddr = {0};
	socklen_t	addrlen = sizeof(remoteaddr);

	dbgcomm_init();

	/* wait for the target to connect to us */
	for (;;)
	{
		fd_set		rmask;
		int			rc;
		struct timeval timeout;

		/* Check for query cancel or termination request */
		CHECK_FOR_INTERRUPTS();
		if (!PostmasterIsAlive())
		{
			/* Emergency bailout if postmaster has died. */
			ereport(FATAL,
					(errmsg("canceling debugging session because postmaster died")));
		}

		FD_ZERO(&rmask);
		FD_SET(sockfd, &rmask);

		/*
		 * Wake up every 1 second to check if we've been killed or
		 * postmaster has died.
		 */
		timeout.tv_sec  = 1;
		timeout.tv_usec = 0;

		rc = select(sockfd + 1, &rmask, NULL, NULL, &timeout);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			/* anything else is an error */
			ereport(ERROR,
					(errmsg("select() failed while waiting for target: %m")));
		}
		if (rc == 0)
		{
			/* Timeout expired. */
			continue;
		}
		if (!FD_ISSET(sockfd, &rmask))
			continue;

		serverSocket = accept(sockfd, (struct sockaddr *) &remoteaddr, &addrlen);
		if (serverSocket < 0)
			ereport(ERROR,
					(errmsg("could not accept connection from debugging target: %m")));

		/*
		 * Authenticate the connection. We do this by checking that the remote
		 * end's port number is listed in a slot in shared memory.
		 */
		LWLockAcquire(getPLDebuggerLock(), LW_EXCLUSIVE);
		for (i = 0; i < NumTargetSlots; i++)
		{
			if (dbgcomm_slots[i].status == DBGCOMM_CONNECTING_TO_PROXY &&
				dbgcomm_slots[i].port == ntohs(remoteaddr.sin_port))
			{
				*targetPid = dbgcomm_slots[i].pid;
				dbgcomm_slots[i].status = DBGCOMM_IDLE;
				break;
			}
		}
		LWLockRelease(getPLDebuggerLock());
		if (i >= NumTargetSlots)
		{
			/*
			 * This connection did not come from a backend. Reject and continue
			 * listening.
			 */
			closesocket(serverSocket);
			continue;
		}
		else
		{
			/* This looks like a legitimate connection. */
			break;
		}
	}

	return serverSocket;
}


int
dbgcomm_listen_for_target(int *port)
{
	int	 		sockfd;
	struct sockaddr_in 			proxy_addr     	= {0};
	socklen_t					proxy_addr_len 	= sizeof( proxy_addr );
	int							reuse_addr_flag = 1;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
		ereport(ERROR,
				(errcode_for_socket_access(),
				 errmsg("could not create socket: %m")));
	/* Sockets seem to be non-blocking by default on Windows.. */
	if (!pg_set_block(sockfd))
	{
		int save_errno = errno;
		closesocket(sockfd);
		errno = save_errno;
		ereport(ERROR,
				(errmsg("could not set socket to blocking mode: %m")));
	}

	/* Ask the TCP/IP stack for an unused port */
	proxy_addr.sin_family      = AF_INET;
	proxy_addr.sin_port        = htons(0);
	proxy_addr.sin_addr.s_addr = resolveHostName("127.0.0.1");

	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
			   (const char *) &reuse_addr_flag, sizeof( reuse_addr_flag ));

	/* Bind a listener socket to that port */
	if (bind( sockfd, (struct sockaddr *) &proxy_addr, sizeof( proxy_addr )) < 0 )
	{
		int save_errno = errno;
		closesocket(sockfd);
		errno = save_errno;
		ereport(ERROR,
				(errmsg("could not create listener for debugger connection")));
	}

	/* Get the port number selected by the TCP/IP stack */
	getsockname( sockfd, (struct sockaddr *)&proxy_addr, &proxy_addr_len );

	*port = (int) ntohs( proxy_addr.sin_port );

	/* Get ready to wait for a client */
	listen(sockfd, 2);

	elog(DEBUG1, "listening for debugging target at port %d", *port);

	return sockfd;
}

/*
 * Find first available target slot.
 *
 * Note: Caller must be holding the lock.
 */
static int
findFreeTargetSlot(void)
{
	int		i;

	for (i = 0; i < NumTargetSlots; i++)
	{
		if (dbgcomm_slots[i].backendid == InvalidBackendId)
			return i;
		if (dbgcomm_slots[i].backendid == MyBackendId)
		{
			/*
			 * If we've failed to deallocate our slot earlier, reuse this slot.
			 * This shouldn't happen.
			 */
			elog(LOG, "found leftover debugging target slot for backend %d",
				 MyBackendId);
			return i;
		}
	}
	return -1;
}


/*
 * Find target slot belonging to given backend.
 *
 * Note: Caller must be holding the lock.
 */
static int
findTargetSlot(BackendId backendid)
{
	int		i;

	for (i = 0; i < NumTargetSlots; i++)
	{
		if (dbgcomm_slots[i].backendid == backendid)
			return i;
	}
	return -1;
}


/*******************************************************************************
 * resolveHostName()
 *
 *	Given the name of a host (hostName), this function returns the IP address
 *	of that host (or 0 if the name does not resolve to an address).
 *
 *	FIXME: this function should probably be a bit more flexibile.
 */

#ifndef INADDR_NONE
#define INADDR_NONE ((unsigned long int) -1)	/* For Solaris */
#endif

static uint32 resolveHostName( const char * hostName )
{
	struct hostent * hostDesc;
	uint32           hostAddress;

	if(( hostDesc = gethostbyname( hostName )))
		hostAddress = ((struct in_addr *)hostDesc->h_addr )->s_addr;
	else
		hostAddress = inet_addr( hostName );

	if(( hostAddress == -1 ) || ( hostAddress == INADDR_NONE ))
		return( 0 );
	else
		return( hostAddress );
}
