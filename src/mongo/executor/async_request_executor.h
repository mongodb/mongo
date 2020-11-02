/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <memory>
#include <string>

#include "mongo/db/request_execution_context.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"

namespace mongo {

/**
 * The base class for constructing command-specific asynchronous executors.
 * Requests (i.e., instances of `RequestExecutionContext`) are scheduled on a thread-pool, and
 * passed to the command-specific implementation of `handleRequest`.
 */
class AsyncRequestExecutor {
public:
    AsyncRequestExecutor(AsyncRequestExecutor&&) = delete;
    AsyncRequestExecutor(const AsyncRequestExecutor&) = delete;

    explicit AsyncRequestExecutor(std::string name);
    ~AsyncRequestExecutor();

    /**
     * Runs the command-specific code to handle the request.
     * Must only access the request on the corresponding client thread.
     */
    virtual Status handleRequest(std::shared_ptr<RequestExecutionContext>) = 0;

    /**
     * Schedules the request on a thread pool (i.e., `_pool`) and calls into `handleRequest` to
     * asynchronously execute the command.
     */
    Future<void> schedule(std::shared_ptr<RequestExecutionContext> rec);

    static constexpr auto kDiagnosticLogLevel = 4;

private:
    const std::string _name;
    std::unique_ptr<ThreadPool> _pool;
};

}  // namespace mongo
