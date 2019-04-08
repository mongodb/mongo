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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage
#define DEBUG_LOG_LEVEL 4

#include "mongo/platform/basic.h"

#include "mongo/db/storage/flow_control.h"

#include <algorithm>
#include <limits>

#include "mongo/db/concurrency/flow_control_ticketholder.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/repl/member_data.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/flow_control_parameters_gen.h"
#include "mongo/util/background.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
const auto getFlowControl = ServiceContext::declareDecoration<std::unique_ptr<FlowControl>>();

int multiplyWithOverflowCheck(double term1, double term2, int maxValue) {
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
}  // namespace

FlowControl::FlowControl(ServiceContext* service, repl::ReplicationCoordinator* replCoord)
    : ServerStatusSection("flowControl"), _replCoord(replCoord) {
    _lastTargetTicketsPermitted.store(0);
    _lastLocksPerOp.store(0.0);
    _lastSustainerAppliedCount.store(0);

    FlowControlTicketholder::set(service, stdx::make_unique<FlowControlTicketholder>(1000));

    service->getPeriodicRunner()->scheduleJob(
        {"FlowControlRefresher",
         [this](Client* client) {
             FlowControlTicketholder::get(client->getServiceContext())->refreshTo(getNumTickets());
         },
         Seconds(1)});
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

double FlowControl::_getMyLocksPerOp() {
    // Primaries sample the number of operations it has applied alongside how many global lock
    // acquisitions (in MODE_IX) it took to process those operations. This method looks at the two
    // most recent samples and returns the ratio of global lock acquisitions to operations processed
    // for the current client workload.
    Sample backTwo;
    Sample backOne;
    std::size_t numSamples;
    {
        stdx::lock_guard<stdx::mutex> lk(_sampledOpsMutex);
        numSamples = _sampledOpsApplied.size();
        if (numSamples >= 2) {
            backTwo = _sampledOpsApplied[numSamples - 2];
            backOne = _sampledOpsApplied[numSamples - 1];
        }
    }

    return (double)(std::get<2>(backOne) - std::get<2>(backTwo)) /
        (double)(std::get<1>(backOne) - std::get<1>(backTwo));
}

BSONObj FlowControl::generateSection(OperationContext* opCtx,
                                     const BSONElement& configElement) const {
    const int lagSecs = _replCoord->getMyLastAppliedOpTime().getSecs() -
        _replCoord->getLastCommittedOpTime().getSecs();

    BSONObjBuilder bob;
    bob.append("targetRateLimit", _lastTargetTicketsPermitted.load());
    bob.append("timeAcquiringMicros", 0);
    bob.append("locksPerOp", _lastLocksPerOp.load());
    bob.append("sustainerRate", _lastSustainerAppliedCount.load());
    bob.append("isLagged", lagSecs >= gFlowControlTargetLagSeconds.load());

    return bob.obj();
}

int FlowControl::getNumTickets() {
    const int maxTickets = 1000 * 1000 * 1000;
    if (serverGlobalParams.enableMajorityReadConcern == false || gFlowControlEnabled == false) {
        return maxTickets;
    }

    const Timestamp myLastApplied = _replCoord->getMyLastAppliedOpTime().getTimestamp();
    const Timestamp lastCommitted = _replCoord->getLastCommittedOpTime().getTimestamp();
    const int lagSecs = myLastApplied.getSecs() - lastCommitted.getSecs();

    bool areWeLagged = lagSecs >= gFlowControlTargetLagSeconds.load();
    if (areWeLagged && _approximateOpsBetween(lastCommitted.asULL(), myLastApplied.asULL()) == -1) {
        // _approximateOpsBetween will return -1 if the input timestamps are in the same
        // "bucket". This is an indication that there are very few ops between the two timestamps.
        //
        // Don't let the no-op writer on idle systems fool the sophisticated "is the replica set
        // lagged" classifier.
        areWeLagged = false;
    }

    std::vector<repl::MemberData> currMemberData = _replCoord->getMemberData();
    // Sort MemberData with the 0th index being the node with the lowest applied optime.
    std::sort(currMemberData.begin(),
              currMemberData.end(),
              [](const repl::MemberData& left, const repl::MemberData& right) -> bool {
                  return left.getLastAppliedOpTime() < right.getLastAppliedOpTime();
              });

    int ret = 0;
    auto locksUsedLastPeriod = getLocksUsedLastPeriod();
    if (areWeLagged) {
        std::int64_t sustainerAppliedCount = -1;
        if (currMemberData.size() > 0 && currMemberData.size() == _prevMemberData.size()) {
            // The index into the array of sorted MemberData that represents the sustaining node.
            int sustainerIdx = currMemberData.size() / 2;

            auto currSustainerAppliedTs =
                currMemberData[sustainerIdx].getLastAppliedOpTime().getTimestamp();
            auto prevSustainerAppliedTs =
                _prevMemberData[sustainerIdx].getLastAppliedOpTime().getTimestamp();

            sustainerAppliedCount = _approximateOpsBetween(prevSustainerAppliedTs.asULL(),
                                                           currSustainerAppliedTs.asULL());
            LOG(DEBUG_LOG_LEVEL) << " PrevApplied: " << prevSustainerAppliedTs
                                 << " CurrApplied: " << currSustainerAppliedTs
                                 << " NumSustainerApplied: " << sustainerAppliedCount;
        } else {
            error() << "ERRORING FLOW CONTROL. Size diff.";
        }

        _lastSustainerAppliedCount.store(static_cast<int>(sustainerAppliedCount));
        if (sustainerAppliedCount > -1) {
            // We know how many ops the sustainer applied, use that for calculating the new number
            // of tickets.
            const double sustainerAppliedPenalty = (double)(sustainerAppliedCount) / 2.0;
            _lastTargetTicketsPermitted.store(static_cast<int>(sustainerAppliedPenalty));
            const auto locksPerOp = _getMyLocksPerOp();
            _lastLocksPerOp.store(locksPerOp);
            LOG(DEBUG_LOG_LEVEL) << "LocksPerOp: " << locksPerOp
                                 << " Sustainer: " << sustainerAppliedCount
                                 << " Target: " << sustainerAppliedPenalty;
            ret = multiplyWithOverflowCheck(locksPerOp, sustainerAppliedPenalty, maxTickets);
        } else {
            // We don't know how many ops the sustainer applied. Hand out less tickets than were
            // used in the last period.
            if (locksUsedLastPeriod / 2.0 <= static_cast<double>(maxTickets))
                ret = static_cast<int>(locksUsedLastPeriod / 2.0);
            else
                ret = maxTickets;
            _lastTargetTicketsPermitted.store(-1);
        }

        // Always have at least 100 tickets.
        ret = std::max(ret, 100);
    } else {
        ret = multiplyWithOverflowCheck(_lastTargetTicketsPermitted.load() + 1000, 1.1, maxTickets);
    }

    _prevMemberData = std::move(currMemberData);

    // This is a paranoid check. The evaluation of `ret` is non-trivial and thus proving the
    // invariant that `ret <= maxTickets` remains to be true after code modification is not an
    // exercise readers would be delighted to spend their time on.
    ret = std::min(ret, maxTickets);

    LOG(DEBUG_LOG_LEVEL) << "Are lagged? " << areWeLagged << " Prev lag: " << _prevLagSecs
                         << " Curr lag: " << lagSecs << " OpsLagged: "
                         << _approximateOpsBetween(lastCommitted.asULL(), myLastApplied.asULL())
                         << " Granting: " << ret
                         << " Last granted: " << _lastTargetTicketsPermitted.load()
                         << " Acquisitions since last check: " << locksUsedLastPeriod;

    _lastTargetTicketsPermitted.store(ret);
    _prevLagSecs = lagSecs;
    return ret;
}

std::int64_t FlowControl::_approximateOpsBetween(std::uint64_t prevTs, std::uint64_t currTs) {
    std::int64_t prevApplied = -1;
    std::int64_t currApplied = -1;

    stdx::lock_guard<stdx::mutex> lk(_sampledOpsMutex);
    for (auto&& sample : _sampledOpsApplied) {
        if (prevApplied == -1 && prevTs < std::get<0>(sample)) {
            prevApplied = std::get<1>(sample);
        }

        if (currApplied == -1 && currTs < std::get<0>(sample)) {
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
    if (serverGlobalParams.enableMajorityReadConcern == false || gFlowControlEnabled == false) {
        // TODO SERVER-39616: Remove this feature flag such that flow control can be turned on/off
        // at runtime.
        return;
    }

    stdx::lock_guard<stdx::mutex> lk(_sampledOpsMutex);
    _numOpsSinceStartup += opsApplied;
    if (_numOpsSinceStartup - _lastSample < 1000) {
        // Naively sample once every 1000 or so operations.
        return;
    }

    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);

    _lastSample = _numOpsSinceStartup;

    const auto lockAcquisitions = stats.get(resourceIdGlobal, LockMode::MODE_IX).numAcquisitions;
    LOG(DEBUG_LOG_LEVEL) << "Sampling. Time: " << timestamp << " Applied: " << _numOpsSinceStartup
                         << " LockAcquisitions: " << lockAcquisitions;
    _sampledOpsApplied.emplace_back(
        static_cast<std::uint64_t>(timestamp.asULL()), _numOpsSinceStartup, lockAcquisitions);
}

int64_t FlowControl::getLocksUsedLastPeriod() {
    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);

    int64_t counter = stats.get(resourceIdGlobal, LockMode::MODE_IX).numAcquisitions;
    int64_t ret = counter - _lastPollLockAcquisitions;
    _lastPollLockAcquisitions = counter;

    _lastLocksPerOp.store(ret);

    return ret;
}

}  // namespace mongo
