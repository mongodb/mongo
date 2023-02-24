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

#include <boost/filesystem/operations.hpp>

#include "mongo/db/service_context.h"
#include "mongo/util/periodic_runner.h"

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
     * An Action defines a function that should be called when the available disk space falls below
     * a specified threshold.
     */
    struct Action {
        virtual ~Action() {}

        /**
         * If the disk space in bytes falls below this threshold, the act() function should be
         * called.
         */
        virtual int64_t getThresholdBytes() noexcept = 0;

        /**
         * Takes action when the defined threshold is reached. This function may be called an
         * indefinite number of times when the disk falls below its threshold.
         */
        virtual void act(OperationContext* opCtx, int64_t availableBytes) noexcept = 0;
    };

    /**
     * Register an action that responds to changes in disk space.
     */
    void registerAction(std::unique_ptr<Action> action);

    /**
     * Immediately take action based on the provided available disk space in bytes.
     */
    void takeAction(OperationContext* opCtx, int64_t availableBytes);

private:
    void _start(ServiceContext* svcCtx);
    void _stop();

    void _run(Client*);

    PeriodicJobAnchor _job;

    // This mutex protects _actions and the entire run loop of the disk space monitor.
    Mutex _mutex = MONGO_MAKE_LATCH("DiskSpaceMonitor::_mutex");
    std::vector<std::unique_ptr<Action>> _actions;
};
}  // namespace mongo
