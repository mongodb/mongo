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

#ifndef THREAD_MANAGER_H
#define THREAD_MANAGER_H

#include "thread_context.h"

namespace test_harness {
/* Class that handles threads, from their initialization to their deletion. */
class thread_manager {
    public:
    ~thread_manager()
    {
        for (auto *worker : _workers) {
            /* Make sure the worker is done before deleting it. */
            worker->finish();
            delete worker;
        }
    }

    template <typename Callable>
    void
    add_thread(thread_context *tc, Callable &&fct)
    {
        tc->set_running(true);
        std::thread *t = new std::thread(fct, std::ref(*tc));
        tc->set_thread(t);
        _workers.push_back(tc);
    }

    void
    finish()
    {
        for (auto *worker : _workers) {
            if (worker == nullptr)
                debug_info("finish : worker is NULL", _trace_level, DEBUG_ERROR);
            else
                worker->finish();
        }
    }

    private:
    std::vector<thread_context *> _workers;
};
} // namespace test_harness

#endif
