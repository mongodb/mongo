/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <map>
#include <memory>
#include <vector>

#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/future.h"

namespace mongo {

/**
 * Provides a facility for asynchronously waiting a local opTime to be majority committed.
 */
class WaitForMajorityService {
public:
    ~WaitForMajorityService();

    static WaitForMajorityService& get(ServiceContext* service);

    /**
     * Sets up the background thread responsible for waiting for opTimes to be majority committed.
     */
    void setUp(ServiceContext* service);

    /**
     * Blocking method, which shuts down and joins the background thread.
     */
    void shutDown();

    /**
     * Enqueue a request to wait for the given opTime to be majority committed.
     */
    SharedSemiFuture<void> waitUntilMajority(const repl::OpTime& opTime);

private:
    using OpTimeWaitingMap = std::map<repl::OpTime, SharedPromise<void>>;

    /**
     * Periodically checks the list of opTimes to wait for majority committed.
     */
    void _periodicallyWaitForMajority(ServiceContext* service);

    stdx::mutex _mutex;

    // Contains an ordered list of opTimes to wait to be majority comitted.
    OpTimeWaitingMap _queuedOpTimes;

    // Contains the last opTime that the background thread was able to successfully wait to be
    // majority comitted.
    repl::OpTime _lastOpTimeWaited;

    // The background thread.
    stdx::thread _thread;

    // Use for signalling new opTime requests being queued.
    stdx::condition_variable _hasNewOpTimeCV;

    // If set, contains a reference to the opCtx being used by the background thread.
    // Only valid when _thread.joinable() and not nullptr.
    OperationContext* _opCtx{nullptr};

    // Flag is set to true after shutDown() is called.
    bool _inShutDown{false};
};

}  // namespace mongo
