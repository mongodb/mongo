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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/periodic_balancer_config_refresher.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

const auto getPeriodicBalancerConfigRefresher =
    ServiceContext::declareDecoration<PeriodicBalancerConfigRefresher>();

std::unique_ptr<PeriodicRunner::PeriodicJobHandle> launchBalancerConfigRefresher(
    ServiceContext* serviceContext) {
    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);

    PeriodicRunner::PeriodicJob job(
        "PeriodicBalancerConfigRefresher",
        [](Client* client) {
            auto opCtx = client->makeOperationContext();

            const auto balancerConfig = Grid::get(opCtx.get())->getBalancerConfiguration();
            invariant(balancerConfig);

            Status status = balancerConfig->refreshAndCheck(opCtx.get());
            if (!status.isOK()) {
                log() << "Failed to refresh balancer configuration" << causedBy(status);
            }
        },
        Seconds(30));
    auto balancerConfigRefresher = periodicRunner->makeJob(std::move(job));
    balancerConfigRefresher->start();
    return balancerConfigRefresher;
}

}  // namespace

PeriodicBalancerConfigRefresher& PeriodicBalancerConfigRefresher::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

PeriodicBalancerConfigRefresher& PeriodicBalancerConfigRefresher::get(
    ServiceContext* serviceContext) {
    return getPeriodicBalancerConfigRefresher(serviceContext);
}

void PeriodicBalancerConfigRefresher::onShardingInitialization(ServiceContext* serviceContext,
                                                               bool isPrimary) {
    _isPrimary = isPrimary;
    // This function is called on sharding state initialization, so go ahead
    // and start up the balancer config refresher task if we're a primary.
    if (isPrimary && !_balancerConfigRefresher) {
        _balancerConfigRefresher = launchBalancerConfigRefresher(serviceContext);
    }
}
void PeriodicBalancerConfigRefresher::onStepUp(ServiceContext* serviceContext) {
    if (!_isPrimary) {
        _isPrimary = true;
        // If this is the first time we're stepping up, start a thread to periodically refresh the
        // balancer configuration.
        if (!_balancerConfigRefresher) {
            _balancerConfigRefresher = launchBalancerConfigRefresher(serviceContext);
        } else {
            // If we're stepping up again after having stepped down, just resume
            // the existing task.
            _balancerConfigRefresher->resume();
        }
    }
}

void PeriodicBalancerConfigRefresher::onStepDown() {
    if (_isPrimary) {
        _isPrimary = false;
        invariant(_balancerConfigRefresher);
        // We don't need to be refreshing the balancer configuration unless we're primary.
        _balancerConfigRefresher->pause();
    }
}

}  // namespace mongo
