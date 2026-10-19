/*-------------------------------------------------------------------------
 *
 * pg_numa.h
 *	  NUMA-aware placement of PostgreSQL's main shared memory segment.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/include/storage/pg_numa.h
 *-------------------------------------------------------------------------
 */
#ifndef PG_NUMA_H
#define PG_NUMA_H

#include "utils/guc.h"

/*
 * GUC "numa", parsed by check_numa_shmem().  Accepted values:
 *   "off"          - keep the kernel default (first-touch) policy
 *   "all"          - interleave the segment across every online NUMA node
 *                    (MPOL_INTERLEAVE)
 *   "@<nodelist>"  - bind the segment to the given nodes (MPOL_BIND),
 *                    e.g. "@0,2-3" -- memory isolation / consolidation
 *   "=<nodelist>"  - prefer the given nodes (MPOL_PREFERRED)
 * The nodelist syntax is the one accepted by numa_parse_nodestring().
 */
extern PGDLLIMPORT char *NumaShmem;

extern bool check_numa_shmem(char **newval, void **extra, GucSource source);
extern void pg_numa_apply_shmem_policy(void *addr, Size size);

#endif							/* PG_NUMA_H */
