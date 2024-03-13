/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/types.h>
#include <cstdlib>
#include <signal.h>
#include <vector>
#include <unistd.h>

extern "C" {
#include "test_util.h"
}

#include "model/test/subprocess.h"
#include "model/test/util.h"

static std::vector<std::string> sentinel_stack;

/*
 * handler_sigchld --
 *     Signal handler to catch if the child died.
 */
static void
handler_sigchld(int sig)
{
    pid_t pid;

    pid = wait(NULL);
    WT_UNUSED(sig);

    /* Proceed with failing the parent only if the sentinel file is present. */
    testutil_assert(!sentinel_stack.empty());
    if (!testutil_exists(NULL, sentinel_stack.back().c_str()))
        return;

    /* The core file will indicate why the child exited. Choose EINVAL here. */
    testutil_die(EINVAL, "Child process %" PRIu64 " abnormally exited", (uint64_t)pid);
}

/*
 * subprocess_helper::subprocess_helper --
 *     Create a new instance of the helper, and fork a subprocess.
 */
subprocess_helper::subprocess_helper()
{
    /* Create the sentinel file through which we communicate about whether to monitor the child. */
    _monitor_child_sentinel = create_tmp_file("/tmp", "wt-subprocess-", ".sentinel");
    sentinel_stack.push_back(_monitor_child_sentinel);

    /* Configure the child death handling. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_sigchld;
    testutil_assert_errno(sigaction(SIGCHLD, &sa, &_previous_sigaction) == 0);

    /* Fork! */
    testutil_assert_errno((_child_pid = fork()) >= 0);
}

/*
 * subprocess_helper::~subprocess_helper --
 *     Clean up.
 */
subprocess_helper::~subprocess_helper()
{
    /* Clean up the sentinel file. We no longer need it. */
    testutil_assert(sentinel_stack.back() == _monitor_child_sentinel);
    testutil_remove(_monitor_child_sentinel.c_str());

    /* Clean up the parent. */
    if (parent()) {
        /* Restore the previous signal handler. */
        testutil_assert_errno(sigaction(SIGCHLD, &_previous_sigaction, nullptr) == 0);
    }
}

/*
 * subprocess_helper::abort_if_child --
 *     Abort the subprocess, but only if it is the child.
 */
void
subprocess_helper::abort_if_child()
{
    if (child()) {
        /* Subprocess failure is now expected. */
        testutil_remove(_monitor_child_sentinel.c_str());

        /* Terminate self with a signal that doesn't produce a core file. */
        (void)kill(getpid(), SIGKILL);

        /* Abort in the very unlikely case the previous call failed. */
        abort();
    }
}

/*
 * subprocess_helper::exit_if_child --
 *     Exit the subprocess cleanly, but only if it is the child.
 */
void
subprocess_helper::exit_if_child(int code)
{
    if (child()) {
        /* Subprocess failure is now expected. */
        testutil_remove(_monitor_child_sentinel.c_str());

        exit(code);
    }
}

/*
 * subprocess_helper::wait_if_parent --
 *     Wait for the subprocess to exit, but only if this is the parent.
 */
void
subprocess_helper::wait_if_parent()
{
    if (parent()) {
        int status;
        testutil_assert(waitpid(_child_pid, &status, 0) > 0);
    }
}
