// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"

#include <cstdint>
#include <mutex>

#include <boost/filesystem/operations.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * The DiskSpaceMonitor is a periodic job that observes at the remaining disk space for the database
 * and takes action based on its observations.
 */
class DiskSpaceMonitor final {
public:
    static void start(ServiceContext* svcCtx);
    static void stop(ServiceContext* svcCtx);
    static DiskSpaceMonitor* get(ServiceContext* svcCtx);

    /**
     * Registers an action that responds to changes in disk space and returns its id.
     * If the disk space in bytes falls below the value returned from getThresholdBytes(), the act()
     * function will be called.
     * act() may be called an indefinite number of times when the disk falls below its threshold.
     */
    int64_t registerAction(
        std::function<int64_t()> getThresholdBytes,
        std::function<void(OperationContext*, int64_t availableBytes, int64_t thresholdBytes)> act);

    /**
     * Deregisters the action corresponding to the given id.
     */
    void deregisterAction(int64_t actionId);

    /**
     * Immediately runs the action corresponding to the given id if the available disk space is
     * below its threshold.
     */
    void runAction(OperationContext* opCtx, int64_t id);

    /**
     * Immediately runs each registered action depending on whether the available disk space is
     * below its threshold.
     */
    void runAllActions(OperationContext* opCtx);

private:
    struct Action {
        std::function<int64_t()> getThresholdBytes;
        std::function<void(OperationContext*, int64_t, int64_t)> act;
    };

    void _start(ServiceContext* svcCtx);
    void _stop();

    void _run(Client*);

    void _runAction(OperationContext* opCtx, const Action& action) const;

    PeriodicJobAnchor _job;

    // Copy of the dbpath which is always safe to access.
    boost::filesystem::path _dbpath;
    // This mutex protects _actions and the entire run loop of the disk space monitor.
    // The mutex also enables us to increment the _actionId for each new action added to _actions.
    std::mutex _mutex;
    stdx::unordered_map<int64_t, Action> _actions;

    int64_t _actionId = 0;
};
}  // namespace mongo
