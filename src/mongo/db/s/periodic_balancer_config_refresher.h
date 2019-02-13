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

#include "mongo/base/disallow_copying.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {

class OperationContext;
class ServiceContext;

class PeriodicBalancerConfigRefresher final {
    MONGO_DISALLOW_COPYING(PeriodicBalancerConfigRefresher);

public:
    PeriodicBalancerConfigRefresher() = default;
    ~PeriodicBalancerConfigRefresher() = default;

    PeriodicBalancerConfigRefresher(PeriodicBalancerConfigRefresher&& source) = delete;
    PeriodicBalancerConfigRefresher& operator=(PeriodicBalancerConfigRefresher&& other) = delete;

    /**
     * Obtains the service-wide chunk PeriodicBalancerConfigRefresher instance.
     */
    static PeriodicBalancerConfigRefresher& get(OperationContext* opCtx);
    static PeriodicBalancerConfigRefresher& get(ServiceContext* serviceContext);


    /**
     * Sets the mode to either primary or secondary. If it is primary, starts a
     * periodic task to refresh the balancer configuration.  The
     * PeriodicBalancerConfigRefresher is only active when primary.
     */
    void onShardingInitialization(ServiceContext* serviceContext, bool isPrimary);

    /**
     * Invoked when the shard server primary enters the 'PRIMARY' state to
     * trigger the start of the periodic refresh task.
     */
    void onStepUp(ServiceContext* serviceContext);

    /**
     * Invoked when this node which is currently serving as a 'PRIMARY' steps down.
     *
     * Pauses the periodic refresh until subsequent step up. This method might
     * be called multiple times in succession, which is what happens as a
     * result of incomplete transition to primary so it is resilient to that.
     */
    void onStepDown();

private:
    bool _isPrimary{false};

    // Periodic job for refreshing the balancer configuration
    std::unique_ptr<PeriodicRunner::PeriodicJobHandle> _balancerConfigRefresher;
};
}  // namespace mongo
