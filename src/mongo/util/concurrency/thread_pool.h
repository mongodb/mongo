// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * A configurable thread pool, for general use.
 *
 * See the Options struct for information about how to configure an instance.
 */
class ThreadPool final : public ThreadPoolInterface {
public:
    /**
     * Structure used to configure an instance of ThreadPool.
     */
    struct Options {
        // Set maxThreads to this if you don't want to limit the number of threads in the pool.
        // Note: the value used here is high enough that it will never be reached, but low enough
        // that it won't cause overflows if mixed with signed ints or math.
        static constexpr size_t kUnlimited = 1'000'000'000;

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

    static std::unique_ptr<ThreadPool> make(Options options) {
        return std::make_unique<ThreadPool>(std::move(options));
    }

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

    /**
     * Set the minimum number of threads for this ThreadPool.
     * Calling this method will spin up new threads if the new minimum is greater than the current
     * number of threads.
     */
    void setMinThreads(size_t minThreads);

    /**
     * Set the maximum number of threads for this ThreadPool.
     * Calling this method will cause threads to be reaped once they finish their tasks if more than
     * the maximum are running.
     */
    void setMaxThreads(size_t maxThreads);

    uint64_t joinedThreadsCount_forTest() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace mongo
