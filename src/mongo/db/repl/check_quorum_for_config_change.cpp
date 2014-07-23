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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/check_quorum_for_config_change.h"

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {
namespace {
    /**
     * Quorum checking state machine.
     *
     * Usage: Construct a QuorumChecker, pass in a pointer to the configuration for which you're
     * checking quorum, and the integer index of the member config representing the "executing"
     * node.  Call "run", pass in an executor, and the returned status is the result of the quorum
     * check.
     *
     * Theory of operation:
     *
     * The QuorumChecker executes three kinds of callback in the executor's run loop.
     *
     * - _startQuorumCheck schedules heartbeats to all nodes except the local one, and initializes
     *    the checker's response tabulation with information about the local node.
     *
     * - _onQuorumCheckHeartbeatResponse updates the response tabulation based on a response
     *   or timeout from a remote node.
     *
     * - _onQuorumCheckComplete uses information in the tabulation to compute the final status
     *   of the quorum check, sometime after _haveReceivedSufficientReplies becomes true.
     *
     * The thread executing QuorumChecker::run first performs some initial set up, required for it
     * to synchronize effectively with the thread running executor's event loop.  It creates the
     * event that is used to trigger _onQuorumCheckComplete, and schedules that callback and the
     * _startQuorumCheck callbacks.  It then waits for _onQuorumCheckComplete to return, at which
     * point it knows that _finalStatus is set to the proper response, and so can return.
     *
     * Some scheduled _onQuorumCheckHeartbeatResponse callbacks may not have executed before
     * QuorumChecker::run wakes up from waiting for the _onQuorumCheckComplete callback.  Before
     * returning, QuorumChecker::run marks all callbacks it scheduled for cancelation, ensuring that
     * even if they run they will not access member variables of QuorumChecker.  This makes it safe
     * to let QuorumChecker go out of scope as soon as QuorumChecker::run returns.
     */
    class QuorumChecker {
        MONGO_DISALLOW_COPYING(QuorumChecker);
    public:
        /**
         * Constructs a QuorumChecker that is used to confirm that sufficient nodes are up to accept
         * "rsConfig".  "myIndex" is the index of the local node, which is assumed to be up.
         *
         * "rsConfig" must stay in scope until QuorumChecker's destructor completes.
         */
        QuorumChecker(const ReplicaSetConfig* rsConfig, int myIndex);

        /**
         * Executes the quorum check using "executor" for network and event loop operations.
         *
         * Returns Status::OK() if a quorum responded, or an error status otherwise.
         */
        Status run(ReplicationExecutor* executor);

    private:
        /**
         * Initial callback run in the event loop, that schedules remote heartbeats and initializes
         * QuorumChecker state based on the member config of the current node.
         */
        void _startQuorumCheck(const ReplicationExecutor::CallbackData& cbData);

        /**
         * Callback that processes a single heartbeat response.
         */
        void _onQuorumCheckHeartbeatResponse(
                const ReplicationExecutor::RemoteCommandCallbackData& cbData,
                const int memberIndex);

        /**
         * Callback that executes after _haveReceivedSufficientReplies() becomes true.
         *
         * Computes the quorum result based on responses received so far, stores it into
         * _finalStatus, and enables QuorumChecker::run() to return.
         */
        void _onQuorumCheckComplete(const ReplicationExecutor::CallbackData& cbData);

        /**
         * Updates the QuorumChecker state based on the data from a single heartbeat response.
         *
         * Updates state labeled (X) in the concurrency legend, below, and so only safe
         * to call from within executor thread callbacks.
         */
        void _tabulateHeartbeatResponse(
                const ReplicationExecutor::RemoteCommandCallbackData& cbData,
                const int memberIndex);

        /**
         * Returns true if we've received enough responses to conclude the quorum check.
         *
         * Reads state labeled (X) in the concurrency legend, below, and so only safe
         * to call from within executor thread callbacks.
         */
        bool _haveReceivedSufficientReplies() const;

        /**
         * Signals the event that indicates that _haveReceivedSufficientReplies() is now true,
         * unless that event has already been signaled.
         *
         * Called from executor callbacks or the thread executing QuorumChecker::run.
         */
        void _signalSufficientResponsesReceived(ReplicationExecutor* executor);

        // Concurrency legend:
        // (R) Read-only during concurrent operation (after run() calls a schedule method).
        // (X) Only read and modified by executor callbacks.
        // (C) Written by the "startup" or "completion" callback, only, and read by client threads
        //     that have waited for the "completion" callback to complete

        // Pointer to the replica set configuration for which we're checking quorum.
        const ReplicaSetConfig* const _rsConfig;                                // (R)

        // Index of the local node's member configuration in _rsConfig.
        const int _myIndex;                                                     // (R)

        // Event signaled when _haveReceivedSufficientReplies() becomes true.
        ReplicationExecutor::EventHandle _sufficientResponsesReceived;          // (R)

        // List of nodes believed to be down.
        std::vector<HostAndPort> _down;                                         // (X)

        // List of voting nodes that have responded affirmatively.
        std::vector<HostAndPort> _voters;                                       // (X)

        // Total number of responses and timeouts processed.
        int _numResponses;                                                      // (X)

        // Number of electable nodes that have responded affirmatively.
        int _numElectable;                                                      // (X)

        // Set to a non-OK status if a response from a remote node indicates
        // that the quorum check should definitely fail, such as because of
        // a replica set name mismatch.
        Status _vetoStatus;                                                     // (X)

        // List of heartbeat callbacks scheduled by _startQuorumCheck and
        // canceled by run() before it returns.
        std::vector<ReplicationExecutor::CallbackHandle> _hbResponseCallbacks;  // (C)

        // Final status of the quorum check, returned by run().
        Status _finalStatus;                                                    // (C)
    };

    QuorumChecker::QuorumChecker(const ReplicaSetConfig* rsConfig, int myIndex)
        : _rsConfig(rsConfig),
          _myIndex(myIndex),
          _numResponses(0),
          _numElectable(0),
          _vetoStatus(Status::OK()),
          _finalStatus(ErrorCodes::CallbackCanceled, "Quorum check canceled") {

        invariant(myIndex < _rsConfig->getNumMembers());
    }

    Status QuorumChecker::run(ReplicationExecutor* executor) {
        StatusWith<ReplicationExecutor::EventHandle> evh = executor->makeEvent();
        if (!evh.isOK()) {
            return evh.getStatus();
        }
        _sufficientResponsesReceived = evh.getValue();

        StatusWith<ReplicationExecutor::CallbackHandle> finishCheckCallback = executor->onEvent(
                _sufficientResponsesReceived,
                stdx::bind(&QuorumChecker::_onQuorumCheckComplete, this, stdx::placeholders::_1));
        if (!finishCheckCallback.isOK()) {
            _signalSufficientResponsesReceived(executor);  // Clean up.
            return finishCheckCallback.getStatus();
        }

        StatusWith<ReplicationExecutor::CallbackHandle> startCheckCallback = executor->scheduleWork(
                stdx::bind(&QuorumChecker::_startQuorumCheck, this, stdx::placeholders::_1));
        if (!startCheckCallback.isOK()) {
            _signalSufficientResponsesReceived(executor);      // Clean up.
            executor->cancel(finishCheckCallback.getValue());  // Clean up.
            return startCheckCallback.getStatus();
        }

        executor->wait(finishCheckCallback.getValue());

        // Cancel all the callbacks, so that they do not attempt to access QuorumChecker state
        // after this method returns.
        std::for_each(_hbResponseCallbacks.begin(),
                      _hbResponseCallbacks.end(),
                      stdx::bind(&ReplicationExecutor::cancel, executor, stdx::placeholders::_1));

        return _finalStatus;
    }

    void QuorumChecker::_startQuorumCheck(const ReplicationExecutor::CallbackData& cbData) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }
        const bool isInitialConfig = _rsConfig->getConfigVersion() == 1;
        const MemberConfig& myConfig = _rsConfig->getMemberAt(_myIndex);

        if (myConfig.isVoter()) {
            _voters.push_back(myConfig.getHostAndPort());
        }
        if (myConfig.isElectable()) {
            _numElectable = 1;
        }
        _numResponses = 1; // We "responded" to ourself already.

        if (_haveReceivedSufficientReplies()) {
            _signalSufficientResponsesReceived(cbData.executor);
        }

        // TODO: Call a helper to make the request object.
        const BSONObj hbRequest = BSON(
                "replSetHeartbeat" << _rsConfig->getReplSetName() <<
                "v" << _rsConfig->getConfigVersion() <<
                "pv" << 1 <<
                "checkEmpty" << isInitialConfig <<
                "from" << myConfig.getHostAndPort().toString() <<
                "fromId" << myConfig.getId());

        // Send a bunch of heartbeat requests.
        // Schedule an operation when a "sufficient" number of them have completed, and use that
        // to compute the quorum check results.
        // Wait for the "completion" callback to finish, and then it's OK to return the results.
        for (int i = 0; i < _rsConfig->getNumMembers(); ++i) {
            if (_myIndex == i) {
                // No need to check self for liveness or unreadiness.
                continue;
            }
            const StatusWith<ReplicationExecutor::CallbackHandle> cbh =
                cbData.executor->scheduleRemoteCommand(
                        ReplicationExecutor::RemoteCommandRequest(
                                _rsConfig->getMemberAt(i).getHostAndPort(),
                                "admin",
                                hbRequest,
                                _rsConfig->getHeartbeatTimeoutPeriodMillis()),
                        stdx::bind(&QuorumChecker::_onQuorumCheckHeartbeatResponse,
                                   this,
                                   stdx::placeholders::_1,
                                   i));
            if (!cbh.isOK()) {
                _vetoStatus = cbh.getStatus();
                _signalSufficientResponsesReceived(cbData.executor);
                return;
            }
            _hbResponseCallbacks.push_back(cbh.getValue());
        }
    }

    void QuorumChecker::_onQuorumCheckHeartbeatResponse(
            const ReplicationExecutor::RemoteCommandCallbackData& cbData,
            const int memberIndex) {

        if (cbData.response.getStatus() == ErrorCodes::CallbackCanceled) {
            // If this callback has been canceled, it's not safe to look at *this.  However,
            // QuorumChecker::run has already returned or will without the information we
            // can provide here.
            return;
        }

        _tabulateHeartbeatResponse(cbData, memberIndex);

        if (_haveReceivedSufficientReplies()) {
            _signalSufficientResponsesReceived(cbData.executor);
        }
    }

    void QuorumChecker::_onQuorumCheckComplete(const ReplicationExecutor::CallbackData& cbData) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            // If this callback has been canceled, it's not safe to look at *this.  However,
            // QuorumChecker::run has already returned or will without the information we
            // can provide here.
            return;
        }

        if (!_vetoStatus.isOK()) {
            _finalStatus = _vetoStatus;
            return;
        }
        if (_rsConfig->getConfigVersion() == 1 && !_down.empty()) {
            str::stream message;
            message << "Could not contact the following nodes during replica set initiation: " <<
                _down.front().toString();
            for (size_t i = 1; i < _down.size(); ++i) {
                message << ", " << _down[i].toString();
            }
            _finalStatus = Status(ErrorCodes::NodeNotFound, message);
            return;
        }
        if (_numElectable == 0) {
            _finalStatus = Status(
                    ErrorCodes::NodeNotFound, "Quorum check failed because no "
                    "electable nodes responded; at least one required for config");
            return;
        }
        if (int(_voters.size()) < _rsConfig->getMajorityVoteCount()) {
            str::stream message;
            message << "Quorum check failed because not enough voting nodes responded; required " <<
                _rsConfig->getMajorityVoteCount() << " but ";

            if (_voters.size() == 0) {
                message << "none responded";
            }
            else {
                message << "only the following " << _voters.size() <<
                    " voting nodes responded: " << _voters.front().toString();
                for (size_t i = 1; i < _voters.size(); ++i) {
                    message << ", " << _voters[i].toString();
                }
            }
            _finalStatus = Status(ErrorCodes::NodeNotFound, message);
            return;
        }
        _finalStatus = Status::OK();
    }

    void QuorumChecker::_tabulateHeartbeatResponse(
            const ReplicationExecutor::RemoteCommandCallbackData& cbData,
            const int memberIndex) {

        ++_numResponses;
        if (!cbData.response.isOK()) {
            warning() << "Failed to complete heartbeat request to " << cbData.request.target <<
                "; " << cbData.response.getStatus();
            _down.push_back(cbData.request.target);
            return;
        }
        BSONObj res = cbData.response.getValue();
        if (res["mismatch"].trueValue()) {
            std::string message = str::stream() << "Our set name did not match that of " <<
                cbData.request.target.toString();
            _vetoStatus = Status(ErrorCodes::NewReplicaSetConfigurationIncompatible, message);
            warning() << message;
            return;
        }
        if (res.getStringField("set")[0] != '\0') {
            if (res["v"].numberInt() >= _rsConfig->getConfigVersion()) {
                std::string message = str::stream() << "Our config version of " <<
                    _rsConfig->getConfigVersion() <<
                    " is no larger than the version on " << cbData.request.target.toString() <<
                    ", which is " << res["v"].toString();
                _vetoStatus = Status(ErrorCodes::NewReplicaSetConfigurationIncompatible, message);
                warning() << message;
                return;
            }
        }
        if (!res["ok"].trueValue()) {
            warning() << "Got error response on heartbeat request to " << cbData.request.target <<
                "; " << res;
            _down.push_back(_rsConfig->getMemberAt(memberIndex).getHostAndPort());
            return;
        }

        const MemberConfig& memberConfig = _rsConfig->getMemberAt(memberIndex);
        if (memberConfig.isElectable()) {
            ++_numElectable;
        }
        if (memberConfig.isVoter()) {
            _voters.push_back(cbData.request.target);
        }
    }

    bool QuorumChecker::_haveReceivedSufficientReplies() const {
        if (!_vetoStatus.isOK() || _numResponses == _rsConfig->getNumMembers()) {
            // Vetoed or everybody has responded.  All done.
            return true;
        }
        if (_rsConfig->getConfigVersion() == 1) {
            // Have not received responses from every member, and the proposed config
            // version is 1 (initial configuration).  Keep waiting.
            return false;
        }
        if (_numElectable == 0) {
            // Have not heard from at least one electable node.  Keep waiting.
            return false;
        }
        if (int(_voters.size()) < _rsConfig->getMajorityVoteCount()) {
            // Have not heard from a majority of voters.  Keep waiting.
            return false;
        }

        // Have heard from a majority of voters and one electable node.  All done.
        return true;
    }

    void QuorumChecker::_signalSufficientResponsesReceived(ReplicationExecutor* executor) {
        if (_sufficientResponsesReceived.isValid()) {
            executor->signalEvent(_sufficientResponsesReceived);
            _sufficientResponsesReceived = ReplicationExecutor::EventHandle();
        }
    }

}  // namespace

    Status checkQuorumForInitiate(ReplicationExecutor* executor,
                                  const ReplicaSetConfig& rsConfig,
                                  const int myIndex) {
        invariant(rsConfig.getConfigVersion() == 1);
        QuorumChecker checker(&rsConfig, myIndex);
        return checker.run(executor);
    }

    Status checkQuorumForReconfig(ReplicationExecutor* executor,
                                  const ReplicaSetConfig& rsConfig,
                                  const int myIndex) {
        invariant(rsConfig.getConfigVersion() > 1);
        QuorumChecker checker(&rsConfig, myIndex);
        return checker.run(executor);
    }

}  // namespace repl
}  // namespace mongo
