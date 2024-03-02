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


#include <memory>
#include <mutex>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/member_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_metrics.h"
#include "mongo/db/repl/replication_metrics_gen.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/vote_requester.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplicationElection


namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(hangInWritingLastVoteForDryRun);
MONGO_FAIL_POINT_DEFINE(electionHangsBeforeUpdateMemberState);
MONGO_FAIL_POINT_DEFINE(hangBeforeOnVoteRequestCompleteCallback);

class ReplicationCoordinatorImpl::ElectionState::LoseElectionGuardV1 {
    LoseElectionGuardV1(const LoseElectionGuardV1&) = delete;
    LoseElectionGuardV1& operator=(const LoseElectionGuardV1&) = delete;

public:
    LoseElectionGuardV1(ReplicationCoordinatorImpl* replCoord) : _replCoord(replCoord) {}

    virtual ~LoseElectionGuardV1() {
        if (_dismissed) {
            return;
        }
        LOGV2(21434, "Lost election", "isDryRun"_attr = _isDryRun);
        _replCoord->_topCoord->processLoseElection();
        const auto electionState = _replCoord->_electionState.get();
        if (_isDryRun && electionState->_electionDryRunFinishedEvent.isValid()) {
            _replCoord->_replExecutor->signalEvent(electionState->_electionDryRunFinishedEvent);
        }
        if (electionState->_electionFinishedEvent.isValid()) {
            _replCoord->_replExecutor->signalEvent(electionState->_electionFinishedEvent);
        }
        _replCoord->_electionState = nullptr;
        // Clear the node's election candidate metrics if it loses either the dry-run or actual
        // election, since it will not become primary.
        ReplicationMetrics::get(getGlobalServiceContext()).clearElectionCandidateMetrics();
    }

    void dismiss() {
        _dismissed = true;
    }

protected:
    ReplicationCoordinatorImpl* const _replCoord;
    bool _isDryRun = false;
    bool _dismissed = false;
};

class ReplicationCoordinatorImpl::ElectionState::LoseElectionDryRunGuardV1
    : public LoseElectionGuardV1 {
    LoseElectionDryRunGuardV1(const LoseElectionDryRunGuardV1&) = delete;
    LoseElectionDryRunGuardV1& operator=(const LoseElectionDryRunGuardV1&) = delete;

public:
    LoseElectionDryRunGuardV1(ReplicationCoordinatorImpl* replCoord)
        : LoseElectionGuardV1(replCoord) {
        _isDryRun = true;
    }
};

void ReplicationCoordinatorImpl::cancelElection_forTest() {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_electionState);
    _electionState->cancel(lk);
}

StatusWith<executor::TaskExecutor::EventHandle>
ReplicationCoordinatorImpl::ElectionState::_startVoteRequester(WithLock lk,
                                                               long long term,
                                                               bool dryRun,
                                                               OpTime lastWrittenOpTime,
                                                               OpTime lastAppliedOpTime,
                                                               int primaryIndex) {
    _voteRequester.reset(new VoteRequester);
    return _voteRequester->start(_replExecutor,
                                 _repl->_rsConfig,
                                 _repl->_selfIndex,
                                 term,
                                 dryRun,
                                 lastWrittenOpTime,
                                 lastAppliedOpTime,
                                 primaryIndex);
}

VoteRequester::Result ReplicationCoordinatorImpl::ElectionState::_getElectionResult(
    WithLock lk) const {
    if (_isCanceled) {
        return VoteRequester::Result::kCancelled;
    }
    return _voteRequester->getResult();
}

executor::TaskExecutor::EventHandle
ReplicationCoordinatorImpl::ElectionState::getElectionFinishedEvent(WithLock) {
    return _electionFinishedEvent;
}

executor::TaskExecutor::EventHandle
ReplicationCoordinatorImpl::ElectionState::getElectionDryRunFinishedEvent(WithLock) {
    return _electionDryRunFinishedEvent;
}

void ReplicationCoordinatorImpl::ElectionState::cancel(WithLock) {
    _isCanceled = true;
    // This check is necessary because _voteRequester is only initialized in _startVoteRequester.
    // Since we don't hold mutex during the entire election process, it is possible to get here
    // before _startVoteRequester is ever called.
    if (_voteRequester) {
        _voteRequester->cancel();
    }
}

void ReplicationCoordinatorImpl::ElectionState::start(WithLock lk, StartElectionReasonEnum reason) {
    LoseElectionDryRunGuardV1 lossGuard(_repl);
    switch (_repl->_rsConfigState) {
        case kConfigSteady:
            break;
        case kConfigInitiating:
        case kConfigReconfiguring:
        case kConfigHBReconfiguring:
            LOGV2_DEBUG(21435, 2, "Not standing for election; processing a configuration change");
            return;
        default:
            LOGV2_FATAL(28641,
                        "Entered replica set election code while in illegal config state",
                        "rsConfigState"_attr = int(_repl->_rsConfigState));
    }

    auto finishedEvent = _repl->_makeEvent();
    if (!finishedEvent) {
        return;
    }
    _electionFinishedEvent = finishedEvent;
    auto dryRunFinishedEvent = _repl->_makeEvent();
    if (!dryRunFinishedEvent) {
        return;
    }
    _electionDryRunFinishedEvent = dryRunFinishedEvent;

    invariant(_repl->_rsConfig.getMemberAt(_repl->_selfIndex).isElectable());
    const auto lastWrittenOpTime = _repl->_getMyLastWrittenOpTime_inlock();
    const auto lastAppliedOpTime = _repl->_getMyLastAppliedOpTime_inlock();

    if (lastWrittenOpTime == OpTime() || lastAppliedOpTime == OpTime()) {
        LOGV2(21436,
              "Not trying to elect self, "
              "do not yet have a complete set of data from any point in time");
        return;
    }

    long long term = _topCoord->getTerm();
    int primaryIndex = -1;

    if (reason == StartElectionReasonEnum::kStepUpRequestSkipDryRun) {
        long long newTerm = term + 1;
        LOGV2(21437, "Skipping dry run and running for election", "newTerm"_attr = newTerm);
        _startRealElection(lk, newTerm, reason);
        lossGuard.dismiss();
        return;
    }

    LOGV2(21438,
          "Conducting a dry run election to see if we could be elected",
          "currentTerm"_attr = term);

    // Only set primaryIndex if the primary's vote is required during the dry run.
    if (reason == StartElectionReasonEnum::kCatchupTakeover) {
        primaryIndex = _topCoord->getCurrentPrimaryIndex();
    }
    StatusWith<executor::TaskExecutor::EventHandle> nextPhaseEvh =
        _startVoteRequester(lk,
                            term,
                            true,  // dry run
                            lastWrittenOpTime,
                            lastAppliedOpTime,
                            primaryIndex);
    if (nextPhaseEvh.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(28685, nextPhaseEvh.getStatus());
    _replExecutor
        ->onEvent(nextPhaseEvh.getValue(),
                  [=, this](const executor::TaskExecutor::CallbackArgs&) {
                      _processDryRunResult(term, reason);
                  })
        .status_with_transitional_ignore();
    lossGuard.dismiss();
}

void ReplicationCoordinatorImpl::ElectionState::_processDryRunResult(
    long long originalTerm, StartElectionReasonEnum reason) {
    stdx::lock_guard<Latch> lk(_repl->_mutex);
    LoseElectionDryRunGuardV1 lossGuard(_repl);
    invariant(_voteRequester != nullptr);

    if (_topCoord->getTerm() != originalTerm) {
        LOGV2(21439,
              "Not running for primary, we have been superseded already during dry run",
              "originalTerm"_attr = originalTerm,
              "currentTerm"_attr = _topCoord->getTerm());
        return;
    }

    const auto endResult = _getElectionResult(lk);
    switch (endResult) {
        case VoteRequester::Result::kInsufficientVotes:
            LOGV2(21440, "Not running for primary, we received insufficient votes");
            return;
        case VoteRequester::Result::kStaleTerm:
            LOGV2(21441, "Not running for primary, we have been superseded already");
            return;
        case VoteRequester::Result::kPrimaryRespondedNo:
            LOGV2(21442,
                  "Not running for primary, the current primary responded no in the dry run");
            return;
        case VoteRequester::Result::kCancelled:
            LOGV2(214400, "Not running for primary, election has been cancelled");
            return;
        case VoteRequester::Result::kSuccessfullyElected:
            break;
    }

    long long newTerm = originalTerm + 1;
    LOGV2(21444, "Dry election run succeeded, running for election", "newTerm"_attr = newTerm);

    _startRealElection(lk, newTerm, reason);
    lossGuard.dismiss();
}

void ReplicationCoordinatorImpl::ElectionState::_startRealElection(WithLock lk,
                                                                   long long newTerm,
                                                                   StartElectionReasonEnum reason) {
    const auto& rsConfig = _repl->_rsConfig;
    const auto selfIndex = _repl->_selfIndex;

    const Date_t now = _replExecutor->now();
    const OpTime lastCommittedOpTime = _topCoord->getLastCommittedOpTime();
    const OpTime latestWrittenOpTime = _topCoord->latestKnownWrittenOpTime();
    const OpTime latestAppliedOpTime = _topCoord->latestKnownAppliedOpTime();
    const int numVotesNeeded = rsConfig.getMajorityVoteCount();
    const double priorityAtElection = rsConfig.getMemberAt(selfIndex).getPriority();
    const Milliseconds electionTimeoutMillis = rsConfig.getElectionTimeoutPeriod();
    const int priorPrimaryIndex = _topCoord->getCurrentPrimaryIndex();
    const boost::optional<int> priorPrimaryMemberId = (priorPrimaryIndex == -1)
        ? boost::none
        : boost::make_optional(rsConfig.getMemberAt(priorPrimaryIndex).getId().getData());

    ReplicationMetrics::get(_repl->getServiceContext())
        .setElectionCandidateMetrics(reason,
                                     now,
                                     newTerm,
                                     lastCommittedOpTime,
                                     latestWrittenOpTime,
                                     latestAppliedOpTime,
                                     numVotesNeeded,
                                     priorityAtElection,
                                     electionTimeoutMillis,
                                     priorPrimaryMemberId);
    ReplicationMetrics::get(_repl->getServiceContext())
        .incrementNumElectionsCalledForReason(reason);

    LoseElectionDryRunGuardV1 lossGuard(_repl);

    TopologyCoordinator::UpdateTermResult updateTermResult;
    _repl->_updateTerm_inlock(newTerm, &updateTermResult);
    // This is the only valid result from this term update. If we are here, then we are not a
    // primary, so a stepdown is not possible. We have also not yet learned of a higher term from
    // someone else: seeing an update in the topology coordinator mid-election requires releasing
    // the mutex. This only happens during a dry run, which makes sure to check for term updates.
    invariant(updateTermResult == TopologyCoordinator::UpdateTermResult::kUpdatedTerm);
    // Secure our vote for ourself first
    _topCoord->voteForMyselfV1();

    // Store the vote in persistent storage.
    LastVote lastVote{newTerm, selfIndex};

    auto cbStatus = _replExecutor->scheduleWork(
        [this, lastVote, reason](const executor::TaskExecutor::CallbackArgs& cbData) {
            _writeLastVoteForMyElection(lastVote, cbData, reason);
        });
    if (cbStatus.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(34421, cbStatus.getStatus());
    lossGuard.dismiss();
}

void ReplicationCoordinatorImpl::ElectionState::_writeLastVoteForMyElection(
    LastVote lastVote,
    const executor::TaskExecutor::CallbackArgs& cbData,
    StartElectionReasonEnum reason) {
    // storeLocalLastVoteDocument can call back in to the replication coordinator,
    // so _mutex must be unlocked here.  However, we cannot return until we
    // lock it because we want to lose the election on cancel or error and
    // doing so requires _mutex.
    auto status = [&] {
        if (!cbData.status.isOK()) {
            return cbData.status;
        }
        auto opCtx = cc().makeOperationContext();
        // Any operation that occurs as part of an election process is critical to the operation of
        // the cluster. We mark the operation as having Immediate priority to skip ticket
        // acquisition and flow control.
        ScopedAdmissionPriority priority(opCtx.get(), AdmissionContext::Priority::kExempt);

        LOGV2(6015300,
              "Storing last vote document in local storage for my election",
              "lastVote"_attr = lastVote);
        return _repl->_externalState->storeLocalLastVoteDocument(opCtx.get(), lastVote);
    }();

    if (MONGO_unlikely(hangInWritingLastVoteForDryRun.shouldFail())) {
        LOGV2(4825601, "Hang due to hangInWritingLastVoteForDryRun failpoint");
        hangInWritingLastVoteForDryRun.pauseWhileSet();
    }
    stdx::lock_guard<Latch> lk(_repl->_mutex);
    LoseElectionDryRunGuardV1 lossGuard(_repl);
    if (status == ErrorCodes::CallbackCanceled) {
        LOGV2(6015301, "Callback for storing last vote got cancelled");
        return;
    }

    if (!status.isOK()) {
        LOGV2(21445,
              "Failed to store LastVote document when voting for myself",
              "error"_attr = status);
        return;
    }

    if (_topCoord->getTerm() != lastVote.getTerm()) {
        LOGV2(21446,
              "Not running for primary, we have been superseded already while writing our last "
              "vote",
              "electionTerm"_attr = lastVote.getTerm(),
              "currentTerm"_attr = _topCoord->getTerm());
        return;
    }

    _requestVotesForRealElection(lk, lastVote.getTerm(), reason);
    _replExecutor->signalEvent(_electionDryRunFinishedEvent);

    lossGuard.dismiss();
}

void ReplicationCoordinatorImpl::ElectionState::_requestVotesForRealElection(
    WithLock lk, long long newTerm, StartElectionReasonEnum reason) {
    const auto lastWrittenOpTime = _repl->_getMyLastWrittenOpTime_inlock();
    const auto lastAppliedOpTime = _repl->_getMyLastAppliedOpTime_inlock();

    StatusWith<executor::TaskExecutor::EventHandle> nextPhaseEvh =
        _startVoteRequester(lk, newTerm, false, lastWrittenOpTime, lastAppliedOpTime, -1);
    if (nextPhaseEvh.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(28643, nextPhaseEvh.getStatus());
    _replExecutor
        ->onEvent(nextPhaseEvh.getValue(),
                  [=, this](const executor::TaskExecutor::CallbackArgs&) {
                      if (MONGO_unlikely(hangBeforeOnVoteRequestCompleteCallback.shouldFail())) {
                          LOGV2(7277400,
                                "Hang due to hangBeforeOnVoteRequestCompleteCallback failpoint");
                          hangBeforeOnVoteRequestCompleteCallback.pauseWhileSet();
                      }
                      _onVoteRequestComplete(newTerm, reason);
                  })
        .status_with_transitional_ignore();
}

void ReplicationCoordinatorImpl::ElectionState::_onVoteRequestComplete(
    long long newTerm, StartElectionReasonEnum reason) {
    stdx::lock_guard<Latch> lk(_repl->_mutex);
    LoseElectionGuardV1 lossGuard(_repl);
    invariant(_voteRequester != nullptr);

    if (_topCoord->getTerm() != newTerm) {
        LOGV2(21447,
              "Not becoming primary, we have been superseded already during election",
              "electionTerm"_attr = newTerm,
              "currentTerm"_attr = _topCoord->getTerm());
        return;
    }

    const VoteRequester::Result endResult = _getElectionResult(lk);
    invariant(endResult != VoteRequester::Result::kPrimaryRespondedNo);

    switch (endResult) {
        case VoteRequester::Result::kCancelled:
            LOGV2(214480, "Not becoming primary, election has been cancelled");
            return;
        case VoteRequester::Result::kInsufficientVotes:
            LOGV2(21448, "Not becoming primary, we received insufficient votes");
            return;
        case VoteRequester::Result::kStaleTerm:
            LOGV2(21449, "Not becoming primary, we have been superseded already");
            return;
        case VoteRequester::Result::kSuccessfullyElected:
            LOGV2(21450,
                  "Election succeeded, assuming primary role",
                  "term"_attr = _topCoord->getTerm());
            ReplicationMetrics::get(_repl->getServiceContext())
                .incrementNumElectionsSuccessfulForReason(reason);
            break;
        case VoteRequester::Result::kPrimaryRespondedNo:
            // This is impossible because we would only require the primary's
            // vote during a dry run.
            MONGO_UNREACHABLE;
    }

    // Mark all nodes that responded to our vote request as up to avoid immediately
    // relinquishing primary.
    Date_t now = _replExecutor->now();
    _topCoord->resetMemberTimeouts(now, _voteRequester->getResponders());

    auto electionFinishedEvent = _electionFinishedEvent;
    electionHangsBeforeUpdateMemberState.execute([&](const BSONObj& customWait) {
        auto waitForMillis = Milliseconds(customWait["waitForMillis"].numberInt());
        LOGV2(21451,
              "Election succeeded - electionHangsBeforeUpdateMemberState fail point "
              "enabled, sleeping",
              "waitFor"_attr = waitForMillis);
        sleepFor(waitForMillis);
    });

    _repl->_postWonElectionUpdateMemberState(lk);
    _replExecutor->signalEvent(electionFinishedEvent);
    lossGuard.dismiss();

    _repl->_electionState = nullptr;
}

}  // namespace repl
}  // namespace mongo
