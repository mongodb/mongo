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


#include "mongo/db/topology/mongos_topology_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shutdown_in_progress_quiesce_info.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {

const auto getMongosTopologyCoordinator =
    ServiceContext::declareDecoration<MongosTopologyCoordinator>();

/**
 * Generate an identifier unique to this instance.
 */
OID instanceId;

MONGO_INITIALIZER(GenerateMongosInstanceId)(InitializerContext*) {
    instanceId = OID::gen();
}

// Signals that a hello request has started waiting.
MONGO_FAIL_POINT_DEFINE(waitForHelloResponseMongos);
// Awaitable hello requests with the proper topologyVersions are expected to wait for
// maxAwaitTimeMS on mongos. When set, this failpoint will hang right before waiting on a
// topology change.
MONGO_FAIL_POINT_DEFINE(hangWhileWaitingForHelloResponseMongos);
// Failpoint for hanging during quiesce mode on mongos.
MONGO_FAIL_POINT_DEFINE(hangDuringQuiesceModeMongos);
// Simulates returning a specified error in the hello response.
MONGO_FAIL_POINT_DEFINE(setCustomErrorInHelloResponseMongoS);

template <typename T>
StatusOrStatusWith<T> futureGetNoThrowWithDeadline(OperationContext* opCtx,
                                                   SharedSemiFuture<T>& f,
                                                   Date_t deadline,
                                                   ErrorCodes::Error error) {
    try {
        return opCtx->runWithDeadline(deadline, error, [&] { return f.getNoThrow(opCtx); });
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

/**
 * ShutdownInProgress error message
 */

constexpr StringData kQuiesceModeShutdownMessage =
    "Mongos is in quiesce mode and will shut down"_sd;

}  // namespace

// Make MongosTopologyCoordinator a decoration on the ServiceContext.
MongosTopologyCoordinator* MongosTopologyCoordinator::get(OperationContext* opCtx) {
    return &getMongosTopologyCoordinator(opCtx->getClient()->getServiceContext());
}

MongosTopologyCoordinator::MongosTopologyCoordinator()
    : _topologyVersion(instanceId, 0),
      _inQuiesceMode(false),
      _promise(std::make_shared<SharedPromise<std::shared_ptr<const MongosHelloResponse>>>()) {}

long long MongosTopologyCoordinator::_calculateRemainingQuiesceTimeMillis() const {
    auto preciseClock = getGlobalServiceContext()->getPreciseClockSource();
    auto remainingQuiesceTimeMillis =
        std::max(Milliseconds::zero(), _quiesceDeadline - preciseClock->now());
    // Turn remainingQuiesceTimeMillis into an int64 so that it's a supported BSONElement.
    long long remainingQuiesceTimeLong = durationCount<Milliseconds>(remainingQuiesceTimeMillis);
    return remainingQuiesceTimeLong;
}

std::shared_ptr<MongosHelloResponse> MongosTopologyCoordinator::_makeHelloResponse(
    WithLock lock) const {
    // It's possible for us to transition to Quiesce Mode after a hello request timed out.
    // Check that we are not in Quiesce Mode before returning a response to avoid responding with
    // a higher topology version, but no indication that we are shutting down.
    uassert(ShutdownInProgressQuiesceInfo(_calculateRemainingQuiesceTimeMillis()),
            kQuiesceModeShutdownMessage,
            !_inQuiesceMode);

    auto response = std::make_shared<MongosHelloResponse>(_topologyVersion);
    return response;
}

std::shared_ptr<const MongosHelloResponse> MongosTopologyCoordinator::awaitHelloResponse(
    OperationContext* opCtx,
    boost::optional<TopologyVersion> clientTopologyVersion,
    boost::optional<Date_t> deadline) const {
    stdx::unique_lock lk(_mutex);

    // Fail all new hello requests with ShutdownInProgress if we've transitioned to Quiesce
    // Mode.
    uassert(ShutdownInProgressQuiesceInfo(_calculateRemainingQuiesceTimeMillis()),
            kQuiesceModeShutdownMessage,
            !_inQuiesceMode);

    // Respond immediately if:
    // (1) There is no clientTopologyVersion, which indicates that the client is not using
    //     awaitable hello.
    // (2) The process IDs are different.
    // (3) The clientTopologyVersion counter is less than mongos' counter.
    if (!clientTopologyVersion ||
        clientTopologyVersion->getProcessId() != _topologyVersion.getProcessId() ||
        clientTopologyVersion->getCounter() < _topologyVersion.getCounter()) {
        return _makeHelloResponse(lk);
    }
    uassert(51761,
            str::stream() << "Received a topology version with counter: "
                          << clientTopologyVersion->getCounter()
                          << " which is greater than the mongos topology version counter: "
                          << _topologyVersion.getCounter(),
            clientTopologyVersion->getCounter() == _topologyVersion.getCounter());

    auto future = _promise->getFuture();
    // At this point, we have verified that clientTopologyVersion is not none. It this is true,
    // deadline must also be not none.
    invariant(deadline);

    HelloMetrics::get(opCtx)->incrementNumAwaitingTopologyChanges();

    // Wait for a mongos topology change with timeout set to deadline.
    LOGV2_DEBUG(4695502,
                1,
                "Waiting for a hello response from a topology change or until deadline",
                "deadline"_attr = deadline.value(),
                "currentMongosTopologyVersionCounter"_attr = _topologyVersion.getCounter());

    lk.unlock();

    if (MONGO_unlikely(waitForHelloResponseMongos.shouldFail())) {
        // Used in tests that wait for this failpoint to be entered before shutting down mongos,
        // which is the only action that triggers a topology change.
        LOGV2(4695704, "waitForHelloResponseMongos failpoint enabled");
    }

    if (MONGO_unlikely(hangWhileWaitingForHelloResponseMongos.shouldFail())) {
        LOGV2(4695501, "hangWhileWaitingForHelloResponseMongos failpoint enabled");
        hangWhileWaitingForHelloResponseMongos.pauseWhileSet(opCtx);
    }

    auto statusWithHello =
        futureGetNoThrowWithDeadline(opCtx, future, deadline.value(), opCtx->getTimeoutError());
    auto status = statusWithHello.getStatus();

    setCustomErrorInHelloResponseMongoS.execute([&](const BSONObj& data) {
        auto errorCode = data["errorType"].safeNumberInt();
        LOGV2(6208202,
              "Triggered setCustomErrorInHelloResponseMongoS fail point.",
              "errorCode"_attr = errorCode);

        status = Status(ErrorCodes::Error(errorCode),
                        "Set by setCustomErrorInHelloResponseMongoS fail point.");
    });
    ON_BLOCK_EXIT([&, opCtx] {
        // We decrement the counter. Note that some errors may already be covered
        // by calls to resetNumAwaitingTopologyChanges(), which sets the counter to zero, so we
        // only decrement non-zero counters. This is safe so long as:
        // 1) Increment + decrement calls always occur at a 1:1 ratio and in that order.
        // 2) All callers to increment/decrement/reset take locks.
        stdx::lock_guard lk(_mutex);
        if (status != ErrorCodes::SplitHorizonChange &&
            HelloMetrics::get(opCtx)->getNumAwaitingTopologyChanges() > 0) {
            HelloMetrics::get(opCtx)->decrementNumAwaitingTopologyChanges();
        }
    });

    if (!status.isOK()) {
        LOGV2_DEBUG(6208205, 1, "Error while waiting for hello response", "status"_attr = status);

        // Return a MongosHelloResponse with the current topology version on timeout when
        // waiting for a topology change.
        if (status == ErrorCodes::ExceededTimeLimit) {
            stdx::lock_guard lk(_mutex);
            return _makeHelloResponse(lk);
        }
    }

    // A topology change has happened so we return a MongosHelloResponse with the updated
    // topology version.
    uassertStatusOK(status);
    return statusWithHello.getValue();
}

void MongosTopologyCoordinator::enterQuiesceModeAndWait(OperationContext* opCtx,
                                                        Milliseconds quiesceTime) {
    {
        stdx::lock_guard lk(_mutex);
        _inQuiesceMode = true;
        _quiesceDeadline = getGlobalServiceContext()->getPreciseClockSource()->now() + quiesceTime;

        // Increment the topology version and respond to any waiting hello request with an error.
        auto counter = _topologyVersion.getCounter();
        _topologyVersion.setCounter(counter + 1);
        _promise->setError(
            Status(ShutdownInProgressQuiesceInfo(_calculateRemainingQuiesceTimeMillis()),
                   kQuiesceModeShutdownMessage));

        // Reset counter to 0 since we will respond to all waiting hello requests with an error.
        // All new hello requests will immediately fail with ShutdownInProgress.
        HelloMetrics::resetNumAwaitingTopologyChangesForAllSessionManagers(
            getGlobalServiceContext());
    }

    if (MONGO_unlikely(hangDuringQuiesceModeMongos.shouldFail())) {
        LOGV2(4695700, "hangDuringQuiesceModeMongos failpoint enabled");
        hangDuringQuiesceModeMongos.pauseWhileSet(opCtx);
    }

    LOGV2(4695701, "Entering quiesce mode for mongos shutdown", "quiesceTime"_attr = quiesceTime);
    opCtx->sleepUntil(_quiesceDeadline);
    LOGV2(4695702, "Exiting quiesce mode for mongos shutdown");
}

}  // namespace mongo
