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

#include "thread_manager.h"

#include "logger.h"

namespace test_harness {
thread_manager::~thread_manager()
{
    for (auto &it : _workers) {
        if (it != nullptr && it->joinable()) {
            logger::log_msg(LOG_ERROR, "You should've called join on the thread manager");
            it->join();
        }
        delete it;
        it = nullptr;
    }
    _workers.clear();
}

void
thread_manager::join()
{
    for (const auto &it : _workers) {
        while (!it->joinable()) {
            /* Helpful for diagnosing hangs. */
            logger::log_msg(LOG_TRACE, "Thread manager: Waiting to join.");
            /* Check every so often to avoid spamming the log. */
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        it->join();
    }
}
} // namespace test_harness
