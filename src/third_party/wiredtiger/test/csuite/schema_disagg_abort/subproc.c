/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "test_util.h"

#include "subproc.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#if !defined(__GLIBC__)
/* POSIX specifies the symbol but not a header declaration; GLIBC declares it in <unistd.h>. */
extern char **environ;
#endif

/*
 * subproc_pipe --
 *     Create the leader-to-follower event pipe.
 */
void
subproc_pipe(int fds[2])
{
    testutil_assert_errno(pipe(fds) == 0);
}

/*
 * subproc_self_path --
 *     Resolve the running binary's path for re-spawning.
 */
void
subproc_self_path(const char *argv0, char *buf, size_t buf_size)
{
    char resolved[PATH_MAX];

    /* argv0 works as-is when realpath fails (e.g., the binary was found via PATH). */
    const char *path = realpath(argv0, resolved) != NULL ? resolved : argv0;
    testutil_assert(strlen(path) < buf_size);
    strcpy(buf, path);
}

/*
 * subproc_spawn --
 *     Start a child process running the given binary, closing the given descriptors in the child.
 */
void
subproc_spawn(SUBPROC *proc, const char *who, const char *path, char *const argv[],
  const int *close_fds, size_t nclose)
{
    memset(proc, 0, sizeof(*proc));
    proc->who = who;

    posix_spawn_file_actions_t actions;
    testutil_assert(posix_spawn_file_actions_init(&actions) == 0);
    for (size_t i = 0; i < nclose; i++)
        testutil_assert(posix_spawn_file_actions_addclose(&actions, close_fds[i]) == 0);

    const int ret = posix_spawn(&proc->pid, path, &actions, NULL, argv, environ);
    testutil_assert(posix_spawn_file_actions_destroy(&actions) == 0);
    if (ret != 0)
        testutil_die(ret, "posix_spawn %s: %s", who, path);
}

/*
 * subproc_kill --
 *     Terminate a child abruptly, with no chance for cleanup.
 */
void
subproc_kill(SUBPROC *proc)
{
    testutil_assertfmt(!proc->reaped, "%s: killing an already reaped child", proc->who);
    if (kill(proc->pid, SIGKILL) != 0)
        testutil_die(errno, "kill %s", proc->who);
}

/*
 * subproc_decode --
 *     Translate a raw wait status into the portable status/code pair.
 */
static SUBPROC_STATUS
subproc_decode(int status, int *codep)
{
    if (WIFEXITED(status)) {
        *codep = WEXITSTATUS(status);
        return (SUBPROC_EXITED);
    }
    testutil_assert(WIFSIGNALED(status));
    *codep = WTERMSIG(status);
    return (SUBPROC_KILLED);
}

/*
 * subproc_reap --
 *     Collect a child's termination status, optionally blocking until it terminates.
 */
static SUBPROC_STATUS
subproc_reap(SUBPROC *proc, int *codep, bool block)
{
    if (!proc->reaped) {
        int status;
        const pid_t got = waitpid(proc->pid, &status, block ? 0 : WNOHANG);
        testutil_assert_errno(got != -1);
        if (got == 0)
            return (SUBPROC_RUNNING);
        proc->reaped = true;
        proc->status = status;
    }
    return (subproc_decode(proc->status, codep));
}

/*
 * subproc_poll --
 *     Non-blocking status probe.
 */
SUBPROC_STATUS
subproc_poll(SUBPROC *proc, int *codep)
{
    return (subproc_reap(proc, codep, false));
}

/*
 * subproc_wait --
 *     Blocking wait for termination.
 */
SUBPROC_STATUS
subproc_wait(SUBPROC *proc, int *codep)
{
    return (subproc_reap(proc, codep, true));
}
