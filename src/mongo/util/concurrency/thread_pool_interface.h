// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/functional.h"
#include "mongo/util/modules.h"
#include "mongo/util/out_of_line_executor.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

class Status;

/**
 * Interface for a thread pool.
 */
class [[MONGO_MOD_OPEN]] ThreadPoolInterface : public OutOfLineExecutor {
    ThreadPoolInterface(const ThreadPoolInterface&) = delete;
    ThreadPoolInterface& operator=(const ThreadPoolInterface&) = delete;

public:
    /**
     * Destroys a thread pool.
     *
     * The destructor may block if join() has not previously been called and returned.
     * It is fatal to destroy the pool while another thread is blocked on join().
     */
    ~ThreadPoolInterface() override = default;

    /**
     * Starts the thread pool. May be called at most once.
     */
    virtual void startup() = 0;

    /**
     * Signals the thread pool to shut down.  Returns promptly.
     *
     * After this call, the thread will return an error for subsequent calls to schedule().
     *
     * May be called by a task executing in the thread pool.  Call join() after calling shutdown()
     * to block until all tasks scheduled on the pool complete.
     */
    virtual void shutdown() = 0;

    /**
     * Blocks until the thread pool has fully shut down. Call at most once, and never from a task
     * inside the pool.
     */
    virtual void join() = 0;

protected:
    ThreadPoolInterface() = default;
};

}  // namespace mongo
