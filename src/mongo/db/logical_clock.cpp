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
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/util/log.h"

namespace mongo {

constexpr Seconds LogicalClock::kMaxAcceptableLogicalClockDrift;

server_parameter_storage_type<long long, ServerParameterType::kStartupOnly>::value_type
    maxAcceptableLogicalClockDrift(LogicalClock::kMaxAcceptableLogicalClockDrift.count());

class MaxAcceptableLogicalClockDrift
    : public ExportedServerParameter<long long, ServerParameterType::kStartupOnly> {
public:
    MaxAcceptableLogicalClockDrift()
        : ExportedServerParameter<long long, ServerParameterType::kStartupOnly>(
              ServerParameterSet::getGlobal(),
              "maxAcceptableLogicalClockDrift",
              &maxAcceptableLogicalClockDrift) {}

    Status validate(const long long& potentialNewValue) {
        if (potentialNewValue < 0) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "maxAcceptableLogicalClockDrift cannot be negative, but "
                                           "attempted to set to: "
                                        << potentialNewValue);
        }

        return Status::OK();
    }
} maxAcceptableLogicalClockDriftParameter;

namespace {
const auto getLogicalClock = ServiceContext::declareDecoration<std::unique_ptr<LogicalClock>>();
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

LogicalClock::LogicalClock(ServiceContext* service, std::unique_ptr<TimeProofService> tps)
    : _service(service), _timeProofService(std::move(tps)) {}

SignedLogicalTime LogicalClock::getClusterTime() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _clusterTime;
}

SignedLogicalTime LogicalClock::_makeSignedLogicalTime(LogicalTime logicalTime) {
    return SignedLogicalTime(logicalTime, _timeProofService->getProof(logicalTime));
}

Status LogicalClock::advanceClusterTime(const SignedLogicalTime& newTime) {
    const auto& newLogicalTime = newTime.getTime();

    // No need to check proof if no time was given.
    if (newLogicalTime == LogicalTime::kUninitialized) {
        return Status::OK();
    }

    invariant(_timeProofService);
    auto res = _timeProofService->checkProof(newLogicalTime, newTime.getProof());
    if (res != Status::OK()) {
        return res;
    }

    return advanceClusterTimeFromTrustedSource(newTime);
}

Status LogicalClock::_advanceClusterTime_inlock(SignedLogicalTime newTime) {
    if (newTime.getTime() > _clusterTime.getTime()) {
        _clusterTime = newTime;
    }

    return Status::OK();
}

Status LogicalClock::advanceClusterTimeFromTrustedSource(SignedLogicalTime newTime) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto rateLimitStatus = _passesRateLimiter_inlock(newTime.getTime());
    if (!rateLimitStatus.isOK()) {
        return rateLimitStatus;
    }

    return _advanceClusterTime_inlock(std::move(newTime));
}

Status LogicalClock::signAndAdvanceClusterTime(LogicalTime newTime) {
    auto newSignedTime = _makeSignedLogicalTime(newTime);

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _advanceClusterTime_inlock(std::move(newSignedTime));
}

LogicalTime LogicalClock::reserveTicks(uint64_t ticks) {

    invariant(ticks > 0);

    stdx::lock_guard<stdx::mutex> lock(_mutex);

    const unsigned wallClockSecs =
        durationCount<Seconds>(_service->getFastClockSource()->now().toDurationSinceEpoch());
    unsigned currentSecs = _clusterTime.getTime().asTimestamp().getSecs();
    LogicalTime clusterTimestamp = _clusterTime.getTime();

    if (MONGO_unlikely(currentSecs < wallClockSecs)) {
        clusterTimestamp = LogicalTime(Timestamp(wallClockSecs, 1));
    } else {
        clusterTimestamp.addTicks(1);
    }
    auto currentTime = clusterTimestamp;
    clusterTimestamp.addTicks(ticks - 1);

    // Fail if time is not moving forward for 2**31 ticks
    if (MONGO_unlikely(clusterTimestamp.asTimestamp().getSecs() > wallClockSecs) &&
        clusterTimestamp.asTimestamp().getInc() >= 1U << 31) {
        mongo::severe() << "clock skew detected, prev: " << wallClockSecs
                        << " now: " << clusterTimestamp.asTimestamp().getSecs();
        fassertFailed(17449);
    }

    _clusterTime = _makeSignedLogicalTime(clusterTimestamp);
    return currentTime;
}

void LogicalClock::initClusterTimeFromTrustedSource(LogicalTime newTime) {
    invariant(_clusterTime.getTime() == LogicalTime::kUninitialized);
    // Rate limit checks are skipped here so a server with no activity for longer than
    // maxAcceptableLogicalClockDrift seconds can still have its cluster time initialized.
    _clusterTime = _makeSignedLogicalTime(newTime);
}

Status LogicalClock::_passesRateLimiter_inlock(LogicalTime newTime) {
    const unsigned wallClockSecs =
        durationCount<Seconds>(_service->getFastClockSource()->now().toDurationSinceEpoch());
    auto maxAcceptableDrift = static_cast<const unsigned>(maxAcceptableLogicalClockDrift);
    auto newTimeSecs = newTime.asTimestamp().getSecs();

    // Both values are unsigned, so compare them first to avoid wrap-around.
    if ((newTimeSecs > wallClockSecs) && (newTimeSecs - wallClockSecs) > maxAcceptableDrift) {
        return Status(ErrorCodes::ClusterTimeFailsRateLimiter,
                      str::stream() << "New cluster time, " << newTimeSecs
                                    << ", is too far from this node's wall clock time, "
                                    << wallClockSecs
                                    << ".");
    }

    return Status::OK();
}

}  // namespace mongo
