/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"

#include <functional>
#include <string>
#include <thread>

namespace mongo {
namespace process_health {

class FaultManager;

/**
 *  Tracks that the health checks are invoked regularly and have progress.
 */
class ProgressMonitor {
public:
    ProgressMonitor(FaultManager* faultManager,
                    ServiceContext* svcCtx,
                    std::function<void(std::string cause)> crashCb);

    // Signals that the monitoring can stop and blocks until the thread is joined.
    // Invoked to signal that the task executor is joined.
    void join();

    // Checks that the health checks are invoked and are not stuck forever. Invokes the callback
    // after timeout configured by options.
    void progressMonitorCheck(std::function<void(std::string cause)> crashCb);

private:
    // Checks that the periodic health checks actually make progress.
    void _progressMonitorLoop();

    FaultManager* const _faultManager;
    ServiceContext* const _svcCtx;
    // Callback used to crash the server.
    const std::function<void(std::string cause)> _crashCb;
    // This flag is set after the _taskExecutor join() returns, thus
    // we know no more health checks are still running.
    AtomicWord<bool> _terminate{false};
    stdx::thread _progressMonitorThread;
};

}  // namespace process_health
}  // namespace mongo
