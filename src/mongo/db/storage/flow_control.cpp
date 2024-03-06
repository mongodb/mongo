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


#include <absl/container/node_hash_map.h>
#include <fmt/format.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/flow_control_ticketholder.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/lock_stats.h"
#include "mongo/db/repl/member_data.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/flow_control.h"
#include "mongo/db/storage/flow_control_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


#define DEBUG_LOG_LEVEL 4

namespace mongo {

MONGO_FAIL_POINT_DEFINE(flowControlTicketOverride);

namespace {
const auto getFlowControl = ServiceContext::declareDecoration<std::unique_ptr<FlowControl>>();

int multiplyWithOverflowCheck(double term1, double term2, int maxValue) {
    if (term1 == 0.0 || term2 == 0.0) {
        // Early return to avoid any divide by zero errors.
        return 0;
    }

    if (static_cast<double>(std::numeric_limits<int>::max()) / term2 < term1) {
        // Multiplying term1 and term2 would overflow, return maxValue.
        return maxValue;
    }

    double ret = term1 * term2;
    if (ret >= maxValue) {
        return maxValue;
    }

    return static_cast<int>(ret);
}

std::uint64_t getLagMillis(Date_t myLastApplied, Date_t lastCommitted) {
    if (!myLastApplied.isFormattable() || !lastCommitted.isFormattable()) {
        return 0;
    }
    return static_cast<std::uint64_t>(durationCount<Milliseconds>(myLastApplied - lastCommitted));
}

std::uint64_t getThresholdLagMillis() {
    return static_cast<std::uint64_t>(1000.0 * gFlowControlThresholdLagPercentage.load() *
                                      gFlowControlTargetLagSeconds.load());
}

Timestamp getMedianAppliedTimestamp(const std::vector<repl::MemberData>& sortedMemberData) {
    if (sortedMemberData.size() == 0) {
        return Timestamp::min();
    }

    const int sustainerIdx = sortedMemberData.size() / 2;
    return sortedMemberData[sustainerIdx].getLastAppliedOpTime().getTimestamp();
}

/**
 * Sanity checks whether the successive queries of topology data are comparable for doing a flow
 * control calculation. In particular, the number of members must be the same and the median
 * applier's timestamp must not go backwards.
 */
bool sustainerAdvanced(const std::vector<repl::MemberData>& prevMemberData,
                       const std::vector<repl::MemberData>& currMemberData) {
    if (currMemberData.size() == 0 || currMemberData.size() != prevMemberData.size()) {
        LOGV2_WARNING(22223,
                      "Flow control detected a change in topology",
                      "prevSize"_attr = prevMemberData.size(),
                      "currSize"_attr = currMemberData.size());
        return false;
    }

    auto currSustainerAppliedTs = getMedianAppliedTimestamp(currMemberData);
    auto prevSustainerAppliedTs = getMedianAppliedTimestamp(prevMemberData);

    if (currSustainerAppliedTs < prevSustainerAppliedTs) {
        LOGV2_WARNING(22224,
                      "Flow control's sustainer time decreased",
                      "prevApplied"_attr = prevSustainerAppliedTs,
                      "currApplied"_attr = currSustainerAppliedTs);
        return false;
    }

    return true;
}
}  // namespace

FlowControl::FlowControl(repl::ReplicationCoordinator* replCoord)
    : ServerStatusSection("flowControl"),
      _replCoord(replCoord),
      _lastTimeSustainerAdvanced(Date_t::now()) {}

FlowControl::FlowControl(ServiceContext* service, repl::ReplicationCoordinator* replCoord)
    : ServerStatusSection("flowControl"),
      _replCoord(replCoord),
      _lastTimeSustainerAdvanced(Date_t::now()) {
    // Initialize _lastTargetTicketsPermitted to maximum tickets to make sure flow control doesn't
    // cause a slow start on start up.
    FlowControlTicketholder::set(service, std::make_unique<FlowControlTicketholder>(kMaxTickets));

    _jobAnchor = service->getPeriodicRunner()->makeJob(
        {"FlowControlRefresher",
         [this](Client* client) {
             FlowControlTicketholder::get(client->getServiceContext())->refreshTo(getNumTickets());
         },
         Seconds(1),
         // TODO(SERVER-74657): Please revisit if this periodic job could be made killable.
         false /*isKillableByStepdown*/});
    _jobAnchor.start();
}

FlowControl* FlowControl::get(ServiceContext* service) {
    return getFlowControl(service).get();
}

FlowControl* FlowControl::get(ServiceContext& service) {
    return getFlowControl(service).get();
}

FlowControl* FlowControl::get(OperationContext* ctx) {
    return get(ctx->getClient()->getServiceContext());
}

void FlowControl::set(ServiceContext* service, std::unique_ptr<FlowControl> flowControl) {
    auto& globalFlow = getFlowControl(service);
    globalFlow = std::move(flowControl);
}

void FlowControl::shutdown(ServiceContext* service) {
    auto& globalFlow = getFlowControl(service);
    if (globalFlow) {
        globalFlow->_jobAnchor.stop();
        globalFlow.reset();
    }
}

/**
 * Returns -1.0 if there are not enough samples.
 */
double FlowControl::_getLocksPerOp() {
    // Primaries sample the number of operations it has applied alongside how many global lock
    // acquisitions (in MODE_IX) it took to process those operations. This method looks at the two
    // most recent samples and returns the ratio of global lock acquisitions to operations processed
    // for the current client workload.
    Sample backTwo;
    Sample backOne;
    std::size_t numSamples;
    {
        stdx::lock_guard<Latch> lk(_sampledOpsMutex);
        numSamples = _sampledOpsApplied.size();
        if (numSamples >= 2) {
            backTwo = _sampledOpsApplied[numSamples - 2];
            backOne = _sampledOpsApplied[numSamples - 1];
        } else {
            _lastLocksPerOp.store(0.0);
            return -1.0;
        }
    }

    auto ret = (double)(std::get<2>(backOne) - std::get<2>(backTwo)) /
        (double)(std::get<1>(backOne) - std::get<1>(backTwo));
    _lastLocksPerOp.store(ret);
    return ret;
}

BSONObj FlowControl::generateSection(OperationContext* opCtx,
                                     const BSONElement& configElement) const {
    BSONObjBuilder bob;
    // Most of these values are only computed and meaningful when flow control is enabled.
    bob.append("enabled", gFlowControlEnabled.load());
    bob.append("targetRateLimit", _lastTargetTicketsPermitted.load());
    bob.append("timeAcquiringMicros",
               FlowControlTicketholder::get(opCtx)->totalTimeAcquiringMicros());
    // Ensure sufficient significant figures of locksPerOp are reported in FTDC, which stores data
    // as integers.
    bob.append("locksPerKiloOp", _lastLocksPerOp.load() * 1000);
    bob.append("sustainerRate", _lastSustainerAppliedCount.load());
    bob.append("isLagged", _isLagged.load());
    bob.append("isLaggedCount", _isLaggedCount.load());
    bob.append("isLaggedTimeMicros", _isLaggedTimeMicros.load());

    return bob.obj();
}

void FlowControl::disableUntil(Date_t deadline) {
    _disableUntil.store(deadline);
}

/**
 * Advance the `_*MemberData` fields and sort the new data by the element's last applied optime.
 */
void FlowControl::_updateTopologyData() {
    _prevMemberData = _currMemberData;
    _currMemberData = _replCoord->getMemberData();

    // Sort MemberData with the 0th index being the node with the lowest applied optime.
    std::sort(_currMemberData.begin(),
              _currMemberData.end(),
              [](const repl::MemberData& left, const repl::MemberData& right) -> bool {
                  return left.getLastAppliedOpTime() < right.getLastAppliedOpTime();
              });
}

int FlowControl::_calculateNewTicketsForLag(const std::vector<repl::MemberData>& prevMemberData,
                                            const std::vector<repl::MemberData>& currMemberData,
                                            std::int64_t locksUsedLastPeriod,
                                            double locksPerOp,
                                            std::uint64_t lagMillis,
                                            std::uint64_t thresholdLagMillis) {
    invariant(lagMillis >= thresholdLagMillis);
    using namespace fmt::literals;

    const auto currSustainerAppliedTs = getMedianAppliedTimestamp(currMemberData);
    const auto prevSustainerAppliedTs = getMedianAppliedTimestamp(prevMemberData);
    invariant(prevSustainerAppliedTs <= currSustainerAppliedTs,
              "PrevSustainer: {} CurrSustainer: {}"_format(prevSustainerAppliedTs.toString(),
                                                           currSustainerAppliedTs.toString()));

    const std::int64_t sustainerAppliedCount =
        _approximateOpsBetween(prevSustainerAppliedTs, currSustainerAppliedTs);
    LOGV2_DEBUG(22218,
                DEBUG_LOG_LEVEL,
                " PrevApplied: {prevSustainerAppliedTs} CurrApplied: {currSustainerAppliedTs} "
                "NumSustainerApplied: {sustainerAppliedCount}",
                "prevSustainerAppliedTs"_attr = prevSustainerAppliedTs,
                "currSustainerAppliedTs"_attr = currSustainerAppliedTs,
                "sustainerAppliedCount"_attr = sustainerAppliedCount);
    if (sustainerAppliedCount > 0) {
        _lastTimeSustainerAdvanced = Date_t::now();
    } else {
        auto warnThresholdSeconds = gFlowControlWarnThresholdSeconds.load();
        const auto now = Date_t::now();
        if (warnThresholdSeconds > 0 &&
            now - _lastTimeSustainerAdvanced >= Seconds(warnThresholdSeconds)) {
            LOGV2_WARNING(22225,
                          "Flow control is engaged and the sustainer point is not moving. Please "
                          "check the health of all secondaries.");

            // Log once every `warnThresholdSeconds` seconds.
            _lastTimeSustainerAdvanced = now;
        }
    }

    _lastSustainerAppliedCount.store(static_cast<int>(sustainerAppliedCount));
    if (sustainerAppliedCount == -1) {
        // We don't know how many ops the sustainer applied. Hand out less tickets than were
        // used in the last period.
        return std::min(static_cast<int>(locksUsedLastPeriod / 2.0), kMaxTickets);
    }

    // Given a "sustainer rate", this function wants to calculate what fraction the primary should
    // accept writes at to allow secondaries to catch up.
    //
    // When the commit point lag is similar to the threshold, the function will output an exponent
    // close to 0 resulting in a coefficient close to 1. In this state, the primary will accept
    // writes roughly on pace with the sustainer rate.
    //
    // As another example, as the commit point lag increases to say, 2x the threshold, the exponent
    // will be close to 1. In this case the primary will accept writes at roughly the
    // `gFlowControlDecayConstant` (original default of 0.5).
    auto exponent = static_cast<double>(lagMillis - thresholdLagMillis) /
        static_cast<double>(std::max(thresholdLagMillis, static_cast<std::uint64_t>(1)));
    invariant(exponent >= 0.0);

    const double reduce = pow(gFlowControlDecayConstant.load(), exponent);

    // The fudge factor, by default is 0.95. Keeping this value close to one reduces oscillations in
    // an environment where secondaries consistently process operations slower than the primary.
    double sustainerAppliedPenalty =
        sustainerAppliedCount * reduce * gFlowControlFudgeFactor.load();
    LOGV2_DEBUG(22219,
                DEBUG_LOG_LEVEL,
                "Sustainer: {sustainerAppliedCount} LagMillis: {lagMillis} Threshold lag: "
                "{thresholdLagMillis} Exponent: {exponent} Reduce: {reduce} Penalty: "
                "{sustainerAppliedPenalty}",
                "sustainerAppliedCount"_attr = sustainerAppliedCount,
                "lagMillis"_attr = lagMillis,
                "thresholdLagMillis"_attr = thresholdLagMillis,
                "exponent"_attr = exponent,
                "reduce"_attr = reduce,
                "sustainerAppliedPenalty"_attr = sustainerAppliedPenalty);

    return multiplyWithOverflowCheck(locksPerOp, sustainerAppliedPenalty, kMaxTickets);
}

int FlowControl::getNumTickets(Date_t now) {
    // Flow control can be disabled until a certain deadline is passed.
    const Date_t disabledUntil = _disableUntil.load();
    if (now < disabledUntil) {
        return kMaxTickets;
    }

    // Flow Control is only enabled on nodes that can accept writes.
    const bool canAcceptWrites = _replCoord->canAcceptNonLocalWrites();

    if (auto sfp = flowControlTicketOverride.scoped(); MONGO_unlikely(sfp.isActive())) {
        int numTickets = sfp.getData().getIntField("numTickets");
        if (numTickets > 0 && canAcceptWrites) {
            return numTickets;
        }
    }

    // It's important to update the topology on each iteration.
    _updateTopologyData();
    const repl::OpTimeAndWallTime myLastApplied = _replCoord->getMyLastAppliedOpTimeAndWallTime();
    const repl::OpTimeAndWallTime lastCommitted = _replCoord->getLastCommittedOpTimeAndWallTime();
    const double locksPerOp = _getLocksPerOp();
    const std::int64_t locksUsedLastPeriod = _getLocksUsedLastPeriod();

    if (serverGlobalParams.enableMajorityReadConcern == false ||
        gFlowControlEnabled.load() == false || canAcceptWrites == false || locksPerOp < 0.0) {
        _trimSamples(std::min(lastCommitted.opTime.getTimestamp(),
                              getMedianAppliedTimestamp(_prevMemberData)));
        return kMaxTickets;
    }

    int ret = 0;
    const auto thresholdLagMillis = getThresholdLagMillis();

    // Successive lastCommitted and lastApplied wall clock time recordings are not guaranteed to be
    // monotonically increasing. Recordings that satisfy the following check result in a negative
    // value for lag, so ignore them.
    const bool ignoreWallTimes = lastCommitted.wallTime > myLastApplied.wallTime;

    // _approximateOpsBetween will return -1 if the input timestamps are in the same "bucket".
    // This is an indication that there are very few ops between the two timestamps.
    //
    // Don't let the no-op writer on idle systems fool the sophisticated "is the replica set
    // lagged" classifier.
    const bool isHealthy = !ignoreWallTimes &&
        (getLagMillis(myLastApplied.wallTime, lastCommitted.wallTime) < thresholdLagMillis ||
         _approximateOpsBetween(lastCommitted.opTime.getTimestamp(),
                                myLastApplied.opTime.getTimestamp()) == -1);

    if (isHealthy) {
        // The add/multiply technique is used to ensure ticket allocation can ramp up quickly,
        // particularly if there were very few tickets to begin with.
        ret = multiplyWithOverflowCheck(_lastTargetTicketsPermitted.load() +
                                            gFlowControlTicketAdderConstant.load(),
                                        gFlowControlTicketMultiplierConstant.load(),
                                        kMaxTickets);
        _lastTimeSustainerAdvanced = Date_t::now();
        if (_isLagged.load()) {
            _isLagged.store(false);
            auto waitTime = curTimeMicros64() - _startWaitTime;
            _isLaggedTimeMicros.fetchAndAddRelaxed(waitTime);
        }
    } else if (!ignoreWallTimes && sustainerAdvanced(_prevMemberData, _currMemberData)) {
        // Expected case where flow control has meaningful data from the last period to make a new
        // calculation.
        ret =
            _calculateNewTicketsForLag(_prevMemberData,
                                       _currMemberData,
                                       locksUsedLastPeriod,
                                       locksPerOp,
                                       getLagMillis(myLastApplied.wallTime, lastCommitted.wallTime),
                                       thresholdLagMillis);
        if (!_isLagged.load()) {
            _isLagged.store(true);
            _isLaggedCount.fetchAndAddRelaxed(1);
            _startWaitTime = curTimeMicros64();
        }
    } else {
        // Unexpected case where consecutive readings from the topology state don't meet some basic
        // expectations, or where the lag measure is nonsensical.
        ret = _lastTargetTicketsPermitted.load();
        _lastTimeSustainerAdvanced = Date_t::now();
        // Since this case does not give conclusive evidence that isLagged could have meaningfully
        // transitioned from true to false, it does not make sense to update the _isLagged*
        // variables here.
    }

    ret = std::max(ret, gFlowControlMinTicketsPerSecond.load());

    LOGV2_DEBUG(22220,
                DEBUG_LOG_LEVEL,
                "FlowControl debug.",
                "isLagged"_attr = (_isLagged.load() ? "true" : "false"),
                "currlagMillis"_attr = getLagMillis(myLastApplied.wallTime, lastCommitted.wallTime),
                "opsLagged"_attr = _approximateOpsBetween(lastCommitted.opTime.getTimestamp(),
                                                          myLastApplied.opTime.getTimestamp()),
                "granting"_attr = ret,
                "lastGranted"_attr = _lastTargetTicketsPermitted.load(),
                "lastSustainerApplied"_attr = _lastSustainerAppliedCount.load(),
                "acquisitionsSinceLastCheck"_attr = locksUsedLastPeriod,
                "locksPerOp"_attr = _lastLocksPerOp.load(),
                "countOfLaggedPeriods"_attr = _isLaggedCount.load(),
                "totalDurationOfLaggedPeriods"_attr = _isLaggedTimeMicros.load());

    _lastTargetTicketsPermitted.store(ret);

    _trimSamples(
        std::min(lastCommitted.opTime.getTimestamp(), getMedianAppliedTimestamp(_prevMemberData)));

    return ret;
}

std::int64_t FlowControl::_approximateOpsBetween(Timestamp prevTs, Timestamp currTs) {
    std::int64_t prevApplied = -1;
    std::int64_t currApplied = -1;

    stdx::lock_guard<Latch> lk(_sampledOpsMutex);
    for (auto&& sample : _sampledOpsApplied) {
        if (prevApplied == -1 && prevTs.asULL() <= std::get<0>(sample)) {
            prevApplied = std::get<1>(sample);
        }

        if (currTs.asULL() <= std::get<0>(sample)) {
            currApplied = std::get<1>(sample);
            break;
        }
    }

    if (prevApplied != -1 && currApplied == -1) {
        currApplied = std::get<1>(_sampledOpsApplied[_sampledOpsApplied.size() - 1]);
    }

    if (prevApplied != -1 && currApplied != -1) {
        return currApplied - prevApplied;
    }

    return -1;
}

void FlowControl::sample(Timestamp timestamp, std::uint64_t opsApplied) {
    if (serverGlobalParams.enableMajorityReadConcern == false) {
        return;
    }

    stdx::lock_guard<Latch> lk(_sampledOpsMutex);
    _numOpsSinceStartup += opsApplied;
    if (_numOpsSinceStartup - _lastSample <
        static_cast<std::size_t>(gFlowControlSamplePeriod.load())) {
        // Naively sample once every 1000 or so operations.
        return;
    }

    if (_sampledOpsApplied.size() > 0 &&
        static_cast<std::uint64_t>(timestamp.asULL()) <= std::get<0>(_sampledOpsApplied.back())) {
        // The optime generator mutex is no longer held, these timestamps can come in out of order.
        return;
    }

    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);

    _lastSample = _numOpsSinceStartup;

    const auto lockAcquisitions = stats.get(resourceIdGlobal, LockMode::MODE_IX).numAcquisitions;
    LOGV2_DEBUG(22221,
                DEBUG_LOG_LEVEL,
                "Sampling. Time: {timestamp} Applied: {numOpsSinceStartup} LockAcquisitions: "
                "{lockAcquisitions}",
                "timestamp"_attr = timestamp,
                "numOpsSinceStartup"_attr = _numOpsSinceStartup,
                "lockAcquisitions"_attr = lockAcquisitions);

    if (_sampledOpsApplied.size() <
        static_cast<std::deque<Sample>::size_type>(gFlowControlMaxSamples)) {
        _sampledOpsApplied.emplace_back(
            static_cast<std::uint64_t>(timestamp.asULL()), _numOpsSinceStartup, lockAcquisitions);
    } else {
        // At ~24 bytes per sample, 1 million samples is ~24MB of memory. Instead of growing
        // proportionally to replication lag, FlowControl opts to lose resolution (the number of
        // operations between recorded samples increases). Hitting the sample limit implies there's
        // replication lag. When there's replication lag, the oldest values are actively being used
        // to compute the number of tickets to allocate. FlowControl intentionally prioritizes the
        // oldest entries as those are, by definition, the most valuable when there is lag. Instead,
        // we choose to lose resolution at the newest value.
        _sampledOpsApplied[_sampledOpsApplied.size() - 1] = {
            static_cast<std::uint64_t>(timestamp.asULL()), _numOpsSinceStartup, lockAcquisitions};
    }
}

void FlowControl::_trimSamples(const Timestamp trimTo) {
    int numTrimmed = 0;
    stdx::lock_guard<Latch> lk(_sampledOpsMutex);
    // Always leave at least two samples for calculating `locksPerOp`.
    while (_sampledOpsApplied.size() > 2 &&
           std::get<0>(_sampledOpsApplied.front()) < trimTo.asULL()) {
        _sampledOpsApplied.pop_front();
        ++numTrimmed;
    }

    LOGV2_DEBUG(22222,
                DEBUG_LOG_LEVEL,
                "Trimmed samples. Num: {numTrimmed}",
                "numTrimmed"_attr = numTrimmed);
}

int64_t FlowControl::_getLocksUsedLastPeriod() {
    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);

    int64_t counter = stats.get(resourceIdGlobal, LockMode::MODE_IX).numAcquisitions;
    int64_t ret = counter - _lastPollLockAcquisitions;
    _lastPollLockAcquisitions = counter;

    return ret;
}

}  // namespace mongo
