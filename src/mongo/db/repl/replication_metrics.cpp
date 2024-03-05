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

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <cstdint>
#include <mutex>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/repl/election_reason_counter.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/decorable.h"

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

void ReplicationMetrics::incrementNumElectionsCalledForReason(StartElectionReasonEnum reason) {
    stdx::lock_guard<Latch> lk(_mutex);
    switch (reason) {
        case StartElectionReasonEnum::kStepUpRequest:
        case StartElectionReasonEnum::kStepUpRequestSkipDryRun: {
            ElectionReasonCounter& stepUpCmd = _electionMetrics.getStepUpCmd();
            stepUpCmd.incrementCalled();
            break;
        }
        case StartElectionReasonEnum::kPriorityTakeover: {
            ElectionReasonCounter& priorityTakeover = _electionMetrics.getPriorityTakeover();
            priorityTakeover.incrementCalled();
            break;
        }
        case StartElectionReasonEnum::kCatchupTakeover: {
            ElectionReasonCounter& catchUpTakeover = _electionMetrics.getCatchUpTakeover();
            catchUpTakeover.incrementCalled();
            break;
        }
        case StartElectionReasonEnum::kElectionTimeout: {
            ElectionReasonCounter& electionTimeout = _electionMetrics.getElectionTimeout();
            electionTimeout.incrementCalled();
            break;
        }
        case StartElectionReasonEnum::kSingleNodePromptElection: {
            ElectionReasonCounter& freezeTimeout = _electionMetrics.getFreezeTimeout();
            freezeTimeout.incrementCalled();
            break;
        }
    }
}

void ReplicationMetrics::incrementNumElectionsSuccessfulForReason(StartElectionReasonEnum reason) {
    stdx::lock_guard<Latch> lk(_mutex);
    switch (reason) {
        case StartElectionReasonEnum::kStepUpRequest:
        case StartElectionReasonEnum::kStepUpRequestSkipDryRun: {
            ElectionReasonCounter& stepUpCmd = _electionMetrics.getStepUpCmd();
            stepUpCmd.incrementSuccessful();
            break;
        }
        case StartElectionReasonEnum::kPriorityTakeover: {
            ElectionReasonCounter& priorityTakeover = _electionMetrics.getPriorityTakeover();
            priorityTakeover.incrementSuccessful();
            break;
        }
        case StartElectionReasonEnum::kCatchupTakeover: {
            ElectionReasonCounter& catchUpTakeover = _electionMetrics.getCatchUpTakeover();
            catchUpTakeover.incrementSuccessful();
            break;
        }
        case StartElectionReasonEnum::kElectionTimeout: {
            ElectionReasonCounter& electionTimeout = _electionMetrics.getElectionTimeout();
            electionTimeout.incrementSuccessful();
            break;
        }
        case StartElectionReasonEnum::kSingleNodePromptElection: {
            ElectionReasonCounter& freezeTimeout = _electionMetrics.getFreezeTimeout();
            freezeTimeout.incrementSuccessful();
            break;
        }
    }
}

void ReplicationMetrics::incrementNumStepDownsCausedByHigherTerm() {
    stdx::lock_guard<Latch> lk(_mutex);
    _electionMetrics.setNumStepDownsCausedByHigherTerm(
        _electionMetrics.getNumStepDownsCausedByHigherTerm() + 1);
}

void ReplicationMetrics::incrementNumCatchUps() {
    stdx::lock_guard<Latch> lk(_mutex);
    _electionMetrics.setNumCatchUps(_electionMetrics.getNumCatchUps() + 1);
    _updateAverageCatchUpOps(lk);
}

void ReplicationMetrics::incrementNumCatchUpsConcludedForReason(
    ReplicationCoordinator::PrimaryCatchUpConclusionReason reason) {
    stdx::lock_guard<Latch> lk(_mutex);
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

long ReplicationMetrics::getNumStepUpCmdsCalled_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getStepUpCmd().getCalled();
}

long ReplicationMetrics::getNumPriorityTakeoversCalled_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getPriorityTakeover().getCalled();
}

long ReplicationMetrics::getNumCatchUpTakeoversCalled_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getCatchUpTakeover().getCalled();
}

long ReplicationMetrics::getNumElectionTimeoutsCalled_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getElectionTimeout().getCalled();
}

long ReplicationMetrics::getNumFreezeTimeoutsCalled_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getFreezeTimeout().getCalled();
}

long ReplicationMetrics::getNumStepUpCmdsSuccessful_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getStepUpCmd().getSuccessful();
}

long ReplicationMetrics::getNumPriorityTakeoversSuccessful_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getPriorityTakeover().getSuccessful();
}

long ReplicationMetrics::getNumCatchUpTakeoversSuccessful_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getCatchUpTakeover().getSuccessful();
}

long ReplicationMetrics::getNumElectionTimeoutsSuccessful_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getElectionTimeout().getSuccessful();
}

long ReplicationMetrics::getNumFreezeTimeoutsSuccessful_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getFreezeTimeout().getSuccessful();
}

long ReplicationMetrics::getNumStepDownsCausedByHigherTerm_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getNumStepDownsCausedByHigherTerm();
}

long ReplicationMetrics::getNumCatchUps_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getNumCatchUps();
}

long ReplicationMetrics::getNumCatchUpsSucceeded_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getNumCatchUpsSucceeded();
}

long ReplicationMetrics::getNumCatchUpsAlreadyCaughtUp_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getNumCatchUpsAlreadyCaughtUp();
}

long ReplicationMetrics::getNumCatchUpsSkipped_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getNumCatchUpsSkipped();
}

long ReplicationMetrics::getNumCatchUpsTimedOut_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getNumCatchUpsTimedOut();
}

long ReplicationMetrics::getNumCatchUpsFailedWithError_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getNumCatchUpsFailedWithError();
}

long ReplicationMetrics::getNumCatchUpsFailedWithNewTerm_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getNumCatchUpsFailedWithNewTerm();
}

long ReplicationMetrics::getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd();
}

void ReplicationMetrics::setElectionCandidateMetrics(
    const StartElectionReasonEnum reason,
    const Date_t lastElectionDate,
    const long long electionTerm,
    const OpTime lastCommittedOpTime,
    const OpTime latestWrittenOpTime,
    const OpTime latestAppliedOpTime,
    const int numVotesNeeded,
    const double priorityAtElection,
    const Milliseconds electionTimeout,
    const boost::optional<int> priorPrimaryMemberId) {

    stdx::lock_guard<Latch> lk(_mutex);

    _nodeIsCandidateOrPrimary = true;
    _electionCandidateMetrics.setLastElectionReason(reason);
    _electionCandidateMetrics.setLastElectionDate(lastElectionDate);
    _electionCandidateMetrics.setElectionTerm(electionTerm);
    _electionCandidateMetrics.setLastCommittedOpTimeAtElection(lastCommittedOpTime);
    _electionCandidateMetrics.setLastSeenWrittenOpTimeAtElection(latestWrittenOpTime);
    _electionCandidateMetrics.setLastSeenOpTimeAtElection(latestAppliedOpTime);
    _electionCandidateMetrics.setNumVotesNeeded(numVotesNeeded);
    _electionCandidateMetrics.setPriorityAtElection(priorityAtElection);
    long long electionTimeoutMillis = durationCount<Milliseconds>(electionTimeout);
    _electionCandidateMetrics.setElectionTimeoutMillis(electionTimeoutMillis);
    _electionCandidateMetrics.setPriorPrimaryMemberId(priorPrimaryMemberId);
}

void ReplicationMetrics::setTargetCatchupOpTime(OpTime opTime) {
    stdx::lock_guard<Latch> lk(_mutex);
    _electionCandidateMetrics.setTargetCatchupOpTime(opTime);
}

void ReplicationMetrics::setNumCatchUpOps(long numCatchUpOps) {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(numCatchUpOps >= 0);
    _electionCandidateMetrics.setNumCatchUpOps(numCatchUpOps);
    _totalNumCatchUpOps += numCatchUpOps;
    _updateAverageCatchUpOps(lk);
}

void ReplicationMetrics::setCandidateNewTermStartDate(Date_t newTermStartDate) {
    stdx::lock_guard<Latch> lk(_mutex);
    _electionCandidateMetrics.setNewTermStartDate(newTermStartDate);
}

void ReplicationMetrics::setWMajorityWriteAvailabilityDate(Date_t wMajorityWriteAvailabilityDate) {
    stdx::lock_guard<Latch> lk(_mutex);
    _electionCandidateMetrics.setWMajorityWriteAvailabilityDate(wMajorityWriteAvailabilityDate);
}

boost::optional<OpTime> ReplicationMetrics::getTargetCatchupOpTime_forTesting() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionCandidateMetrics.getTargetCatchupOpTime();
}

BSONObj ReplicationMetrics::getElectionMetricsBSON() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _electionMetrics.toBSON();
}

BSONObj ReplicationMetrics::getElectionCandidateMetricsBSON() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_nodeIsCandidateOrPrimary) {
        return _electionCandidateMetrics.toBSON();
    }
    return BSONObj();
}

void ReplicationMetrics::clearElectionCandidateMetrics() {
    stdx::lock_guard<Latch> lk(_mutex);
    _electionCandidateMetrics.setTargetCatchupOpTime(boost::none);
    _electionCandidateMetrics.setNumCatchUpOps(boost::none);
    _electionCandidateMetrics.setNewTermStartDate(boost::none);
    _electionCandidateMetrics.setWMajorityWriteAvailabilityDate(boost::none);
    _nodeIsCandidateOrPrimary = false;
}

void ReplicationMetrics::setElectionParticipantMetrics(const bool votedForCandidate,
                                                       const long long electionTerm,
                                                       const Date_t lastVoteDate,
                                                       const int electionCandidateMemberId,
                                                       const std::string voteReason,
                                                       const OpTime lastWrittenOpTime,
                                                       const OpTime maxWrittenOpTimeInSet,
                                                       const OpTime lastAppliedOpTime,
                                                       const OpTime maxAppliedOpTimeInSet,
                                                       const double priorityAtElection) {
    stdx::lock_guard<Latch> lk(_mutex);

    _nodeHasVotedInElection = true;
    _electionParticipantMetrics.setVotedForCandidate(votedForCandidate);
    _electionParticipantMetrics.setElectionTerm(electionTerm);
    _electionParticipantMetrics.setLastVoteDate(lastVoteDate);
    _electionParticipantMetrics.setElectionCandidateMemberId(electionCandidateMemberId);
    _electionParticipantMetrics.setVoteReason(voteReason);
    _electionParticipantMetrics.setLastAppliedOpTimeAtElection(lastAppliedOpTime);
    _electionParticipantMetrics.setMaxAppliedOpTimeInSet(maxAppliedOpTimeInSet);
    _electionParticipantMetrics.setLastWrittenOpTimeAtElection(lastWrittenOpTime);
    _electionParticipantMetrics.setMaxWrittenOpTimeInSet(maxWrittenOpTimeInSet);
    _electionParticipantMetrics.setPriorityAtElection(priorityAtElection);
}

BSONObj ReplicationMetrics::getElectionParticipantMetricsBSON() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_nodeHasVotedInElection) {
        return _electionParticipantMetrics.toBSON();
    }
    return BSONObj();
}

void ReplicationMetrics::setParticipantNewTermDates(Date_t newTermStartDate,
                                                    Date_t newTermAppliedDate) {
    stdx::lock_guard<Latch> lk(_mutex);
    _electionParticipantMetrics.setNewTermStartDate(newTermStartDate);
    _electionParticipantMetrics.setNewTermAppliedDate(newTermAppliedDate);
}

void ReplicationMetrics::clearParticipantNewTermDates() {
    stdx::lock_guard<Latch> lk(_mutex);
    _electionParticipantMetrics.setNewTermStartDate(boost::none);
    _electionParticipantMetrics.setNewTermAppliedDate(boost::none);
}

void ReplicationMetrics::_updateAverageCatchUpOps(WithLock lk) {
    long numCatchUps = _electionMetrics.getNumCatchUps();
    if (numCatchUps > 0) {
        _electionMetrics.setAverageCatchUpOps(_totalNumCatchUpOps / numCatchUps);
    }
}

class ReplicationMetrics::ElectionMetricsSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        ReplicationMetrics& replicationMetrics = ReplicationMetrics::get(opCtx);

        return replicationMetrics.getElectionMetricsBSON();
    }
};
auto electionMetricsSSS =
    *ServerStatusSectionBuilder<ReplicationMetrics::ElectionMetricsSSS>("electionMetrics");

}  // namespace repl
}  // namespace mongo
