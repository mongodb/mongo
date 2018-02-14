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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplicationRollback

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rollback_impl.h"

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/roll_back_local_operations.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/db/session_catalog.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

RollbackImpl::RollbackImpl(OplogInterface* localOplog,
                           OplogInterface* remoteOplog,
                           StorageInterface* storageInterface,
                           ReplicationProcess* replicationProcess,
                           ReplicationCoordinator* replicationCoordinator,
                           Listener* listener)
    : _localOplog(localOplog),
      _remoteOplog(remoteOplog),
      _storageInterface(storageInterface),
      _replicationProcess(replicationProcess),
      _replicationCoordinator(replicationCoordinator),
      _listener(listener) {

    invariant(localOplog);
    invariant(remoteOplog);
    invariant(storageInterface);
    invariant(replicationProcess);
    invariant(replicationCoordinator);
    invariant(listener);
}

RollbackImpl::RollbackImpl(OplogInterface* localOplog,
                           OplogInterface* remoteOplog,
                           StorageInterface* storageInterface,
                           ReplicationProcess* replicationProcess,
                           ReplicationCoordinator* replicationCoordinator)
    : RollbackImpl(localOplog,
                   remoteOplog,
                   storageInterface,
                   replicationProcess,
                   replicationCoordinator,
                   {}) {}

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

    // Persist the common point to the 'oplogTruncateAfterPoint' document. We save this value so
    // that the replication recovery logic knows where to truncate the oplog. Note that it must be
    // saved *durably* in case a crash occurs after the storage engine recovers to the stable
    // timestamp. Upon startup after such a crash, the standard replication recovery code will know
    // where to truncate the oplog by observing the value of the 'oplogTruncateAfterPoint' document.
    // Note that the storage engine timestamp recovery only restores the database *data* to a stable
    // timestamp, but does not revert the oplog, which must be done as part of the rollback process.
    _replicationProcess->getConsistencyMarkers()->setOplogTruncateAfterPoint(
        opCtx, commonPointSW.getValue());
    _listener->onCommonPointFound(commonPointSW.getValue());

    // Increment the Rollback ID of this node. The Rollback ID is a natural number that it is
    // incremented by 1 every time a rollback occurs. Note that the Rollback ID must be incremented
    // before modifying any local data.
    status = _replicationProcess->incrementRollbackID(opCtx);
    if (!status.isOK()) {
        return status;
    }

    // Recover to the stable timestamp.
    status = _recoverToStableTimestamp(opCtx);
    if (!status.isOK()) {
        return status;
    }
    _listener->onRecoverToStableTimestamp();

    // Run the oplog recovery logic.
    status = _oplogRecovery(opCtx);
    if (!status.isOK()) {
        return status;
    }
    _listener->onRecoverFromOplog();

    // At this point these functions need to always be called before returning, even on failure.
    // These functions fassert on failure.
    ON_BLOCK_EXIT([this, opCtx] {
        auto validator = LogicalTimeValidator::get(opCtx);
        if (validator) {
            validator->resetKeyManagerCache();
        }

        _checkShardIdentityRollback(opCtx);
        _resetSessions(opCtx);
        _transitionFromRollbackToSecondary(opCtx);
    });

    return Status::OK();
}

void RollbackImpl::shutdown() {
    log() << "rollback shutting down";

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

    log() << "transition to ROLLBACK";
    {
        Lock::GlobalWrite globalWrite(opCtx);

        auto status = _replicationCoordinator->setFollowerMode(MemberState::RS_ROLLBACK);
        if (!status.isOK()) {
            status.addContext(str::stream() << "Cannot transition from "
                                            << _replicationCoordinator->getMemberState().toString()
                                            << " to "
                                            << MemberState(MemberState::RS_ROLLBACK).toString());
            log() << status;
            return status;
        }
    }
    return Status::OK();
}

StatusWith<Timestamp> RollbackImpl::_findCommonPoint() {
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }

    log() << "finding common point";

    auto onLocalOplogEntryFn = [](const BSONObj& operation) { return Status::OK(); };

    // Calls syncRollBackLocalOperations to find the common point and run onLocalOplogEntryFn on
    // each oplog entry up until the common point. We only need the Timestamp of the common point
    // for the oplog truncate after point.
    auto commonPointSW =
        syncRollBackLocalOperations(*_localOplog, *_remoteOplog, onLocalOplogEntryFn);
    if (!commonPointSW.isOK()) {
        return commonPointSW.getStatus();
    }

    OpTime commonPoint = commonPointSW.getValue().first;
    OpTime lastCommittedOpTime = _replicationCoordinator->getLastCommittedOpTime();
    OpTime committedSnapshot = _replicationCoordinator->getCurrentCommittedSnapshotOpTime();

    log() << "Rollback common point is " << commonPoint;

    // Rollback common point should be >= the replication commit point.
    invariant(!_replicationCoordinator->isV1ElectionProtocol() ||
              commonPoint.getTimestamp() >= lastCommittedOpTime.getTimestamp());
    invariant(!_replicationCoordinator->isV1ElectionProtocol() ||
              commonPoint >= lastCommittedOpTime);

    // Rollback common point should be >= the committed snapshot optime.
    invariant(commonPoint.getTimestamp() >= committedSnapshot.getTimestamp());
    invariant(commonPoint >= committedSnapshot);

    return commonPoint.getTimestamp();
}

Status RollbackImpl::_recoverToStableTimestamp(OperationContext* opCtx) {
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }
    // Recover to the stable timestamp while holding the global exclusive lock.
    auto serviceCtx = opCtx->getServiceContext();
    {
        Lock::GlobalWrite globalWrite(opCtx);
        try {
            return _storageInterface->recoverToStableTimestamp(serviceCtx);
        } catch (...) {
            return exceptionToStatus();
        }
    }
}

Status RollbackImpl::_oplogRecovery(OperationContext* opCtx) {
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }
    // Run the recovery process.
    _replicationProcess->getReplicationRecovery()->recoverFromOplog(opCtx);
    return Status::OK();
}


void RollbackImpl::_checkShardIdentityRollback(OperationContext* opCtx) {
    invariant(opCtx);

    log() << "checking shard identity document for roll back";

    if (ShardIdentityRollbackNotifier::get(opCtx)->didRollbackHappen()) {
        severe() << "shardIdentity document rollback detected.  Shutting down to clear "
                    "in-memory sharding state.  Restarting this process should safely return it "
                    "to a healthy state";
        fassertFailedNoTrace(40407);
    }
}

void RollbackImpl::_resetSessions(OperationContext* opCtx) {
    invariant(opCtx);

    log() << "resetting in-memory state of active sessions";

    SessionCatalog::get(opCtx)->invalidateSessions(opCtx, boost::none);
}

void RollbackImpl::_transitionFromRollbackToSecondary(OperationContext* opCtx) {
    invariant(opCtx);
    invariant(_replicationCoordinator->getMemberState() == MemberState(MemberState::RS_ROLLBACK));

    log() << "transition to SECONDARY";

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
