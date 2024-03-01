/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/duration.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * A configurable thread pool, for general use.
 *
 * See the Options struct for information about how to configure an instance.
 */
class ThreadPool final : public ThreadPoolInterface {
public:
    /**
     * Contains a subset of the fields from Options related to limiting the number of concurrent
     * threads in the pool. Used in places where we want a way to specify limits to the size of a
     * ThreadPool without overriding the other behaviors of the pool such thread names or onCreate
     * behaviors. Each field of Limits maps directly to the same-named field in Options.
     */
    struct Limits {
        size_t minThreads = 1;
        size_t maxThreads = 8;
        Milliseconds maxIdleThreadAge = Seconds{30};
    };

    /**
     * Structure used to configure an instance of ThreadPool.
     */
    struct Options {
        // Set maxThreads to this if you don't want to limit the number of threads in the pool.
        // Note: the value used here is high enough that it will never be reached, but low enough
        // that it won't cause overflows if mixed with signed ints or math.
        static constexpr size_t kUnlimited = 1'000'000'000;

        Options() = default;

        explicit Options(const Limits& limits)
            : minThreads(limits.minThreads),
              maxThreads(limits.maxThreads),
              maxIdleThreadAge(limits.maxIdleThreadAge) {}

        // Name of the thread pool. If this string is empty, the pool will be assigned a
        // name unique to the current process.
        std::string poolName;

        // Prefix used to name threads for logging purposes.
        //
        // An integer will be appended to this string to create the thread name for each thread in
        // the pool.  Warning, if you create two pools and give them the same threadNamePrefix, you
        // could have multiple threads that report the same name. If you leave this empty, the
        // prefix will be the pool name followed by a hyphen.
        std::string threadNamePrefix;

        // Minimum number of threads that must be in the pool.
        //
        // At least this many threads will be created at startup, and the pool will not reduce the
        // total number of threads below this threshold before shutdown.
        size_t minThreads = 1;

        // The pool will never grow to contain more than this many threads.
        size_t maxThreads = 8;

        // If the pool has had at least one idle thread for this much time, it may consider reaping
        // a thread.
        Milliseconds maxIdleThreadAge = Seconds{30};

        /** If callable, called before each worker thread begins consuming tasks. */
        std::function<void(const std::string&)> onCreateThread;

        /**
         * If callable, called after joining each retired thread.
         * Since there could be multiple calls to this function in a single critical section,
         * avoid complex logic in the callback.
         */
        std::function<void(const stdx::thread&)> onJoinRetiredThread;
    };

    /**
     * Structure used to return information about the thread pool via getStats().
     */
    struct Stats {
        // The options for the instance of the pool returning these stats.
        Options options;

        // The count includes both regular worker threads and the cleanup thread,
        // whether idle or active, in the pool. So, this count can be greater than
        // options.maxThreads.
        size_t numThreads;

        // The number of idle threads currently in the pool.
        size_t numIdleThreads;

        // The number of tasks waiting to be executed by the pool.
        size_t numPendingTasks;

        // The last time that no threads in the pool were idle.
        Date_t lastFullUtilizationDate;
    };

    /**
     * Constructs a thread pool, configured with the given "options".
     */
    explicit ThreadPool(Options options);

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ~ThreadPool() override;

    // from OutOfLineExecutor (base of ThreadPoolInterface)
    void schedule(Task task) override;

    // from ThreadPoolInterface
    void startup() override;
    void shutdown() override;

    /**
     * Joins all scheduled tasks. Can also spawn a free thread that ignores maxThread options to
     * execute pending tasks.
     */
    void join() override;

    /**
     * Blocks the caller until there are no pending tasks on this pool.
     *
     * It is legal to call this whether or not shutdown has been called, but if it is called
     * *before* shutdown() is called, there is no guarantee that there will still be no pending
     * tasks when the function returns.
     *
     * May be called multiple times, by multiple threads. May not be called by a task in the thread
     * pool.
     */
    void waitForIdle();

    /**
     * Returns statistics about the thread pool's utilization.
     */
    Stats getStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace mongo
