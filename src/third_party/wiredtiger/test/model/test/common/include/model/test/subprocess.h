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

#pragma once

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <string>

extern "C" {
#include "test_util.h"
}

/*
 * in_subprocess --
 *     Run the following code in a subprocess, and exit normally at the end.
 */
#define in_subprocess                                   \
    for (subprocess_helper __sh;; __sh.exit_if_child()) \
        if (__sh.parent()) {                            \
            __sh.wait_if_parent();                      \
            break;                                      \
        } else

/*
 * in_subprocess_abort --
 *     Run the following code in a subprocess, and abort at the end.
 */
#define in_subprocess_abort                              \
    for (subprocess_helper __sh;; __sh.abort_if_child()) \
        if (__sh.parent()) {                             \
            __sh.wait_if_parent();                       \
            break;                                       \
        } else

/*
 * subprocess_helper --
 *     Helper for subprocess execution.
 */
class subprocess_helper {

public:
    /*
     * subprocess_helper::subprocess_helper --
     *     Create a new instance of the helper, and fork a subprocess.
     */
    subprocess_helper();

    /*
     * subprocess_helper::~subprocess_helper --
     *     Clean up.
     */
    ~subprocess_helper();

    /*
     * subprocess_helper::abort_if_child --
     *     Abort the subprocess, but only if it is the child.
     */
    void abort_if_child();

    /*
     * subprocess_helper::exit_if_child --
     *     Exit the subprocess cleanly, but only if it is the child.
     */
    void exit_if_child(int code = 0);

    /*
     * subprocess_helper::wait_if_parent --
     *     Wait for the subprocess to exit, but only if this is the parent.
     */
    void wait_if_parent();

    /*
     * subprocess_helper::child --
     *     Check whether we are running in the child subprocess.
     */
    inline bool
    child() const noexcept
    {
        return _child_pid == 0;
    }

    /*
     * subprocess_helper::parent --
     *     Check whether we are running in the parent subprocess.
     */
    inline bool
    parent() const noexcept
    {
        return _child_pid != 0;
    }

private:
    pid_t _child_pid;
    std::string _monitor_child_sentinel;

    struct sigaction _previous_sigaction;
};
