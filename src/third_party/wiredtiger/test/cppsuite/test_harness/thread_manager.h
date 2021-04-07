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

#include <thread>

#include "database_operation.h"
#include "thread_context.h"

namespace test_harness {
/* Class that handles threads, from their initialization to their deletion. */
class thread_manager {
    public:
    ~thread_manager()
    {
        for (auto &it : _workers) {
            if (it != nullptr && it->joinable()) {
                debug_print("You should've called join on the thread manager", DEBUG_ERROR);
                it->join();
            }
            delete it;
            it = nullptr;
        }
        _workers.clear();
    }

    /*
     * Generic function to create threads that take contexts, typically these will be static
     * functions.
     */
    template <typename Callable>
    void
    add_thread(thread_context *tc, database_operation *db_operation, Callable &&fct)
    {
        tc->set_running(true);
        std::thread *t = new std::thread(fct, std::ref(*tc), std::ref(*db_operation));
        _workers.push_back(t);
    }

    /*
     * Generic function to create threads that do not take thread contexts but take a single
     * argument, typically these threads are calling non static member function of classes.
     */
    template <typename Callable, typename Args>
    void
    add_thread(Callable &&fct, Args &&args)
    {
        std::thread *t = new std::thread(fct, args);
        _workers.push_back(t);
    }

    /*
     * Complete the operations for all threads.
     */
    void
    join()
    {
        for (const auto &it : _workers) {
            if (it->joinable())
                it->join();
        }
    }

    private:
    std::vector<std::thread *> _workers;
};
} // namespace test_harness

#endif
