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

ReplicationMetrics::ReplicationMetrics()
    : _electionMetrics(ElectionReasonCounter(),
                       ElectionReasonCounter(),
                       ElectionReasonCounter(),
                       ElectionReasonCounter(),
                       ElectionReasonCounter()) {}

ReplicationMetrics::~ReplicationMetrics() {}

void ReplicationMetrics::incrementNumElectionsCalledForReason(
    TopologyCoordinator::StartElectionReason reason) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    switch (reason) {
        case TopologyCoordinator::StartElectionReason::kStepUpRequest:
        case TopologyCoordinator::StartElectionReason::kStepUpRequestSkipDryRun: {
            ElectionReasonCounter& stepUpCmd = _electionMetrics.getStepUpCmd();
            stepUpCmd.incrementCalled();
            break;
        }
        case TopologyCoordinator::StartElectionReason::kPriorityTakeover: {
            ElectionReasonCounter& priorityTakeover = _electionMetrics.getPriorityTakeover();
            priorityTakeover.incrementCalled();
            break;
        }
        case TopologyCoordinator::StartElectionReason::kCatchupTakeover: {
            ElectionReasonCounter& catchUpTakeover = _electionMetrics.getCatchUpTakeover();
            catchUpTakeover.incrementCalled();
            break;
        }
        case TopologyCoordinator::StartElectionReason::kElectionTimeout: {
            ElectionReasonCounter& electionTimeout = _electionMetrics.getElectionTimeout();
            electionTimeout.incrementCalled();
            break;
        }
        case TopologyCoordinator::StartElectionReason::kSingleNodePromptElection: {
            ElectionReasonCounter& freezeTimeout = _electionMetrics.getFreezeTimeout();
            freezeTimeout.incrementCalled();
            break;
        }
    }
}

void ReplicationMetrics::incrementNumElectionsSuccessfulForReason(
    TopologyCoordinator::StartElectionReason reason) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    switch (reason) {
        case TopologyCoordinator::StartElectionReason::kStepUpRequest:
        case TopologyCoordinator::StartElectionReason::kStepUpRequestSkipDryRun: {
            ElectionReasonCounter& stepUpCmd = _electionMetrics.getStepUpCmd();
            stepUpCmd.incrementSuccessful();
            break;
        }
        case TopologyCoordinator::StartElectionReason::kPriorityTakeover: {
            ElectionReasonCounter& priorityTakeover = _electionMetrics.getPriorityTakeover();
            priorityTakeover.incrementSuccessful();
            break;
        }
        case TopologyCoordinator::StartElectionReason::kCatchupTakeover: {
            ElectionReasonCounter& catchUpTakeover = _electionMetrics.getCatchUpTakeover();
            catchUpTakeover.incrementSuccessful();
            break;
        }
        case TopologyCoordinator::StartElectionReason::kElectionTimeout: {
            ElectionReasonCounter& electionTimeout = _electionMetrics.getElectionTimeout();
            electionTimeout.incrementSuccessful();
            break;
        }
        case TopologyCoordinator::StartElectionReason::kSingleNodePromptElection: {
            ElectionReasonCounter& freezeTimeout = _electionMetrics.getFreezeTimeout();
            freezeTimeout.incrementSuccessful();
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

void ReplicationMetrics::incrementNumCatchUpsConcludedForReason(
    ReplicationCoordinator::PrimaryCatchUpConclusionReason reason) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    switch (reason) {
        case ReplicationCoordinator::PrimaryCatchUpConclusionReason::kSucceeded:
            _electionMetrics.setNumCatchUpsSucceeded(_electionMetrics.getNumCatchUpsSucceeded() +
                                                     1);
            break;
        case ReplicationCoordinator::PrimaryCatchUpConclusionReason::kAlreadyCaughtUp:
            _electionMetrics.setNumCatchUpsAlreadyCaughtUp(
                _electionMetrics.getNumCatchUpsAlreadyCaughtUp() + 1);
            break;
        case ReplicationCoordinator::PrimaryCatchUpConclusionReason::kSkipped:
            _electionMetrics.setNumCatchUpsSkipped(_electionMetrics.getNumCatchUpsSkipped() + 1);
            break;
        case ReplicationCoordinator::PrimaryCatchUpConclusionReason::kTimedOut:
            _electionMetrics.setNumCatchUpsTimedOut(_electionMetrics.getNumCatchUpsTimedOut() + 1);
            break;
        case ReplicationCoordinator::PrimaryCatchUpConclusionReason::kFailedWithError:
            _electionMetrics.setNumCatchUpsFailedWithError(
                _electionMetrics.getNumCatchUpsFailedWithError() + 1);
            break;
        case ReplicationCoordinator::PrimaryCatchUpConclusionReason::kFailedWithNewTerm:
            _electionMetrics.setNumCatchUpsFailedWithNewTerm(
                _electionMetrics.getNumCatchUpsFailedWithNewTerm() + 1);
            break;
        case ReplicationCoordinator::PrimaryCatchUpConclusionReason::
            kFailedWithReplSetAbortPrimaryCatchUpCmd:
            _electionMetrics.setNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd(
                _electionMetrics.getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd() + 1);
            break;
    }
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

int ReplicationMetrics::getNumStepUpCmdsSuccessful_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getStepUpCmd().getSuccessful();
}

int ReplicationMetrics::getNumPriorityTakeoversSuccessful_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getPriorityTakeover().getSuccessful();
}

int ReplicationMetrics::getNumCatchUpTakeoversSuccessful_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getCatchUpTakeover().getSuccessful();
}

int ReplicationMetrics::getNumElectionTimeoutsSuccessful_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getElectionTimeout().getSuccessful();
}

int ReplicationMetrics::getNumFreezeTimeoutsSuccessful_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getFreezeTimeout().getSuccessful();
}

int ReplicationMetrics::getNumStepDownsCausedByHigherTerm_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getNumStepDownsCausedByHigherTerm();
}

int ReplicationMetrics::getNumCatchUps_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getNumCatchUps();
}

int ReplicationMetrics::getNumCatchUpsSucceeded_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getNumCatchUpsSucceeded();
}

int ReplicationMetrics::getNumCatchUpsAlreadyCaughtUp_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getNumCatchUpsAlreadyCaughtUp();
}

int ReplicationMetrics::getNumCatchUpsSkipped_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getNumCatchUpsSkipped();
}

int ReplicationMetrics::getNumCatchUpsTimedOut_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getNumCatchUpsTimedOut();
}

int ReplicationMetrics::getNumCatchUpsFailedWithError_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getNumCatchUpsFailedWithError();
}

int ReplicationMetrics::getNumCatchUpsFailedWithNewTerm_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getNumCatchUpsFailedWithNewTerm();
}

int ReplicationMetrics::getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd();
}

void ReplicationMetrics::setElectionCandidateMetrics(Date_t lastElectionDate) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _electionCandidateMetrics.setLastElectionDate(lastElectionDate);
    _nodeIsCandidateOrPrimary = true;
}

void ReplicationMetrics::setTargetCatchupOpTime(OpTime opTime) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _electionCandidateMetrics.setTargetCatchupOpTime(opTime);
}

void ReplicationMetrics::setNumCatchUpOps(int numCatchUpOps) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _electionCandidateMetrics.setNumCatchUpOps(numCatchUpOps);
}

void ReplicationMetrics::setNewTermStartDate(Date_t newTermStartDate) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _electionCandidateMetrics.setNewTermStartDate(newTermStartDate);
}

void ReplicationMetrics::setWMajorityWriteAvailabilityDate(Date_t wMajorityWriteAvailabilityDate) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _electionCandidateMetrics.setWMajorityWriteAvailabilityDate(wMajorityWriteAvailabilityDate);
}

boost::optional<OpTime> ReplicationMetrics::getTargetCatchupOpTime_forTesting() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionCandidateMetrics.getTargetCatchupOpTime();
}

BSONObj ReplicationMetrics::getElectionMetricsBSON() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _electionMetrics.toBSON();
}

BSONObj ReplicationMetrics::getElectionCandidateMetricsBSON() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_nodeIsCandidateOrPrimary) {
        return _electionCandidateMetrics.toBSON();
    }
    return BSONObj();
}

void ReplicationMetrics::clearElectionCandidateMetrics() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _electionCandidateMetrics.setTargetCatchupOpTime(boost::none);
    _electionCandidateMetrics.setNumCatchUpOps(boost::none);
    _electionCandidateMetrics.setNewTermStartDate(boost::none);
    _electionCandidateMetrics.setWMajorityWriteAvailabilityDate(boost::none);
    _nodeIsCandidateOrPrimary = false;
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
