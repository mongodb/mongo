/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/periodic_runner.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem/operations.hpp>

namespace mongo {

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
    std::string _dbpath;
    // This mutex protects _actions and the entire run loop of the disk space monitor.
    // The mutex also enables us to increment the _actionId for each new action added to _actions.
    stdx::mutex _mutex;
    stdx::unordered_map<int64_t, Action> _actions;

    int64_t _actionId = 0;
};
}  // namespace mongo
