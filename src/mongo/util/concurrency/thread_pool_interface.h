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

#include "mongo/util/functional.h"
#include "mongo/util/modules_incompletely_marked_header.h"
#include "mongo/util/out_of_line_executor.h"

namespace mongo {

class Status;

/**
 * Interface for a thread pool.
 */
class MONGO_MOD_OPEN ThreadPoolInterface : public OutOfLineExecutor {
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
