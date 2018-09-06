/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/vote_requester.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

class ReplicationCoordinatorImpl::LoseElectionGuardV1 {
    MONGO_DISALLOW_COPYING(LoseElectionGuardV1);

public:
    LoseElectionGuardV1(ReplicationCoordinatorImpl* replCoord) : _replCoord(replCoord) {}

    virtual ~LoseElectionGuardV1() {
        if (_dismissed) {
            return;
        }
        _replCoord->_topCoord->processLoseElection();
        _replCoord->_voteRequester.reset(nullptr);
        if (_isDryRun && _replCoord->_electionDryRunFinishedEvent.isValid()) {
            _replCoord->_replExecutor->signalEvent(_replCoord->_electionDryRunFinishedEvent);
        }
        if (_replCoord->_electionFinishedEvent.isValid()) {
            _replCoord->_replExecutor->signalEvent(_replCoord->_electionFinishedEvent);
        }
    }

    void dismiss() {
        _dismissed = true;
    }

protected:
    ReplicationCoordinatorImpl* const _replCoord;
    bool _isDryRun = false;
    bool _dismissed = false;
};

class ReplicationCoordinatorImpl::LoseElectionDryRunGuardV1 : public LoseElectionGuardV1 {
    MONGO_DISALLOW_COPYING(LoseElectionDryRunGuardV1);

public:
    LoseElectionDryRunGuardV1(ReplicationCoordinatorImpl* replCoord)
        : LoseElectionGuardV1(replCoord) {
        _isDryRun = true;
    }
};


void ReplicationCoordinatorImpl::_startElectSelfV1(
    TopologyCoordinator::StartElectionReason reason) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _startElectSelfV1_inlock(reason);
}

void ReplicationCoordinatorImpl::_startElectSelfV1_inlock(
    TopologyCoordinator::StartElectionReason reason) {
    invariant(!_voteRequester);
    invariant(!_freshnessChecker);

    switch (_rsConfigState) {
        case kConfigSteady:
            break;
        case kConfigInitiating:
        case kConfigReconfiguring:
        case kConfigHBReconfiguring:
            LOG(2) << "Not standing for election; processing a configuration change";
            // Transition out of candidate role.
            _topCoord->processLoseElection();
            return;
        default:
            severe() << "Entered replica set election code while in illegal config state "
                     << int(_rsConfigState);
            fassertFailed(28641);
    }

    auto finishedEvent = _makeEvent();
    if (!finishedEvent) {
        return;
    }
    _electionFinishedEvent = finishedEvent;

    auto dryRunFinishedEvent = _makeEvent();
    if (!dryRunFinishedEvent) {
        return;
    }
    _electionDryRunFinishedEvent = dryRunFinishedEvent;

    LoseElectionDryRunGuardV1 lossGuard(this);


    invariant(_rsConfig.getMemberAt(_selfIndex).isElectable());
    const auto lastOpTime = _getMyLastAppliedOpTime_inlock();

    if (lastOpTime == OpTime()) {
        log() << "not trying to elect self, "
                 "do not yet have a complete set of data from any point in time";
        return;
    }

    long long term = _topCoord->getTerm();
    int primaryIndex = -1;

    if (reason == TopologyCoordinator::StartElectionReason::kStepUpRequestSkipDryRun) {
        long long newTerm = term + 1;
        log() << "skipping dry run and running for election in term " << newTerm;
        _startRealElection_inlock(newTerm);
        lossGuard.dismiss();
        return;
    }

    log() << "conducting a dry run election to see if we could be elected. current term: " << term;
    _voteRequester.reset(new VoteRequester);

    // Only set primaryIndex if the primary's vote is required during the dry run.
    if (reason == TopologyCoordinator::StartElectionReason::kCatchupTakeover) {
        primaryIndex = _topCoord->getCurrentPrimaryIndex();
    }
    StatusWith<executor::TaskExecutor::EventHandle> nextPhaseEvh =
        _voteRequester->start(_replExecutor.get(),
                              _rsConfig,
                              _selfIndex,
                              term,
                              true,  // dry run
                              lastOpTime,
                              primaryIndex);
    if (nextPhaseEvh.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(28685, nextPhaseEvh.getStatus());
    _replExecutor
        ->onEvent(nextPhaseEvh.getValue(),
                  stdx::bind(&ReplicationCoordinatorImpl::_processDryRunResult, this, term))
        .status_with_transitional_ignore();
    lossGuard.dismiss();
}

void ReplicationCoordinatorImpl::_processDryRunResult(long long originalTerm) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    LoseElectionDryRunGuardV1 lossGuard(this);

    invariant(_voteRequester);

    if (_topCoord->getTerm() != originalTerm) {
        log() << "not running for primary, we have been superseded already during dry run. "
              << "original term: " << originalTerm << ", current term: " << _topCoord->getTerm();
        return;
    }

    const VoteRequester::Result endResult = _voteRequester->getResult();

    if (endResult == VoteRequester::Result::kInsufficientVotes) {
        log() << "not running for primary, we received insufficient votes";
        return;
    } else if (endResult == VoteRequester::Result::kStaleTerm) {
        log() << "not running for primary, we have been superseded already";
        return;
    } else if (endResult == VoteRequester::Result::kPrimaryRespondedNo) {
        log() << "not running for primary, the current primary responded no in the dry run";
        return;
    } else if (endResult != VoteRequester::Result::kSuccessfullyElected) {
        log() << "not running for primary, we received an unexpected problem";
        return;
    }

    long long newTerm = originalTerm + 1;
    log() << "dry election run succeeded, running for election in term " << newTerm;

    _startRealElection_inlock(newTerm);
    lossGuard.dismiss();
}

void ReplicationCoordinatorImpl::_startRealElection_inlock(long long newTerm) {
    LoseElectionDryRunGuardV1 lossGuard(this);

    TopologyCoordinator::UpdateTermResult updateTermResult;
    _updateTerm_inlock(newTerm, &updateTermResult);
    // This is the only valid result from this term update. If we are here, then we are not a
    // primary, so a stepdown is not possible. We have also not yet learned of a higher term from
    // someone else: seeing an update in the topology coordinator mid-election requires releasing
    // the mutex. This only happens during a dry run, which makes sure to check for term updates.
    invariant(updateTermResult == TopologyCoordinator::UpdateTermResult::kUpdatedTerm);
    // Secure our vote for ourself first
    _topCoord->voteForMyselfV1();

    // Store the vote in persistent storage.
    LastVote lastVote{newTerm, _selfIndex};

    auto cbStatus = _replExecutor->scheduleWork(
        [this, lastVote](const executor::TaskExecutor::CallbackArgs& cbData) {
            _writeLastVoteForMyElection(lastVote, cbData);
        });
    if (cbStatus.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(34421, cbStatus.getStatus());
    lossGuard.dismiss();
}

void ReplicationCoordinatorImpl::_writeLastVoteForMyElection(
    LastVote lastVote, const executor::TaskExecutor::CallbackArgs& cbData) {
    // storeLocalLastVoteDocument can call back in to the replication coordinator,
    // so _mutex must be unlocked here.  However, we cannot return until we
    // lock it because we want to lose the election on cancel or error and
    // doing so requires _mutex.
    auto status = [&] {
        if (!cbData.status.isOK()) {
            return cbData.status;
        }
        auto opCtx = cc().makeOperationContext();
        return _externalState->storeLocalLastVoteDocument(opCtx.get(), lastVote);
    }();

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    LoseElectionDryRunGuardV1 lossGuard(this);
    if (status == ErrorCodes::CallbackCanceled) {
        return;
    }

    if (!status.isOK()) {
        log() << "failed to store LastVote document when voting for myself: " << status;
        return;
    }

    if (_topCoord->getTerm() != lastVote.getTerm()) {
        log() << "not running for primary, we have been superseded already while writing our last "
                 "vote. election term: "
              << lastVote.getTerm() << ", current term: " << _topCoord->getTerm();
        return;
    }
    _startVoteRequester_inlock(lastVote.getTerm());
    _replExecutor->signalEvent(_electionDryRunFinishedEvent);

    lossGuard.dismiss();
}

void ReplicationCoordinatorImpl::_startVoteRequester_inlock(long long newTerm) {
    const auto lastOpTime = _getMyLastAppliedOpTime_inlock();

    _voteRequester.reset(new VoteRequester);
    StatusWith<executor::TaskExecutor::EventHandle> nextPhaseEvh = _voteRequester->start(
        _replExecutor.get(), _rsConfig, _selfIndex, newTerm, false, lastOpTime, -1);
    if (nextPhaseEvh.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(28643, nextPhaseEvh.getStatus());
    _replExecutor
        ->onEvent(nextPhaseEvh.getValue(),
                  stdx::bind(&ReplicationCoordinatorImpl::_onVoteRequestComplete, this, newTerm))
        .status_with_transitional_ignore();
}

void ReplicationCoordinatorImpl::_onVoteRequestComplete(long long newTerm) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    LoseElectionGuardV1 lossGuard(this);

    invariant(_voteRequester);

    if (_topCoord->getTerm() != newTerm) {
        log() << "not becoming primary, we have been superseded already during election. "
              << "election term: " << newTerm << ", current term: " << _topCoord->getTerm();
        return;
    }

    const VoteRequester::Result endResult = _voteRequester->getResult();
    invariant(endResult != VoteRequester::Result::kPrimaryRespondedNo);

    switch (endResult) {
        case VoteRequester::Result::kInsufficientVotes:
            log() << "not becoming primary, we received insufficient votes";
            return;
        case VoteRequester::Result::kStaleTerm:
            log() << "not becoming primary, we have been superseded already";
            return;
        case VoteRequester::Result::kSuccessfullyElected:
            log() << "election succeeded, assuming primary role in term " << _topCoord->getTerm();
            break;
        case VoteRequester::Result::kPrimaryRespondedNo:
            // This is impossible because we would only require the primary's
            // vote during a dry run.
            invariant(false);
    }

    // Mark all nodes that responded to our vote request as up to avoid immediately
    // relinquishing primary.
    Date_t now = _replExecutor->now();
    _topCoord->resetMemberTimeouts(now, _voteRequester->getResponders());

    _voteRequester.reset();
    auto electionFinishedEvent = _electionFinishedEvent;

    lk.unlock();
    _performPostMemberStateUpdateAction(kActionWinElection);

    _replExecutor->signalEvent(electionFinishedEvent);
    lossGuard.dismiss();
}

}  // namespace repl
}  // namespace mongo
