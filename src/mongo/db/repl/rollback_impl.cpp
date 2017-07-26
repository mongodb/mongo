/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/db/repl/rollback_impl.h"

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/roll_back_local_operations.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/db/session_catalog.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

RollbackImpl::RollbackImpl(OplogInterface* localOplog,
                           OplogInterface* remoteOplog,
                           ReplicationCoordinator* replicationCoordinator,
                           Listener* listener)
    : _localOplog(localOplog),
      _remoteOplog(remoteOplog),
      _replicationCoordinator(replicationCoordinator),
      _listener(listener) {

    invariant(localOplog);
    invariant(remoteOplog);
    invariant(replicationCoordinator);
    invariant(listener);
}

RollbackImpl::RollbackImpl(OplogInterface* localOplog,
                           OplogInterface* remoteOplog,
                           ReplicationCoordinator* replicationCoordinator)
    : RollbackImpl(localOplog, remoteOplog, replicationCoordinator, {}) {}

RollbackImpl::~RollbackImpl() {
    shutdown();
}

Status RollbackImpl::runRollback(OperationContext* opCtx) {
    auto status = _transitionToRollback(opCtx);
    if (!status.isOK()) {
        return status;
    }
    _listener->onTransitionToRollback();

    auto commonPointSW = _findCommonPoint();
    if (!commonPointSW.isOK()) {
        return commonPointSW.getStatus();
    }
    _listener->onCommonPointFound(commonPointSW.getValue());

    // At this point these functions need to always be called before returning, even on failure.
    // These functions fassert on failure.
    ON_BLOCK_EXIT([this, opCtx] {
        _checkShardIdentityRollback(opCtx);
        _clearSessionTransactionTable(opCtx);
        _transitionFromRollbackToSecondary(opCtx);
    });

    // TODO: The rest of roll back.
    return Status::OK();
}

void RollbackImpl::shutdown() {
    log() << "Rollback - shutting down";

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _inShutdown = true;
}

bool RollbackImpl::_isInShutdown() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _inShutdown;
}

Status RollbackImpl::_transitionToRollback(OperationContext* opCtx) {
    invariant(opCtx);
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }

    log() << "Rollback - transition to ROLLBACK";
    {
        Lock::GlobalWrite globalWrite(opCtx);

        auto status = _replicationCoordinator->setFollowerMode(MemberState::RS_ROLLBACK);
        if (!status.isOK()) {
            std::string msg = str::stream()
                << "Cannot transition from " << _replicationCoordinator->getMemberState().toString()
                << " to " << MemberState(MemberState::RS_ROLLBACK).toString()
                << causedBy(status.reason());
            log() << msg;
            return Status(status.code(), msg);
        }
    }
    return Status::OK();
}

StatusWith<Timestamp> RollbackImpl::_findCommonPoint() {
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }

    log() << "Rollback - finding common point";

    auto onLocalOplogEntryFn = [](const BSONObj& operation) { return Status::OK(); };

    // Calls syncRollBackLocalOperations to find the common point and run onLocalOplogEntryFn on
    // each oplog entry up until the common point. We only need the Timestamp of the common point
    // for the oplog truncate after point.
    auto commonPointSW =
        syncRollBackLocalOperations(*_localOplog, *_remoteOplog, onLocalOplogEntryFn);
    if (!commonPointSW.isOK()) {
        return commonPointSW.getStatus();
    }
    return commonPointSW.getValue().first.getTimestamp();
}

void RollbackImpl::_checkShardIdentityRollback(OperationContext* opCtx) {
    invariant(opCtx);

    log() << "Rollback - checking shard identity document for roll back";

    if (ShardIdentityRollbackNotifier::get(opCtx)->didRollbackHappen()) {
        severe() << "shardIdentity document rollback detected.  Shutting down to clear "
                    "in-memory sharding state.  Restarting this process should safely return it "
                    "to a healthy state";
        fassertFailedNoTrace(40407);
    }
}

void RollbackImpl::_clearSessionTransactionTable(OperationContext* opCtx) {
    invariant(opCtx);

    log() << "Rollback - clearing transaction table";

    SessionCatalog::get(opCtx)->clearTransactionTable();
}

void RollbackImpl::_transitionFromRollbackToSecondary(OperationContext* opCtx) {
    invariant(opCtx);
    invariant(_replicationCoordinator->getMemberState() == MemberState(MemberState::RS_ROLLBACK));

    log() << "Rollback - transition to SECONDARY";

    Lock::GlobalWrite globalWrite(opCtx);

    auto status = _replicationCoordinator->setFollowerMode(MemberState::RS_SECONDARY);
    if (!status.isOK()) {
        severe() << "Failed to transition into " << MemberState(MemberState::RS_SECONDARY)
                 << "; expected to be in state " << MemberState(MemberState::RS_ROLLBACK)
                 << "; found self in " << _replicationCoordinator->getMemberState()
                 << causedBy(status);
        fassertFailedNoTrace(40408);
    }
}

}  // namespace repl
}  // namespace mongo
