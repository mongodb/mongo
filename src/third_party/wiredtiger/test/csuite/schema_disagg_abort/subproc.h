/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Minimal child-process layer built on spawn-style creation, chosen so a Windows port only has to
 * substitute the CRT near-equivalents (_spawnv, _pipe, _cwait/TerminateProcess) inside subproc.c
 * without touching the callers.
 */

#pragma once

#include <stdbool.h>
#include <sys/types.h>

typedef struct {
    const char *who; /* Child's role name for diagnostics; NULL marks an unspawned slot. */
    pid_t pid;
    bool reaped;
    int status; /* Raw wait status, valid once reaped. */
} SUBPROC;

/* Fixed slots for the children a test run can have, indexed by node id. */
#define SUBPROC_SLOTS 2

typedef enum {
    SUBPROC_RUNNING,
    SUBPROC_EXITED, /* Normal exit; code is the exit status. */
    SUBPROC_KILLED  /* Abnormal termination; code is the signal number. */
} SUBPROC_STATUS;

/* Create the leader-to-follower event pipe; fds[0] is the read end. */
void subproc_pipe(int fds[2]);

/* Resolve the running binary's path for re-spawning; argv0 is used as a fallback. */
void subproc_self_path(const char *argv0, char *buf, size_t buf_size);

/*
 * Start a child process running the given binary. The child inherits open descriptors except those
 * in close_fds (length nclose), which are closed in the child before it starts.
 */
void subproc_spawn(SUBPROC *proc, const char *who, const char *path, char *const argv[],
  const int *close_fds, size_t nclose);

/* Terminate a child abruptly, with no chance for cleanup. */
void subproc_kill(SUBPROC *proc);

/* Non-blocking status probe. */
SUBPROC_STATUS subproc_poll(SUBPROC *proc, int *codep);

/* Blocking wait for termination. */
SUBPROC_STATUS subproc_wait(SUBPROC *proc, int *codep);
