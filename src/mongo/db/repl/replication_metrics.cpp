/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/repl/replication_metrics.h"

#include "mongo/db/commands/server_status.h"
#include "mongo/db/repl/election_reason_counter.h"

namespace mongo {
namespace repl {

namespace {
static const auto getReplicationMetrics = ServiceContext::declareDecoration<ReplicationMetrics>();
}  // namespace

ReplicationMetrics& ReplicationMetrics::get(ServiceContext* svc) {
    return getReplicationMetrics(svc);
}

ReplicationMetrics& ReplicationMetrics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

ReplicationMetrics::ReplicationMetrics() : _electionMetrics() {}

ReplicationMetrics::~ReplicationMetrics() {}

void ReplicationMetrics::incrementNumElectionsCalledForReason(
    TopologyCoordinator::StartElectionReason reason) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    switch (reason) {
        case TopologyCoordinator::StartElectionReason::kStepUpRequest:
        case TopologyCoordinator::StartElectionReason::kStepUpRequestSkipDryRun: {
            ElectionReasonCounter& stepUpCmd = _electionMetrics.getStepUpCmd();
            stepUpCmd.incrementCalled();
            _electionMetrics.setStepUpCmd(stepUpCmd);
            break;
        }
        case TopologyCoordinator::StartElectionReason::kPriorityTakeover: {
            ElectionReasonCounter& priorityTakeover = _electionMetrics.getPriorityTakeover();
            priorityTakeover.incrementCalled();
            _electionMetrics.setPriorityTakeover(priorityTakeover);
            break;
        }
        case TopologyCoordinator::StartElectionReason::kCatchupTakeover: {
            ElectionReasonCounter& catchUpTakeover = _electionMetrics.getCatchUpTakeover();
            catchUpTakeover.incrementCalled();
            _electionMetrics.setCatchUpTakeover(catchUpTakeover);
            break;
        }
        case TopologyCoordinator::StartElectionReason::kElectionTimeout: {
            ElectionReasonCounter& electionTimeout = _electionMetrics.getElectionTimeout();
            electionTimeout.incrementCalled();
            _electionMetrics.setElectionTimeout(electionTimeout);
            break;
        }
        case TopologyCoordinator::StartElectionReason::kSingleNodePromptElection: {
            ElectionReasonCounter& freezeTimeout = _electionMetrics.getFreezeTimeout();
            freezeTimeout.incrementCalled();
            _electionMetrics.setFreezeTimeout(freezeTimeout);
            break;
        }
    }
}

void ReplicationMetrics::incrementNumStepDownsCausedByHigherTerm() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _electionMetrics.setNumStepDownsCausedByHigherTerm(
        _electionMetrics.getNumStepDownsCausedByHigherTerm() + 1);
}

void ReplicationMetrics::incrementNumCatchUps() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _electionMetrics.setNumCatchUps(_electionMetrics.getNumCatchUps() + 1);
}

int ReplicationMetrics::getNumStepUpCmdsCalled_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getStepUpCmd().getCalled();
}

int ReplicationMetrics::getNumPriorityTakeoversCalled_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getPriorityTakeover().getCalled();
}

int ReplicationMetrics::getNumCatchUpTakeoversCalled_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getCatchUpTakeover().getCalled();
}

int ReplicationMetrics::getNumElectionTimeoutsCalled_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getElectionTimeout().getCalled();
}

int ReplicationMetrics::getNumFreezeTimeoutsCalled_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getFreezeTimeout().getCalled();
}

int ReplicationMetrics::getNumStepDownsCausedByHigherTerm_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getNumStepDownsCausedByHigherTerm();
}

int ReplicationMetrics::getNumCatchUps_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getNumCatchUps();
}

BSONObj ReplicationMetrics::getElectionMetricsBSON() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.toBSON();
}

class ReplicationMetrics::ElectionMetricsSSS : public ServerStatusSection {
public:
    ElectionMetricsSSS() : ServerStatusSection("electionMetrics") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        ReplicationMetrics& replicationMetrics = ReplicationMetrics::get(opCtx);

        return replicationMetrics.getElectionMetricsBSON();
    }
} electionMetricsSSS;

}  // namespace repl
}  // namespace mongo
