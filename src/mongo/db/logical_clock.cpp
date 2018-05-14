/**
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/logical_clock.h"

#include "mongo/base/status.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/util/log.h"

namespace mongo {

constexpr Seconds LogicalClock::kMaxAcceptableLogicalClockDriftSecs;

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(maxAcceptableLogicalClockDriftSecs,
                                      long long,
                                      LogicalClock::kMaxAcceptableLogicalClockDriftSecs.count())
    ->withValidator([](const long long& potentialNewValue) {
        if (potentialNewValue <= 0) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "maxAcceptableLogicalClockDriftSecs must be positive, "
                                           "but attempted to set to: "
                                        << potentialNewValue);
        }
        return Status::OK();
    });

namespace {
const auto getLogicalClock = ServiceContext::declareDecoration<std::unique_ptr<LogicalClock>>();

bool lessThanOrEqualToMaxPossibleTime(LogicalTime time, uint64_t nTicks) {
    return time.asTimestamp().getSecs() <= LogicalClock::kMaxSignedInt &&
        time.asTimestamp().getInc() <= (LogicalClock::kMaxSignedInt - nTicks);
}
}

LogicalTime LogicalClock::getClusterTimeForReplicaSet(OperationContext* opCtx) {
    if (getGlobalReplSettings().usingReplSets()) {
        return get(opCtx)->getClusterTime();
    }

    return {};
}

LogicalClock* LogicalClock::get(ServiceContext* service) {
    return getLogicalClock(service).get();
}

LogicalClock* LogicalClock::get(OperationContext* ctx) {
    return get(ctx->getClient()->getServiceContext());
}

void LogicalClock::set(ServiceContext* service, std::unique_ptr<LogicalClock> clockArg) {
    auto& clock = getLogicalClock(service);
    clock = std::move(clockArg);
}

LogicalClock::LogicalClock(ServiceContext* service) : _service(service) {}

LogicalTime LogicalClock::getClusterTime() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _clusterTime;
}

Status LogicalClock::advanceClusterTime(const LogicalTime newTime) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    auto rateLimitStatus = _passesRateLimiter_inlock(newTime);
    if (!rateLimitStatus.isOK()) {
        return rateLimitStatus;
    }

    if (newTime > _clusterTime) {
        _clusterTime = newTime;
    }

    return Status::OK();
}

LogicalTime LogicalClock::reserveTicks(uint64_t nTicks) {

    invariant(nTicks > 0 && nTicks <= kMaxSignedInt);

    stdx::lock_guard<stdx::mutex> lock(_mutex);

    LogicalTime clusterTime = _clusterTime;

    const unsigned wallClockSecs =
        durationCount<Seconds>(_service->getFastClockSource()->now().toDurationSinceEpoch());
    unsigned clusterTimeSecs = clusterTime.asTimestamp().getSecs();

    // Synchronize clusterTime with wall clock time, if clusterTime was behind in seconds.
    if (clusterTimeSecs < wallClockSecs) {
        clusterTime = LogicalTime(Timestamp(wallClockSecs, 0));
    }
    // If reserving 'nTicks' would force the cluster timestamp's increment field to exceed (2^31-1),
    // overflow by moving to the next second. We use the signed integer maximum as an overflow point
    // in order to preserve compatibility with potentially signed or unsigned integral Timestamp
    // increment types. It is also unlikely to apply more than 2^31 oplog entries in the span of one
    // second.
    else if (clusterTime.asTimestamp().getInc() > (kMaxSignedInt - nTicks)) {

        log() << "Exceeded maximum allowable increment value within one second. Moving clusterTime "
                 "forward to the next second.";

        // Move time forward to the next second
        clusterTime = LogicalTime(Timestamp(clusterTime.asTimestamp().getSecs() + 1, 0));
    }

    uassert(40482,
            "cluster time cannot be advanced beyond its maximum value",
            lessThanOrEqualToMaxPossibleTime(clusterTime, nTicks));

    // Save the next cluster time.
    clusterTime.addTicks(1);
    _clusterTime = clusterTime;

    // Add the rest of the requested ticks if needed.
    if (nTicks > 1) {
        _clusterTime.addTicks(nTicks - 1);
    }

    return clusterTime;
}

void LogicalClock::setClusterTimeFromTrustedSource(LogicalTime newTime) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    // Rate limit checks are skipped here so a server with no activity for longer than
    // maxAcceptableLogicalClockDriftSecs seconds can still have its cluster time initialized.

    uassert(40483,
            "cluster time cannot be advanced beyond its maximum value",
            lessThanOrEqualToMaxPossibleTime(newTime, 0));

    if (newTime > _clusterTime) {
        _clusterTime = newTime;
    }
}

Status LogicalClock::_passesRateLimiter_inlock(LogicalTime newTime) {
    const unsigned wallClockSecs =
        durationCount<Seconds>(_service->getFastClockSource()->now().toDurationSinceEpoch());
    auto maxAcceptableDriftSecs = static_cast<const unsigned>(maxAcceptableLogicalClockDriftSecs);
    auto newTimeSecs = newTime.asTimestamp().getSecs();

    // Both values are unsigned, so compare them first to avoid wrap-around.
    if ((newTimeSecs > wallClockSecs) && (newTimeSecs - wallClockSecs) > maxAcceptableDriftSecs) {
        return Status(ErrorCodes::ClusterTimeFailsRateLimiter,
                      str::stream() << "New cluster time, " << newTimeSecs
                                    << ", is too far from this node's wall clock time, "
                                    << wallClockSecs
                                    << ".");
    }

    uassert(40484,
            "cluster time cannot be advanced beyond its maximum value",
            lessThanOrEqualToMaxPossibleTime(newTime, 0));

    return Status::OK();
}

bool LogicalClock::isEnabled() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _isEnabled;
}

void LogicalClock::disable() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _isEnabled = false;
}

}  // namespace mongo
