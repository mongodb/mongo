/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <cstddef>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

/**
 * Implementation of a fixed-size pool of threads that can perform scheduled
 * tasks.
 *
 * Deprecated.  Use ThreadPool from thread_pool.h, instead.
 */
class OldThreadPool {
    MONGO_DISALLOW_COPYING(OldThreadPool);

public:
    typedef stdx::function<void(void)> Task;  // nullary function or functor
    struct DoNotStartThreadsTag {};

    explicit OldThreadPool(int nThreads = 8, const std::string& threadNamePrefix = "");
    explicit OldThreadPool(const DoNotStartThreadsTag&,
                           int nThreads = 8,
                           const std::string& threadNamePrefix = "");

    std::size_t getNumThreads() const;

    // Launches the worker threads; call exactly once, if and only if
    // you used the DoNotStartThreadsTag form of the constructor.
    void startThreads();

    // blocks until all tasks are complete (tasks_remaining() == 0)
    // does not prevent new tasks from being scheduled so could wait forever.
    // Also, new tasks could be scheduled after this returns.
    void join();

    // task will be copied a few times so make sure it's relatively cheap
    void schedule(Task task);

    // Helpers that wrap schedule and stdx::bind.
    // Functor and args will be copied a few times so make sure it's relatively cheap
    template <typename F, typename A>
    void schedule(F f, A a) {
        schedule(stdx::bind(f, a));
    }
    template <typename F, typename A, typename B>
    void schedule(F f, A a, B b) {
        schedule(stdx::bind(f, a, b));
    }
    template <typename F, typename A, typename B, typename C>
    void schedule(F f, A a, B b, C c) {
        schedule(stdx::bind(f, a, b, c));
    }
    template <typename F, typename A, typename B, typename C, typename D>
    void schedule(F f, A a, B b, C c, D d) {
        schedule(stdx::bind(f, a, b, c, d));
    }
    template <typename F, typename A, typename B, typename C, typename D, typename E>
    void schedule(F f, A a, B b, C c, D d, E e) {
        schedule(stdx::bind(f, a, b, c, d, e));
    }

private:
    ThreadPool _pool;
};

}  // namespace mongo
