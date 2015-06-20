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

SyncSourceFeedback::SyncSourceFeedback() : _positionChanged(false), _shutdownSignaled(false) {}
SyncSourceFeedback::~SyncSourceFeedback() {}

void SyncSourceFeedback::_resetConnection() {
    LOG(1) << "resetting connection in sync source feedback";
    _connection.reset();
}

bool SyncSourceFeedback::replAuthenticate() {
    if (!getGlobalAuthorizationManager()->isAuthEnabled())
        return true;

    if (!isInternalAuthSet())
        return false;
    return authenticateInternalUser(_connection.get());
}

bool SyncSourceFeedback::_connect(OperationContext* txn, const HostAndPort& host) {
    if (hasConnection()) {
        return true;
    }
    log() << "setting syncSourceFeedback to " << host.toString();
    _connection.reset(new DBClientConnection(false, OplogReader::tcp_timeout));
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

    return hasConnection();
}

void SyncSourceFeedback::forwardSlaveProgress() {
    stdx::unique_lock<stdx::mutex> lock(_mtx);
    _positionChanged = true;
    _cond.notify_all();
}

Status SyncSourceFeedback::updateUpstream(OperationContext* txn) {
    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
    if (replCoord->getMemberState().primary()) {
        // primary has no one to update to
        return Status::OK();
    }
    BSONObjBuilder cmd;
    {
        stdx::unique_lock<stdx::mutex> lock(_mtx);
        // the command could not be created, likely because the node was removed from the set
        if (!replCoord->prepareReplSetUpdatePositionCommand(&cmd)) {
            return Status::OK();
        }
    }
    BSONObj res;

    LOG(2) << "Sending slave oplog progress to upstream updater: " << cmd.done();
    try {
        _connection->runCommand("admin", cmd.obj(), res);
    } catch (const DBException& e) {
        log() << "SyncSourceFeedback error sending update: " << e.what() << endl;
        // blacklist sync target for .5 seconds and find a new one
        replCoord->blacklistSyncSource(_syncTarget, Date_t::now() + Milliseconds(500));
        BackgroundSync::get()->clearSyncTarget();
        _resetConnection();
        return e.toStatus();
    }

    Status status = Command::getStatusFromCommandResult(res);
    if (!status.isOK()) {
        log() << "SyncSourceFeedback error sending update, response: " << res.toString() << endl;
        // blacklist sync target for .5 seconds and find a new one, unless we were rejected due
        // to the syncsource having a newer config
        if (status != ErrorCodes::InvalidReplicaSetConfig || res["cfgver"].eoo() ||
            res["cfgver"].numberLong() < replCoord->getConfig().getConfigVersion()) {
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

    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
    while (true) {  // breaks once _shutdownSignaled is true
        {
            stdx::unique_lock<stdx::mutex> lock(_mtx);
            while (!_positionChanged && !_shutdownSignaled) {
                _cond.wait(lock);
            }

            if (_shutdownSignaled) {
                break;
            }

            _positionChanged = false;
        }

        auto txn = cc().makeOperationContext();
        MemberState state = replCoord->getMemberState();
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
            if (target.empty()) {
                sleepmillis(500);
                stdx::unique_lock<stdx::mutex> lock(_mtx);
                _positionChanged = true;
                continue;
            }
            if (!_connect(txn.get(), target)) {
                sleepmillis(500);
                stdx::unique_lock<stdx::mutex> lock(_mtx);
                _positionChanged = true;
                continue;
            }
        }
        Status status = updateUpstream(txn.get());
        if (!status.isOK()) {
            sleepmillis(500);
            stdx::unique_lock<stdx::mutex> lock(_mtx);
            _positionChanged = true;
        }
    }
}
}  // namespace repl
}  // namespace mongo
