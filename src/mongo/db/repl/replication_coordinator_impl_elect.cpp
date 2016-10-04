/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/repl/elect_cmd_runner.h"
#include "mongo/db/repl/freshness_checker.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

using LockGuard = stdx::lock_guard<stdx::mutex>;

namespace {
class LoseElectionGuard {
    MONGO_DISALLOW_COPYING(LoseElectionGuard);

public:
    LoseElectionGuard(TopologyCoordinator* topCoord,
                      ReplicationExecutor* executor,
                      std::unique_ptr<FreshnessChecker>* freshnessChecker,
                      std::unique_ptr<ElectCmdRunner>* electCmdRunner,
                      ReplicationExecutor::EventHandle* electionFinishedEvent)
        : _topCoord(topCoord),
          _executor(executor),
          _freshnessChecker(freshnessChecker),
          _electCmdRunner(electCmdRunner),
          _electionFinishedEvent(electionFinishedEvent),
          _dismissed(false) {}

    ~LoseElectionGuard() {
        if (_dismissed) {
            return;
        }
        _topCoord->processLoseElection();
        _freshnessChecker->reset(NULL);
        _electCmdRunner->reset(NULL);
        if (_electionFinishedEvent->isValid()) {
            _executor->signalEvent(*_electionFinishedEvent);
        }
    }

    void dismiss() {
        _dismissed = true;
    }

private:
    TopologyCoordinator* const _topCoord;
    ReplicationExecutor* const _executor;
    std::unique_ptr<FreshnessChecker>* const _freshnessChecker;
    std::unique_ptr<ElectCmdRunner>* const _electCmdRunner;
    const ReplicationExecutor::EventHandle* _electionFinishedEvent;
    bool _dismissed;
};

}  // namespace

void ReplicationCoordinatorImpl::_startElectSelf() {
    invariant(!_freshnessChecker);
    invariant(!_electCmdRunner);

    stdx::unique_lock<stdx::mutex> lk(_mutex);
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
            fassertFailed(18913);
    }

    log() << "Standing for election";
    const StatusWith<ReplicationExecutor::EventHandle> finishEvh = _replExecutor.makeEvent();
    if (finishEvh.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(18680, finishEvh.getStatus());
    _electionFinishedEvent = finishEvh.getValue();
    LoseElectionGuard lossGuard(_topCoord.get(),
                                &_replExecutor,
                                &_freshnessChecker,
                                &_electCmdRunner,
                                &_electionFinishedEvent);


    invariant(_rsConfig.getMemberAt(_selfIndex).isElectable());
    OpTime lastOpTimeApplied(_getMyLastAppliedOpTime_inlock());

    if (lastOpTimeApplied.isNull()) {
        log() << "not trying to elect self, "
                 "do not yet have a complete set of data from any point in time"
                 " -- lastAppliedOpTime is null";
        return;
    }

    _freshnessChecker.reset(new FreshnessChecker);

    // This is necessary because the freshnessChecker may call directly into winning an
    // election, if there are no other MaybeUp nodes.  Winning an election attempts to lock
    // _mutex again.
    lk.unlock();

    StatusWith<ReplicationExecutor::EventHandle> nextPhaseEvh =
        _freshnessChecker->start(&_replExecutor,
                                 lastOpTimeApplied.getTimestamp(),
                                 _rsConfig,
                                 _selfIndex,
                                 _topCoord->getMaybeUpHostAndPorts());
    if (nextPhaseEvh.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(18681, nextPhaseEvh.getStatus());
    _replExecutor.onEvent(nextPhaseEvh.getValue(),
                          stdx::bind(&ReplicationCoordinatorImpl::_onFreshnessCheckComplete, this));
    lossGuard.dismiss();
}

void ReplicationCoordinatorImpl::_onFreshnessCheckComplete() {
    invariant(_freshnessChecker);
    invariant(!_electCmdRunner);
    LoseElectionGuard lossGuard(_topCoord.get(),
                                &_replExecutor,
                                &_freshnessChecker,
                                &_electCmdRunner,
                                &_electionFinishedEvent);
    LockGuard lk(_topoMutex);

    if (_freshnessChecker->isCanceled()) {
        LOG(2) << "Election canceled during freshness check phase";
        return;
    }

    const Date_t now(_replExecutor.now());
    const FreshnessChecker::ElectionAbortReason abortReason =
        _freshnessChecker->shouldAbortElection();

    // need to not sleep after last time sleeping,
    switch (abortReason) {
        case FreshnessChecker::None:
            break;
        case FreshnessChecker::FreshnessTie:
            if ((_selfIndex != 0) && !_sleptLastElection) {
                const auto ms = Milliseconds(_replExecutor.nextRandomInt64(1000) + 50);
                const Date_t nextCandidateTime = now + ms;
                log() << "possible election tie; sleeping " << ms << " until "
                      << dateToISOStringLocal(nextCandidateTime);
                _topCoord->setElectionSleepUntil(nextCandidateTime);
                _scheduleWorkAt(nextCandidateTime,
                                stdx::bind(&ReplicationCoordinatorImpl::_recoverFromElectionTie,
                                           this,
                                           stdx::placeholders::_1));
                _sleptLastElection = true;
                return;
            }
            _sleptLastElection = false;
            break;
        case FreshnessChecker::FresherNodeFound:
            log() << "not electing self, we are not freshest";
            return;
        case FreshnessChecker::QuorumUnreachable:
            log() << "not electing self, we could not contact enough voting members";
            return;
        default:
            log() << "not electing self due to election abort message :"
                  << static_cast<int>(abortReason);
            return;
    }

    log() << "running for election";
    // Secure our vote for ourself first
    if (!_topCoord->voteForMyself(now)) {
        return;
    }

    _electCmdRunner.reset(new ElectCmdRunner);
    StatusWith<ReplicationExecutor::EventHandle> nextPhaseEvh = _electCmdRunner->start(
        &_replExecutor, _rsConfig, _selfIndex, _topCoord->getMaybeUpHostAndPorts());
    if (nextPhaseEvh.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(18685, nextPhaseEvh.getStatus());

    _replExecutor.onEvent(nextPhaseEvh.getValue(),
                          stdx::bind(&ReplicationCoordinatorImpl::_onElectCmdRunnerComplete, this));
    lossGuard.dismiss();
}

void ReplicationCoordinatorImpl::_onElectCmdRunnerComplete() {
    LoseElectionGuard lossGuard(_topCoord.get(),
                                &_replExecutor,
                                &_freshnessChecker,
                                &_electCmdRunner,
                                &_electionFinishedEvent);
    LockGuard lk(_topoMutex);

    invariant(_freshnessChecker);
    invariant(_electCmdRunner);
    if (_electCmdRunner->isCanceled()) {
        LOG(2) << "Election canceled during elect self phase";
        return;
    }

    const int receivedVotes = _electCmdRunner->getReceivedVotes();

    if (receivedVotes < _rsConfig.getMajorityVoteCount()) {
        log() << "couldn't elect self, only received " << receivedVotes
              << " votes, but needed at least " << _rsConfig.getMajorityVoteCount();
        // Suppress ourselves from standing for election again, giving other nodes a chance
        // to win their elections.
        const auto ms = Milliseconds(_replExecutor.nextRandomInt64(1000) + 50);
        const Date_t now(_replExecutor.now());
        const Date_t nextCandidateTime = now + ms;
        log() << "waiting until " << nextCandidateTime << " before standing for election again";
        _topCoord->setElectionSleepUntil(nextCandidateTime);
        _scheduleWorkAt(nextCandidateTime,
                        stdx::bind(&ReplicationCoordinatorImpl::_recoverFromElectionTie,
                                   this,
                                   stdx::placeholders::_1));
        return;
    }

    if (_rsConfig.getConfigVersion() != _freshnessChecker->getOriginalConfigVersion()) {
        log() << "config version changed during our election, ignoring result";
        return;
    }

    log() << "election succeeded, assuming primary role";

    lossGuard.dismiss();
    _freshnessChecker.reset(NULL);
    _electCmdRunner.reset(NULL);
    _performPostMemberStateUpdateAction(kActionWinElection);
    _replExecutor.signalEvent(_electionFinishedEvent);
}

void ReplicationCoordinatorImpl::_recoverFromElectionTie(
    const ReplicationExecutor::CallbackArgs& cbData) {
    LockGuard topoLock(_topoMutex);

    auto now = _replExecutor.now();
    auto lastOpApplied = getMyLastAppliedOpTime();
    const auto status = _topCoord->checkShouldStandForElection(now, lastOpApplied);
    if (!status.isOK()) {
        LOG(2) << "ReplicationCoordinatorImpl::_recoverFromElectionTie -- " << status.reason();
    } else {
        fassertStatusOK(28817, _topCoord->becomeCandidateIfElectable(now, lastOpApplied));
        _startElectSelf();
    }
}

}  // namespace repl
}  // namespace mongo
