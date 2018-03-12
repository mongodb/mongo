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

#include "mongo/db/background.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/roll_back_local_operations.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/session_catalog.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {
namespace {
// Control whether or not the server will write out data files containing deleted documents during
// rollback. This server parameter affects both rollback via refetch and rollback via recovery to
// stable timestamp.
constexpr bool createRollbackFilesDefault = true;
MONGO_EXPORT_SERVER_PARAMETER(createRollbackDataFiles, bool, createRollbackFilesDefault);
}  // namespace

bool RollbackImpl::shouldCreateDataFiles() {
    return createRollbackDataFiles.load();
}

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

    // After successfully transitioning to the ROLLBACK state, we must always transition back to
    // SECONDARY, even if we fail at any point during the rollback process.
    ON_BLOCK_EXIT([this, opCtx] { _transitionFromRollbackToSecondary(opCtx); });

    // Wait for all background index builds to complete before starting the rollback process.
    status = _awaitBgIndexCompletion(opCtx);
    if (!status.isOK()) {
        return status;
    }
    _listener->onBgIndexesComplete();

    auto commonPointSW = _findCommonPoint(opCtx);
    if (!commonPointSW.isOK()) {
        return commonPointSW.getStatus();
    }

    // During replication recovery, we truncate all oplog entries with timestamps greater than or
    // equal to the oplog truncate after point. As a result, we must find the oplog entry after
    // the common point so we do not truncate the common point itself. If we entered rollback,
    // we are guaranteed to have at least one oplog entry after the common point.
    Timestamp truncatePoint = _findTruncateTimestamp(opCtx, commonPointSW.getValue());

    // Persist the truncate point to the 'oplogTruncateAfterPoint' document. We save this value so
    // that the replication recovery logic knows where to truncate the oplog. Note that it must be
    // saved *durably* in case a crash occurs after the storage engine recovers to the stable
    // timestamp. Upon startup after such a crash, the standard replication recovery code will know
    // where to truncate the oplog by observing the value of the 'oplogTruncateAfterPoint' document.
    // Note that the storage engine timestamp recovery only restores the database *data* to a stable
    // timestamp, but does not revert the oplog, which must be done as part of the rollback process.
    _replicationProcess->getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, truncatePoint);
    _listener->onCommonPointFound(commonPointSW.getValue().first.getTimestamp());

    // Increment the Rollback ID of this node. The Rollback ID is a natural number that it is
    // incremented by 1 every time a rollback occurs. Note that the Rollback ID must be incremented
    // before modifying any local data.
    status = _replicationProcess->incrementRollbackID(opCtx);
    if (!status.isOK()) {
        return status;
    }

    // Recover to the stable timestamp.
    auto stableTimestampSW = _recoverToStableTimestamp(opCtx);
    if (!stableTimestampSW.isOK()) {
        return stableTimestampSW.getStatus();
    }
    _listener->onRecoverToStableTimestamp(stableTimestampSW.getValue());

    // Run the oplog recovery logic.
    status = _oplogRecovery(opCtx, stableTimestampSW.getValue());
    if (!status.isOK()) {
        return status;
    }
    _listener->onRecoverFromOplog();

    status = _triggerOpObserver(opCtx);
    if (!status.isOK()) {
        return status;
    }
    _listener->onRollbackOpObserver(_observerInfo);

    return Status::OK();
}

void RollbackImpl::shutdown() {
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

Status RollbackImpl::_awaitBgIndexCompletion(OperationContext* opCtx) {
    invariant(opCtx);
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }

    // Get a list of all databases.
    StorageEngine* storageEngine = opCtx->getServiceContext()->getGlobalStorageEngine();
    std::vector<std::string> dbs;
    {
        Lock::GlobalLock lk(opCtx, MODE_IS, Date_t::max());
        storageEngine->listDatabases(&dbs);
    }

    // Wait for all background operations to complete by waiting on each database.
    std::vector<StringData> dbNames(dbs.begin(), dbs.end());
    log() << "Waiting for all background operations to complete before starting rollback";
    for (auto db : dbNames) {
        LOG(1) << "Waiting for " << BackgroundOperation::numInProgForDb(db)
               << " background operations to complete on database '" << db << "'";
        BackgroundOperation::awaitNoBgOpInProgForDb(db);
        // Check for shutdown again.
        if (_isInShutdown()) {
            return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
        }
    }

    log() << "Finished waiting for background operations to complete before rollback";
    return Status::OK();
}

StatusWith<std::set<NamespaceString>> RollbackImpl::_namespacesForOp(const OplogEntry& oplogEntry) {
    NamespaceString opNss = oplogEntry.getNamespace();
    OpTypeEnum opType = oplogEntry.getOpType();
    std::set<NamespaceString> namespaces;

    // No namespaces for a no-op.
    if (opType == OpTypeEnum::kNoop) {
        return std::set<NamespaceString>();
    }

    // CRUD ops have the proper namespace in the operation 'ns' field.
    if (opType == OpTypeEnum::kInsert || opType == OpTypeEnum::kUpdate ||
        opType == OpTypeEnum::kDelete) {
        return std::set<NamespaceString>({opNss});
    }

    // If the operation is a command, then we need to extract the appropriate namespaces from the
    // command object, as opposed to just using the 'ns' field of the oplog entry itself.
    if (opType == OpTypeEnum::kCommand) {
        auto obj = oplogEntry.getObject();
        auto firstElem = obj.firstElement();

        // Does not handle 'applyOps' entries.
        invariant(oplogEntry.getCommandType() != OplogEntry::CommandType::kApplyOps,
                  "_namespacesForOp does not handle 'applyOps' oplog entries.");

        switch (oplogEntry.getCommandType()) {
            case OplogEntry::CommandType::kRenameCollection: {
                // Add both the 'from' and 'to' namespaces.
                namespaces.insert(NamespaceString(firstElem.valuestrsafe()));
                namespaces.insert(NamespaceString(obj.getStringField("to")));
                break;
            }
            case OplogEntry::CommandType::kDropDatabase: {
                // There is no specific namespace to save for a drop database operation.
                break;
            }
            case OplogEntry::CommandType::kDbCheck:
            case OplogEntry::CommandType::kConvertToCapped:
            case OplogEntry::CommandType::kEmptyCapped: {
                // These commands do not need to be supported by rollback. 'convertToCapped' should
                // always be converted to lower level DDL operations, and 'emptycapped' is a
                // testing-only command.
                std::string message = str::stream() << "Encountered unsupported command type '"
                                                    << firstElem.fieldName()
                                                    << "' during rollback.";
                return Status(ErrorCodes::UnrecoverableRollbackError, message);
            }
            case OplogEntry::CommandType::kCreate:
            case OplogEntry::CommandType::kDrop:
            case OplogEntry::CommandType::kCreateIndexes:
            case OplogEntry::CommandType::kDropIndexes:
            case OplogEntry::CommandType::kCollMod: {
                // For all other command types, we should be able to parse the collection name from
                // the first command argument.
                try {
                    auto cmdNss = CommandHelpers::parseNsCollectionRequired(opNss.db(), obj);
                    namespaces.insert(cmdNss);
                } catch (const DBException& ex) {
                    return ex.toStatus();
                }
                break;
            }
            case OplogEntry::CommandType::kApplyOps:
            default:
                // Every possible command type should be handled above.
                MONGO_UNREACHABLE
        }
    }

    return namespaces;
}

/**
 * Process a single oplog entry that is getting rolled back and update the necessary rollback info
 * structures.
 */
Status RollbackImpl::_processRollbackOp(const OplogEntry& oplogEntry) {
    NamespaceString opNss = oplogEntry.getNamespace();
    OpTypeEnum opType = oplogEntry.getOpType();

    // For applyOps entries, we process each sub-operation individually.
    if (opType == OpTypeEnum::kCommand &&
        oplogEntry.getCommandType() == OplogEntry::CommandType::kApplyOps) {
        try {
            auto subOps = ApplyOps::extractOperations(oplogEntry);
            for (auto& subOp : subOps) {
                auto subStatus = _processRollbackOp(subOp);
                if (!subStatus.isOK()) {
                    return subStatus;
                }
            }
            return Status::OK();
        } catch (DBException& e) {
            return e.toStatus();
        }
    }

    // No information to record for a no-op.
    if (opType == OpTypeEnum::kNoop) {
        return Status::OK();
    }

    // Extract the appropriate namespaces from the oplog operation.
    auto namespacesSW = _namespacesForOp(oplogEntry);
    if (!namespacesSW.isOK()) {
        return namespacesSW.getStatus();
    } else {
        _observerInfo.rollbackNamespaces.insert(namespacesSW.getValue().begin(),
                                                namespacesSW.getValue().end());
    }

    // If the operation being rolled back has a session id, then we add it to the set of
    // sessions that had operations rolled back.
    OperationSessionInfo opSessionInfo = oplogEntry.getOperationSessionInfo();
    auto sessionId = opSessionInfo.getSessionId();
    if (sessionId) {
        _observerInfo.rollbackSessionIds.insert(sessionId->getId());
    }

    // Check if the creation of the shard identity document is being rolled back.
    if (opType == OpTypeEnum::kInsert) {
        auto idVal = oplogEntry.getObject().getStringField("_id");
        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
            opNss == NamespaceString::kServerConfigurationNamespace &&
            idVal == ShardIdentityType::IdName) {
            _observerInfo.shardIdentityRolledBack = true;
            warning() << "Shard identity document rollback detected. oplog op: "
                      << oplogEntry.toBSON();
        }
    }

    return Status::OK();
}

StatusWith<RollBackLocalOperations::RollbackCommonPoint> RollbackImpl::_findCommonPoint(
    OperationContext* opCtx) {
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }

    log() << "finding common point";

    // We save some aggregate information about all operations that are rolled back, so that we can
    // pass this information to the rollback op observer. In most cases, other subsystems do not
    // need to know extensive details about every operation that rolled back, so to reduce
    // complexity by adding observer methods for every operation type, we provide a set of
    // information that should be suitable for most other subsystems to take the necessary actions
    // on a rollback event. This rollback info is kept in memory, so if we crash after we collect
    // it, it may be lost. However, if we crash any time between recovering to a stable timestamp
    // and completing oplog recovery, we assume that this information is not needed, since the node
    // restarting will have cleared out any invalid in-memory state anyway.
    auto onLocalOplogEntryFn = [&](const BSONObj& operation) {
        OplogEntry oplogEntry(operation);
        return _processRollbackOp(oplogEntry);
    };

    // Calls syncRollBackLocalOperations to find the common point and run onLocalOplogEntryFn on
    // each oplog entry up until the common point. We only need the Timestamp of the common point
    // for the oplog truncate after point. Along the way, we save some information about the
    // rollback ops.
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

    return commonPointSW.getValue();
}

Timestamp RollbackImpl::_findTruncateTimestamp(
    OperationContext* opCtx, RollBackLocalOperations::RollbackCommonPoint commonPoint) const {

    AutoGetCollectionForRead oplog(opCtx, NamespaceString::kRsOplogNamespace);
    invariant(oplog.getCollection());
    auto oplogCursor = oplog.getCollection()->getCursor(opCtx, /*forward=*/true);

    auto commonPointRecord = oplogCursor->seekExact(commonPoint.second);
    // Check that we've found the right document for the common point.
    invariant(commonPointRecord);
    auto commonPointTime = OpTime::parseFromOplogEntry(commonPointRecord->data.releaseToBson());
    invariantOK(commonPointTime.getStatus());
    invariant(commonPointTime.getValue() == commonPoint.first,
              str::stream() << "Common point: " << commonPoint.first.toString()
                            << ", record found: "
                            << commonPointTime.getValue().toString());

    // Get the next document, which will be the first document to truncate.
    auto truncatePointRecord = oplogCursor->next();
    invariant(truncatePointRecord);
    auto truncatePointTime = OpTime::parseFromOplogEntry(truncatePointRecord->data.releaseToBson());
    invariantOK(truncatePointTime.getStatus());

    log() << "Marking to truncate all oplog entries with timestamps greater than or equal to "
          << truncatePointTime.getValue();
    return truncatePointTime.getValue().getTimestamp();
}

StatusWith<Timestamp> RollbackImpl::_recoverToStableTimestamp(OperationContext* opCtx) {
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

Status RollbackImpl::_oplogRecovery(OperationContext* opCtx, Timestamp stableTimestamp) {
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }
    // Run the recovery process.
    _replicationProcess->getReplicationRecovery()->recoverFromOplog(opCtx, stableTimestamp);
    return Status::OK();
}

Status RollbackImpl::_triggerOpObserver(OperationContext* opCtx) {
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }
    log() << "Triggering the rollback op observer";
    opCtx->getServiceContext()->getOpObserver()->onReplicationRollback(opCtx, _observerInfo);
    return Status::OK();
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
