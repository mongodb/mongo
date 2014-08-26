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

#include "mongo/db/repl/repl_coordinator_impl.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/db/repl/elect_cmd_runner.h"
#include "mongo/db/repl/freshness_checker.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

    void ReplicationCoordinatorImpl::testElection() {
        // Make a new event for tracking this election attempt.
        StatusWith<ReplicationExecutor::EventHandle> finishEvh = _replExecutor.makeEvent();
        fassert(18680, finishEvh.getStatus());

        StatusWith<ReplicationExecutor::CallbackHandle> cbh = _replExecutor.scheduleWork(
            stdx::bind(&ReplicationCoordinatorImpl::_startElectSelf,
                       this,
                       stdx::placeholders::_1,
                       finishEvh.getValue()));
        fassert(18672, cbh.getStatus());
        _replExecutor.waitForEvent(finishEvh.getValue());
    }

    void ReplicationCoordinatorImpl::_startElectSelf(
        const ReplicationExecutor::CallbackData& cbData,
        const ReplicationExecutor::EventHandle& finishEvh) {
        // Signal finish event upon early exit.
        ScopeGuard finishEvhGuard(MakeGuard(&ReplicationExecutor::signalEvent, 
                                            cbData.executor, 
                                            finishEvh));

        if (cbData.status == ErrorCodes::CallbackCanceled)
            return;

        boost::unique_lock<boost::mutex> lk(_mutex);

        invariant(_rsConfig.getMemberAt(_thisMembersConfigIndex).isElectable());
        OpTime lastOpTimeApplied(_getLastOpApplied_inlock());

        if (lastOpTimeApplied == 0) {
            log() << "replSet info not trying to elect self, "
                "do not yet have a complete set of data from any point in time";
            return;
        }

        if (_freshnessChecker) {
            // If an attempt to elect self is currently in progress, don't interrupt it.
            return;
            // Note that the old code, in addition to prohibiting multiple in-flight election 
            // attempts, used to omit processing *any* incoming knowledge about
            // primaries in the cluster while an election was occurring.  This seemed like
            // overkill, so it has been removed.
        }

        // Make an event for our internal use to help synchronize the next phase of election.
        StatusWith<ReplicationExecutor::EventHandle> nextPhaseEvh = cbData.executor->makeEvent();
        if (nextPhaseEvh.getStatus() == ErrorCodes::ShutdownInProgress) { 
            return;
        } 
        fassert(18681, nextPhaseEvh.getStatus());

        _freshnessChecker.reset(new FreshnessChecker);
        StatusWith<ReplicationExecutor::CallbackHandle> finishCheckCallback = 
            cbData.executor->onEvent(
                nextPhaseEvh.getValue(),
                stdx::bind(&ReplicationCoordinatorImpl::_onFreshnessCheckComplete,
                           this, 
                           stdx::placeholders::_1,
                           finishEvh));
        if (finishCheckCallback.getStatus() == ErrorCodes::ShutdownInProgress) {
            return;
        }
        fassert(18670, finishCheckCallback.getStatus());

        Status status = _freshnessChecker->start(cbData.executor, 
                                                 nextPhaseEvh.getValue(), 
                                                 lastOpTimeApplied, 
                                                 _rsConfig, 
                                                 _thisMembersConfigIndex,
                                                 _topCoord->getMaybeUpHostAndPorts());
        if (status == ErrorCodes::ShutdownInProgress) { 
            return;
        } 
        fassert(18688, status);

        finishEvhGuard.Dismiss();
    }


    void ReplicationCoordinatorImpl::_onFreshnessCheckComplete(
        const ReplicationExecutor::CallbackData& cbData,
        const ReplicationExecutor::EventHandle& finishEvh) {

        // Signal finish event upon early exit.
        ScopeGuard finishEvhGuard(MakeGuard(&ReplicationExecutor::signalEvent, 
                                            cbData.executor, 
                                            finishEvh));

        // Make sure to reset our state on all error exit paths
        ScopeGuard freshnessCheckerDeleter = 
            MakeObjGuard(_freshnessChecker, 
                         &boost::scoped_ptr<FreshnessChecker>::reset, 
                         static_cast<FreshnessChecker*>(NULL));

        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }

        Date_t now(cbData.executor->now());
        bool weAreFreshest;
        bool tied;
        _freshnessChecker->getResults(&weAreFreshest, &tied);

        // need to not sleep after last time sleeping,
        if (tied) {
            boost::unique_lock<boost::mutex> lk(_mutex);
            if ((_thisMembersConfigIndex != 0) && !_sleptLastElection) {
                long long ms = _random.nextInt64(1000) + 50;
                log() << "replSet possible election tie; sleeping a little " << ms << "ms";
                _topCoord->setStepDownTime(now + ms);
                _sleptLastElection = true;
                return;
            }
            _sleptLastElection = false;
        }

        if (!weAreFreshest) {
            log() << "not electing self, we are not freshest";
            return;
        }

        log() << "replSet info electSelf";

        // Secure our vote for ourself first
        if (!_topCoord->voteForMyself(now)) {
            return;
        }

        StatusWith<ReplicationExecutor::EventHandle> nextPhaseEvh = cbData.executor->makeEvent();
        if (nextPhaseEvh.getStatus() == ErrorCodes::ShutdownInProgress) { 
            return;
        } 
        fassert(18685, nextPhaseEvh.getStatus());


        _electCmdRunner.reset(new ElectCmdRunner);
        StatusWith<ReplicationExecutor::CallbackHandle> finishCheckCallback = 
            cbData.executor->onEvent(
                nextPhaseEvh.getValue(),
                stdx::bind(&ReplicationCoordinatorImpl::_onElectCmdRunnerComplete,
                           this,
                           stdx::placeholders::_1,
                           finishEvh));
        if (finishCheckCallback.getStatus() == ErrorCodes::ShutdownInProgress) {
            return;
        }
        fassert(18671, finishCheckCallback.getStatus());

        Status electionCompleteStatus = _electCmdRunner->start(cbData.executor, 
                                                               nextPhaseEvh.getValue(), 
                                                               _rsConfig, 
                                                               _thisMembersConfigIndex,
                                                               _topCoord->getMaybeUpHostAndPorts());
        if (electionCompleteStatus == ErrorCodes::ShutdownInProgress) { 
            return;
        } 
        fassert(18686, electionCompleteStatus);

        freshnessCheckerDeleter.Dismiss();
        finishEvhGuard.Dismiss();
    }

    void ReplicationCoordinatorImpl::_onElectCmdRunnerComplete(
        const ReplicationExecutor::CallbackData& cbData,
        const ReplicationExecutor::EventHandle& finishEvh) {

        // Signal finish event and cleanup, upon function exit in all cases.
        ON_BLOCK_EXIT(&ReplicationExecutor::signalEvent, cbData.executor, finishEvh);
        ON_BLOCK_EXIT_OBJ(_freshnessChecker, 
                          &boost::scoped_ptr<FreshnessChecker>::reset, 
                          static_cast<FreshnessChecker*>(NULL));
        ON_BLOCK_EXIT_OBJ(_electCmdRunner, 
                          &boost::scoped_ptr<ElectCmdRunner>::reset, 
                          static_cast<ElectCmdRunner*>(NULL));

        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }

        int receivedVotes = _electCmdRunner->getReceivedVotes();

        if (receivedVotes * 2 <= _rsConfig.getMajorityVoteCount()) {
            log() << "replSet couldn't elect self, only received " << receivedVotes << " votes";
            return;
        }
        
        if (_rsConfig.getConfigVersion() != _freshnessChecker->getOriginalConfigVersion()) {
            log() << "replSet config version changed during our election, ignoring result";
            return;
        }
        
        log() << "replSet election succeeded, assuming primary role";

        //
        // TODO: setElectionTime(getNextGlobalOptime()), ask Applier to pause, wait for
        //       applier's signal that it's done flushing ops (signalDrainComplete)
        // and then _changememberstate to PRIMARY.

    }

}
}
