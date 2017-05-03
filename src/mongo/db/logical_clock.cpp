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

LogicalClock::LogicalClock(ServiceContext* service) : _service(service) {}

SignedLogicalTime LogicalClock::getClusterTime() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _clusterTime;
}

void LogicalClock::setTimeProofService(std::unique_ptr<TimeProofService> tps) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _timeProofService = std::move(tps);

    // Ensure a clock with a time proof service cannot have a cluster time without a proof to
    // simplify reasoning about signed logical times.
    if (!_clusterTime.getProof()) {
        _clusterTime = _makeSignedLogicalTime_inlock(_clusterTime.getTime());
    }
}

bool LogicalClock::canVerifyAndSign() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return !!_timeProofService;
}

SignedLogicalTime LogicalClock::_makeSignedLogicalTime_inlock(LogicalTime logicalTime) {
    if (_timeProofService) {
        // TODO: SERVER-28436 Implement KeysCollectionManager
        // Replace dummy keyId with real id from key manager.
        return SignedLogicalTime(
            logicalTime, _timeProofService->getProof(logicalTime, _tempKey), 0);
    }
    return SignedLogicalTime(logicalTime, 0);
}

Status LogicalClock::advanceClusterTime(const SignedLogicalTime& newTime) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (!_timeProofService) {
        return Status(ErrorCodes::CannotVerifyAndSignLogicalTime,
                      "Cannot accept logicalTime: " + newTime.getTime().toString() +
                          ". May not be a part of a sharded cluster");
    }

    const auto& newLogicalTime = newTime.getTime();

    // No need to check proof if no time was given.
    if (newLogicalTime == LogicalTime::kUninitialized) {
        return Status::OK();
    }

    const auto newProof = newTime.getProof();
    // Logical time is only sent if a server's clock can verify and sign logical times, so any
    // received logical times should have proofs.
    invariant(newProof);

    auto res = _timeProofService->checkProof(newLogicalTime, newProof.get(), _tempKey);
    if (res != Status::OK()) {
        return res;
    }

    // The rate limiter check cannot be moved into _advanceClusterTime_inlock to avoid code
    // repetition because it shouldn't be called on direct oplog operations.
    auto rateLimitStatus = _passesRateLimiter_inlock(newTime.getTime());
    if (!rateLimitStatus.isOK()) {
        return rateLimitStatus;
    }

    return _advanceClusterTime_inlock(std::move(newTime));
}

Status LogicalClock::_advanceClusterTime_inlock(SignedLogicalTime newTime) {
    if (newTime.getTime() > _clusterTime.getTime()) {
        _clusterTime = newTime;
    }

    return Status::OK();
}

Status LogicalClock::advanceClusterTimeFromTrustedSource(SignedLogicalTime newTime) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    // The rate limiter check cannot be moved into _advanceClusterTime_inlock to avoid code
    // repetition because it shouldn't be called on direct oplog operations.
    auto rateLimitStatus = _passesRateLimiter_inlock(newTime.getTime());
    if (!rateLimitStatus.isOK()) {
        return rateLimitStatus;
    }

    return _advanceClusterTime_inlock(std::move(newTime));
}

Status LogicalClock::signAndAdvanceClusterTime(LogicalTime newTime) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto newSignedTime = _makeSignedLogicalTime_inlock(newTime);

    return _advanceClusterTime_inlock(std::move(newSignedTime));
}

LogicalTime LogicalClock::reserveTicks(uint64_t nTicks) {

    invariant(nTicks > 0 && nTicks < (1U << 31));

    stdx::lock_guard<stdx::mutex> lock(_mutex);

    LogicalTime clusterTime = _clusterTime.getTime();
    LogicalTime nextClusterTime;

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
    else if (clusterTime.asTimestamp().getInc() >= ((1U << 31) - nTicks)) {

        log() << "Exceeded maximum allowable increment value within one second. Moving clusterTime "
                 "forward to the next second.";

        // Move time forward to the next second
        clusterTime = LogicalTime(Timestamp(clusterTime.asTimestamp().getSecs() + 1, 0));
    }

    // Save the next cluster time.
    clusterTime.addTicks(1);
    nextClusterTime = clusterTime;

    // Add the rest of the requested ticks if needed.
    if (nTicks > 1) {
        clusterTime.addTicks(nTicks - 1);
    }

    _clusterTime = _makeSignedLogicalTime_inlock(clusterTime);
    return nextClusterTime;
}

void LogicalClock::initClusterTimeFromTrustedSource(LogicalTime newTime) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_clusterTime.getTime() == LogicalTime::kUninitialized);
    // Rate limit checks are skipped here so a server with no activity for longer than
    // maxAcceptableLogicalClockDrift seconds can still have its cluster time initialized.
    _clusterTime = _makeSignedLogicalTime_inlock(newTime);
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
