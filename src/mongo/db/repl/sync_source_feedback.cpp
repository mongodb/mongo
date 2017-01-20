/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/repl/sync_source_feedback.h"

#include "mongo/client/constants.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

using std::endl;
using std::string;

namespace repl {

void SyncSourceFeedback::_resetConnection() {
    LOG(1) << "resetting connection in sync source feedback";
    _connection.reset();
    _fallBackToOldUpdatePosition = false;
}

bool SyncSourceFeedback::replAuthenticate() {
    if (!getGlobalAuthorizationManager()->isAuthEnabled())
        return true;

    if (!isInternalAuthSet())
        return false;
    return _connection->authenticateInternalUser();
}

bool SyncSourceFeedback::_connect(OperationContext* txn, const HostAndPort& host) {
    if (hasConnection()) {
        return true;
    }
    log() << "setting syncSourceFeedback to " << host.toString();
    _connection.reset(
        new DBClientConnection(false, durationCount<Seconds>(OplogReader::kSocketTimeout)));
    string errmsg;
    try {
        if (!_connection->connect(host, errmsg) ||
            (getGlobalAuthorizationManager()->isAuthEnabled() && !replAuthenticate())) {
            _resetConnection();
            log() << errmsg << endl;
            return false;
        }
    } catch (const DBException& e) {
        error() << "Error connecting to " << host.toString() << ": " << e.what();
        _resetConnection();
        return false;
    }

    // Update keepalive value from config.
    auto rsConfig = repl::ReplicationCoordinator::get(txn)->getConfig();
    _keepAliveInterval = rsConfig.getElectionTimeoutPeriod() / 2;

    return hasConnection();
}

void SyncSourceFeedback::forwardSlaveProgress() {
    stdx::lock_guard<stdx::mutex> lock(_mtx);
    _positionChanged = true;
    _cond.notify_all();
}

Status SyncSourceFeedback::updateUpstream(OperationContext* txn, bool oldStyle) {
    auto replCoord = repl::ReplicationCoordinator::get(txn);
    if (replCoord->getMemberState().primary()) {
        // Primary has no one to send updates to.
        return Status::OK();
    }
    BSONObjBuilder cmd;
    // The command could not be created, likely because this node was removed from the set.
    if (!oldStyle) {
        if (!replCoord->prepareReplSetUpdatePositionCommand(&cmd)) {
            return Status::OK();
        }
    } else {
        if (!replCoord->prepareOldReplSetUpdatePositionCommand(&cmd)) {
            return Status::OK();
        }
    }
    BSONObj res;

    LOG(2) << "Sending slave oplog progress to upstream updater: " << cmd.done();
    try {
        _connection->runCommand("admin", cmd.obj(), res);
    } catch (const DBException& e) {
        log() << "SyncSourceFeedback error sending " << (oldStyle ? "old style " : "")
              << "update: " << e.what();
        // Blacklist sync target for .5 seconds and find a new one.
        replCoord->blacklistSyncSource(_syncTarget, Date_t::now() + Milliseconds(500));
        BackgroundSync::get()->clearSyncTarget();
        _resetConnection();
        return e.toStatus();
    }

    Status status = Command::getStatusFromCommandResult(res);
    if (!status.isOK()) {
        log() << "SyncSourceFeedback error sending " << (oldStyle ? "old style " : "")
              << "update, response: " << res.toString();
        if (status == ErrorCodes::BadValue && !oldStyle) {
            log() << "SyncSourceFeedback falling back to old style UpdatePosition command";
            _fallBackToOldUpdatePosition = true;
        } else if (status != ErrorCodes::InvalidReplicaSetConfig || res["configVersion"].eoo() ||
                   res["configVersion"].numberLong() < replCoord->getConfig().getConfigVersion()) {
            // Blacklist sync target for .5 seconds and find a new one, unless we were rejected due
            // to the syncsource having a newer config.
            replCoord->blacklistSyncSource(_syncTarget, Date_t::now() + Milliseconds(500));
            BackgroundSync::get()->clearSyncTarget();
            _resetConnection();
        }
    }

    return status;
}

void SyncSourceFeedback::shutdown() {
    stdx::unique_lock<stdx::mutex> lock(_mtx);
    _shutdownSignaled = true;
    _cond.notify_all();
}

void SyncSourceFeedback::run() {
    Client::initThread("SyncSourceFeedback");

    while (true) {  // breaks once _shutdownSignaled is true
        auto txn = cc().makeOperationContext();
        {
            stdx::unique_lock<stdx::mutex> lock(_mtx);
            while (!_positionChanged && !_shutdownSignaled) {
                if (_cond.wait_for(lock, _keepAliveInterval) == stdx::cv_status::timeout) {
                    MemberState state = ReplicationCoordinator::get(txn.get())->getMemberState();
                    if (!(state.primary() || state.startup())) {
                        break;
                    }
                }
            }

            if (_shutdownSignaled) {
                break;
            }

            _positionChanged = false;
        }

        MemberState state = ReplicationCoordinator::get(txn.get())->getMemberState();
        if (state.primary() || state.startup()) {
            _resetConnection();
            continue;
        }
        const HostAndPort target = BackgroundSync::get()->getSyncTarget();
        if (_syncTarget != target) {
            _resetConnection();
            _syncTarget = target;
        }
        if (!hasConnection()) {
            // fix connection if need be
            if (target.empty() || !_connect(txn.get(), target)) {
                // Loop back around again; the keepalive functionality will cause us to retry
                continue;
            }
        }
        bool oldFallBackValue = _fallBackToOldUpdatePosition;
        Status status = updateUpstream(txn.get(), _fallBackToOldUpdatePosition);
        if (!status.isOK()) {
            if (_fallBackToOldUpdatePosition != oldFallBackValue) {
                stdx::unique_lock<stdx::mutex> lock(_mtx);
                _positionChanged = true;
            } else {
                log() << (_fallBackToOldUpdatePosition ? "old style " : "") << "updateUpstream"
                      << " failed: " << status << ", will retry";
            }
        }
    }
}
}  // namespace repl
}  // namespace mongo
