// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
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
    Atomic<bool> _terminate{false};
    stdx::thread _progressMonitorThread;
};

}  // namespace process_health
}  // namespace mongo
