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
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repair_database_and_check_version.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/roll_back_local_operations.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/session_catalog.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {
namespace {

RollbackImpl::Listener kNoopListener;
RollbackImpl::RollbackTimeLimitHolder kRollbackTimeLimitHolder;

// Control whether or not the server will write out data files containing deleted documents during
// rollback. This server parameter affects both rollback via refetch and rollback via recovery to
// stable timestamp.
constexpr bool createRollbackFilesDefault = true;
MONGO_EXPORT_SERVER_PARAMETER(createRollbackDataFiles, bool, createRollbackFilesDefault);

// The name of the insert, update and delete commands as found in oplog command entries.
constexpr auto kInsertCmdName = "insert"_sd;
constexpr auto kUpdateCmdName = "update"_sd;
constexpr auto kDeleteCmdName = "delete"_sd;
}  // namespace

constexpr const char* RollbackImpl::kRollbackRemoveSaverType;
constexpr const char* RollbackImpl::kRollbackRemoveSaverWhy;

bool RollbackImpl::shouldCreateDataFiles() {
    return createRollbackDataFiles.load();
}

RollbackImpl::RollbackImpl(OplogInterface* localOplog,
                           OplogInterface* remoteOplog,
                           StorageInterface* storageInterface,
                           ReplicationProcess* replicationProcess,
                           ReplicationCoordinator* replicationCoordinator,
                           Listener* listener)
    : _listener(listener),
      _localOplog(localOplog),
      _remoteOplog(remoteOplog),
      _storageInterface(storageInterface),
      _replicationProcess(replicationProcess),
      _replicationCoordinator(replicationCoordinator) {

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
                   &kNoopListener) {}

RollbackImpl::~RollbackImpl() {
    shutdown();
}

Status RollbackImpl::runRollback(OperationContext* opCtx) {
    _rollbackStats.startTime = opCtx->getServiceContext()->getFastClockSource()->now();

    auto status = _transitionToRollback(opCtx);
    if (!status.isOK()) {
        return status;
    }
    _listener->onTransitionToRollback();

    // We clear the SizeRecoveryState before we recover to a stable timestamp. This ensures that we
    // only use size adjustment markings from the storage and replication recovery processes in this
    // rollback.
    sizeRecoveryState(opCtx->getServiceContext()).clearStateBeforeRecovery();

    // After successfully transitioning to the ROLLBACK state, we must always transition back to
    // SECONDARY, even if we fail at any point during the rollback process.
    ON_BLOCK_EXIT([this, opCtx] { _transitionFromRollbackToSecondary(opCtx); });
    ON_BLOCK_EXIT([this, opCtx] { _summarizeRollback(opCtx); });

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

    const auto commonPoint = commonPointSW.getValue();
    const OpTime commonPointOpTime = commonPoint.getOpTime();
    _rollbackStats.commonPoint = commonPointOpTime;
    _listener->onCommonPointFound(commonPointOpTime.getTimestamp());

    // Now that we have found the common point, we make sure to proceed only if the rollback
    // period is not too long.
    status = _checkAgainstTimeLimit(commonPoint);
    if (!status.isOK()) {
        return status;
    }

    // Ask the record store for the pre-rollback counts of any collections whose counts will change
    // and create a map with the adjusted counts for post-rollback. While finding the common
    // point, we keep track of how much each collection's count will change during the rollback.
    // Note: these numbers are relative to the common point, not the stable timestamp, and thus
    // must be set after recovering from the oplog.
    status = _findRecordStoreCounts(opCtx);
    if (!status.isOK()) {
        return status;
    }

    // Increment the Rollback ID of this node. The Rollback ID is a natural number that it is
    // incremented by 1 every time a rollback occurs. Note that the Rollback ID must be incremented
    // before modifying any local data.
    status = _replicationProcess->incrementRollbackID(opCtx);
    if (!status.isOK()) {
        return status;
    }
    _rollbackStats.rollbackId = _replicationProcess->getRollbackID();

    if (shouldCreateDataFiles()) {
        // Write a rollback file for each namespace that has documents that would be deleted by
        // rollback.
        status = _writeRollbackFiles(opCtx);
        if (!status.isOK()) {
            return status;
        }
    } else {
        log() << "Not writing rollback files. 'createRollbackDataFiles' set to false.";
    }

    // Recover to the stable timestamp.
    auto stableTimestampSW = _recoverToStableTimestamp(opCtx);
    if (!stableTimestampSW.isOK()) {
        return stableTimestampSW.getStatus();
    }
    _rollbackStats.stableTimestamp = stableTimestampSW.getValue();
    _listener->onRecoverToStableTimestamp(stableTimestampSW.getValue());

    // Log the total number of insert and update operations that have been rolled back as a result
    // of recovering to the stable timestamp.
    log() << "Rollback reverted " << _observerInfo.rollbackCommandCounts[kInsertCmdName]
          << " insert operations, " << _observerInfo.rollbackCommandCounts[kUpdateCmdName]
          << " update operations and " << _observerInfo.rollbackCommandCounts[kDeleteCmdName]
          << " delete operations.";

    // During replication recovery, we truncate all oplog entries with timestamps greater than or
    // equal to the oplog truncate after point. As a result, we must find the oplog entry after
    // the common point so we do not truncate the common point itself. If we entered rollback,
    // we are guaranteed to have at least one oplog entry after the common point.
    Timestamp truncatePoint = _findTruncateTimestamp(opCtx, commonPointSW.getValue());

    // We cannot have an interrupt point between setting the oplog truncation point and fixing the
    // record store counts or else a clean shutdown could produce incorrect counts. We explicitly
    // check for shutdown here to safely maximize interruptibility.
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }

    // Persist the truncate point to the 'oplogTruncateAfterPoint' document. We save this value so
    // that the replication recovery logic knows where to truncate the oplog. We save this value
    // durably to match the behavior during startup recovery. This must occur after we successfully
    // recover to a stable timestamp. If recovering to a stable timestamp fails and we still
    // truncate the oplog then the oplog will not match the data files. If we crash at any earlier
    // point, we will recover, find a new sync source, and restart roll back (if necessary on the
    // new sync source). This is safe because a crash before this point would recover to a stable
    // checkpoint anyways at or earlier than the stable timestamp.
    //
    // Note that storage engine timestamp recovery only restores the database *data* to a stable
    // timestamp, but does not revert the oplog, which must be done as part of the rollback process.
    _replicationProcess->getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, truncatePoint);
    _rollbackStats.truncateTimestamp = truncatePoint;
    _listener->onSetOplogTruncateAfterPoint(truncatePoint);

    // Align the drop pending reaper state with what's on disk. Oplog recovery depends on those
    // being consistent.
    _resetDropPendingState(opCtx);

    // Run the recovery process.
    _replicationProcess->getReplicationRecovery()->recoverFromOplog(opCtx,
                                                                    stableTimestampSW.getValue());
    _listener->onRecoverFromOplog();

    // Sets the correct post-rollback counts on any collections whose counts changed during the
    // rollback.
    _correctRecordStoreCounts(opCtx);

    // At this point, the last applied and durable optimes on this node still point to ops on
    // the divergent branch of history. We therefore update the last optimes to the top of the
    // oplog, which should now be at the common point.
    _replicationCoordinator->resetLastOpTimesFromOplog(
        opCtx, ReplicationCoordinator::DataConsistency::Consistent);
    status = _triggerOpObserver(opCtx);
    if (!status.isOK()) {
        return status;
    }
    _listener->onRollbackOpObserver(_observerInfo);

    log() << "Rollback complete";

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
    StorageEngine* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    std::vector<std::string> dbs;
    {
        Lock::GlobalLock lk(opCtx, MODE_IS);
        storageEngine->listDatabases(&dbs);
    }

    // Wait for all background operations to complete by waiting on each database.
    std::vector<StringData> dbNames(dbs.begin(), dbs.end());
    log() << "Waiting for all background operations to complete before starting rollback";
    for (auto db : dbNames) {
        auto numInProg = BackgroundOperation::numInProgForDb(db);
        if (numInProg > 0) {
            LOG(1) << "Waiting for " << numInProg
                   << " background operations to complete on database '" << db << "'";
            BackgroundOperation::awaitNoBgOpInProgForDb(db);
        }

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

void RollbackImpl::_correctRecordStoreCounts(OperationContext* opCtx) {
    // This function explicitly does not check for shutdown since a clean shutdown post oplog
    // truncation is not allowed to occur until the record store counts are corrected.
    const auto& uuidCatalog = UUIDCatalog::get(opCtx);
    for (const auto& uiCount : _newCounts) {
        const auto uuid = uiCount.first;
        const auto coll = uuidCatalog.lookupCollectionByUUID(uuid);
        invariant(coll,
                  str::stream() << "The collection with UUID " << uuid
                                << " is unexpectedly missing in the UUIDCatalog");
        const auto nss = coll->ns();
        invariant(!nss.isEmpty(),
                  str::stream() << "The collection with UUID " << uuid << " has no namespace.");
        const auto ident = coll->getRecordStore()->getIdent();
        invariant(!ident.empty(),
                  str::stream() << "The collection with UUID " << uuid << " has no ident.");

        const auto newCount = uiCount.second;
        // If the collection is marked for size adjustment, then we made sure the collection size
        // was accurate at the stable timestamp and we can trust replication recovery to keep it
        // correct. This is necessary for capped collections whose deletions will be untracked
        // if we just set the collection count here.
        if (sizeRecoveryState(opCtx->getServiceContext())
                .collectionAlwaysNeedsSizeAdjustment(ident)) {
            LOG(2) << "Not setting collection count to " << newCount << " for " << nss.ns() << " ("
                   << uuid.toString() << ") [" << ident
                   << "] because it is marked for size adjustment.";
            continue;
        }

        auto status =
            _storageInterface->setCollectionCount(opCtx, {nss.db().toString(), uuid}, newCount);
        if (!status.isOK()) {
            // We ignore errors here because crashing or leaving rollback would only leave
            // collection counts more inaccurate.
            warning() << "Failed to set count of " << nss.ns() << " (" << uuid.toString() << ") ["
                      << ident << "] to " << newCount << ". Received: " << status;
        } else {
            LOG(2) << "Set collection count of " << nss.ns() << " (" << uuid.toString() << ") ["
                   << ident << "] to " << newCount << ".";
        }
    }
}

Status RollbackImpl::_findRecordStoreCounts(OperationContext* opCtx) {
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }
    const auto& uuidCatalog = UUIDCatalog::get(opCtx);

    log() << "finding record store counts";
    for (const auto& uiCount : _countDiffs) {
        auto uuid = uiCount.first;
        auto countDiff = uiCount.second;
        if (countDiff == 0) {
            continue;
        }

        const auto nss = uuidCatalog.lookupNSSByUUID(uuid);
        invariant(!nss.isEmpty(),
                  str::stream() << "The collection with UUID " << uuid
                                << " is unexpectedly missing in the UUIDCatalog");
        auto countSW = _storageInterface->getCollectionCount(opCtx, {nss.db().toString(), uuid});
        if (!countSW.isOK()) {
            return countSW.getStatus();
        }
        auto oldCount = countSW.getValue();
        if (oldCount > static_cast<uint64_t>(std::numeric_limits<long long>::max())) {
            warning() << "Count for " << nss.ns() << " (" << uuid.toString() << ") was " << oldCount
                      << " which is larger than the maximum int64_t value. Not attempting to fix "
                         "count during rollback.";
            continue;
        }

        long long oldCountSigned = static_cast<long long>(oldCount);
        auto newCount = oldCountSigned + countDiff;

        if (newCount < 0) {
            warning() << "Attempted to set count for " << nss.ns() << " (" << uuid.toString()
                      << ") to " << newCount
                      << " but set it to 0 instead. This is likely due to the count previously "
                         "becoming inconsistent from an unclean shutdown or a rollback that could "
                         "not fix the count correctly. Old count: "
                      << oldCount << ". Count change: " << countDiff;
            newCount = 0;
        }
        LOG(2) << "Record count of " << nss.ns() << " (" << uuid.toString()
               << ") before rollback is " << oldCount << ". Setting it to " << newCount
               << ", due to change of " << countDiff;
        _newCounts[uuid] = newCount;
    }

    return Status::OK();
}

/**
 * Process a single oplog entry that is getting rolled back and update the necessary rollback info
 * structures.
 */
Status RollbackImpl::_processRollbackOp(const OplogEntry& oplogEntry) {
    ++_observerInfo.numberOfEntriesObserved;

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

    // Keep track of the _ids of inserted and updated documents, as we may need to write them out to
    // a rollback file.
    if (opType == OpTypeEnum::kInsert || opType == OpTypeEnum::kUpdate) {
        const auto uuid = oplogEntry.getUuid();
        invariant(uuid,
                  str::stream() << "Oplog entry to roll back is unexpectedly missing a UUID: "
                                << redact(oplogEntry.toBSON()));
        const auto idElem = oplogEntry.getIdElement();
        if (!idElem.eoo()) {
            // We call BSONElement::wrap() on each _id element to create a new BSONObj with an owned
            // buffer, as the underlying storage may be gone when we access this map to write
            // rollback files.
            _observerInfo.rollbackDeletedIdsMap[uuid.get()].insert(idElem.wrap());
            const auto cmdName = opType == OpTypeEnum::kInsert ? kInsertCmdName : kUpdateCmdName;
            ++_observerInfo.rollbackCommandCounts[cmdName];
        }
    }

    if (opType == OpTypeEnum::kInsert) {
        auto idVal = oplogEntry.getObject().getStringField("_id");
        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
            opNss == NamespaceString::kServerConfigurationNamespace &&
            idVal == ShardIdentityType::IdName) {
            // Check if the creation of the shard identity document is being rolled back.
            _observerInfo.shardIdentityRolledBack = true;
            warning() << "Shard identity document rollback detected. oplog op: "
                      << redact(oplogEntry.toBSON());
        } else if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer &&
                   opNss == VersionType::ConfigNS) {
            // Check if the creation of the config server config version document is being rolled
            // back.
            _observerInfo.configServerConfigVersionRolledBack = true;
            warning() << "Config version document rollback detected. oplog op: "
                      << redact(oplogEntry.toBSON());
        }

        // Rolling back an insert must decrement the count by 1.
        _countDiffs[oplogEntry.getUuid().get()] -= 1;
    } else if (opType == OpTypeEnum::kDelete) {
        // Rolling back a delete must increment the count by 1.
        _countDiffs[oplogEntry.getUuid().get()] += 1;
    } else if (opType == OpTypeEnum::kCommand) {
        if (oplogEntry.getCommandType() == OplogEntry::CommandType::kCreate) {
            // If we roll back a create, then we do not need to change the size of that uuid.
            _countDiffs.erase(oplogEntry.getUuid().get());
        }
    }

    // Keep count of major commands that will be rolled back.
    if (opType == OpTypeEnum::kCommand) {
        ++_observerInfo.rollbackCommandCounts[oplogEntry.getObject().firstElementFieldName()];
    }
    if (opType == OpTypeEnum::kDelete) {
        ++_observerInfo.rollbackCommandCounts[kDeleteCmdName];
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

    OpTime commonPointOpTime = commonPointSW.getValue().getOpTime();
    OpTime lastCommittedOpTime = _replicationCoordinator->getLastCommittedOpTime();
    OpTime committedSnapshot = _replicationCoordinator->getCurrentCommittedSnapshotOpTime();

    log() << "Rollback common point is " << commonPointOpTime;

    // Rollback common point should be >= the replication commit point.
    invariant(!_replicationCoordinator->isV1ElectionProtocol() ||
              commonPointOpTime.getTimestamp() >= lastCommittedOpTime.getTimestamp());
    invariant(!_replicationCoordinator->isV1ElectionProtocol() ||
              commonPointOpTime >= lastCommittedOpTime);

    // Rollback common point should be >= the committed snapshot optime.
    invariant(commonPointOpTime.getTimestamp() >= committedSnapshot.getTimestamp());
    invariant(commonPointOpTime >= committedSnapshot);

    return commonPointSW.getValue();
}

Status RollbackImpl::_checkAgainstTimeLimit(
    RollBackLocalOperations::RollbackCommonPoint commonPoint) {

    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }

    auto localOplogIter = _localOplog->makeIterator();
    const auto topOfOplogSW = localOplogIter->next();
    if (!topOfOplogSW.isOK()) {
        return Status(ErrorCodes::OplogStartMissing, "no oplog during rollback");
    }
    const auto topOfOplogBSON = topOfOplogSW.getValue().first;
    const auto topOfOplog = uassertStatusOK(OplogEntry::parse(topOfOplogBSON));

    _rollbackStats.lastLocalOptime = topOfOplog.getOpTime();

    auto topOfOplogWallOpt = topOfOplog.getWallClockTime();
    auto commonPointWallOpt = commonPoint.getWallClockTime();

    // Only compute the difference if both the top of the oplog and the common point
    // have wall clock times.
    if (commonPointWallOpt && topOfOplogWallOpt) {
        auto topOfOplogWallTime = topOfOplogWallOpt.get();
        auto commonPointWallTime = commonPointWallOpt.get();

        if (topOfOplogWallTime >= commonPointWallTime) {

            unsigned long long diff =
                durationCount<Seconds>(Milliseconds(topOfOplogWallTime - commonPointWallTime));

            _rollbackStats.lastLocalWallClockTime = topOfOplogWallTime;
            _rollbackStats.commonPointWallClockTime = commonPointWallTime;

            auto timeLimit = kRollbackTimeLimitHolder.getRollbackTimeLimit();

            if (diff > timeLimit) {
                return Status(ErrorCodes::UnrecoverableRollbackError,
                              str::stream() << "not willing to roll back more than " << timeLimit
                                            << " seconds of data. Have: "
                                            << diff
                                            << " seconds.");
            }

        } else {
            warning() << "Wall clock times on oplog entries not monotonically increasing. This "
                         "might indicate a backward clock skew. Time at common point: "
                      << commonPointWallTime << ". Time at top of oplog: " << topOfOplogWallTime;
        }
    }

    return Status::OK();
}

Timestamp RollbackImpl::_findTruncateTimestamp(
    OperationContext* opCtx, RollBackLocalOperations::RollbackCommonPoint commonPoint) const {

    AutoGetCollectionForRead oplog(opCtx, NamespaceString::kRsOplogNamespace);
    invariant(oplog.getCollection());
    auto oplogCursor = oplog.getCollection()->getCursor(opCtx, /*forward=*/true);

    auto commonPointRecord = oplogCursor->seekExact(commonPoint.getRecordId());
    auto commonPointOpTime = commonPoint.getOpTime();
    // Check that we've found the right document for the common point.
    invariant(commonPointRecord);
    auto commonPointTime = OpTime::parseFromOplogEntry(commonPointRecord->data.releaseToBson());
    invariant(commonPointTime.getStatus());
    invariant(commonPointTime.getValue() == commonPointOpTime,
              str::stream() << "Common point: " << commonPointOpTime.toString()
                            << ", record found: "
                            << commonPointTime.getValue().toString());

    // Get the next document, which will be the first document to truncate.
    auto truncatePointRecord = oplogCursor->next();
    invariant(truncatePointRecord);
    auto truncatePointTime = OpTime::parseFromOplogEntry(truncatePointRecord->data.releaseToBson());
    invariant(truncatePointTime.getStatus());

    log() << "Marking to truncate all oplog entries with timestamps greater than or equal to "
          << truncatePointTime.getValue();
    return truncatePointTime.getValue().getTimestamp();
}

boost::optional<BSONObj> RollbackImpl::_findDocumentById(OperationContext* opCtx,
                                                         UUID uuid,
                                                         NamespaceString nss,
                                                         BSONElement id) {
    auto document = _storageInterface->findById(opCtx, {nss.db().toString(), uuid}, id);
    if (document.isOK()) {
        return document.getValue();
    } else if (document.getStatus().code() == ErrorCodes::NoSuchKey) {
        return boost::none;
    } else {
        severe() << "Rollback failed to read document with " << redact(id) << " in namespace "
                 << nss.ns() << " with uuid " << uuid.toString() << causedBy(document.getStatus());
        fassert(50751, document.getStatus());
    }

    MONGO_UNREACHABLE;
}

Status RollbackImpl::_writeRollbackFiles(OperationContext* opCtx) {
    const auto& uuidCatalog = UUIDCatalog::get(opCtx);
    for (auto&& entry : _observerInfo.rollbackDeletedIdsMap) {
        const auto& uuid = entry.first;
        const auto nss = uuidCatalog.lookupNSSByUUID(uuid);
        invariant(!nss.isEmpty(),
                  str::stream() << "The collection with UUID " << uuid
                                << " is unexpectedly missing in the UUIDCatalog");

        if (_isInShutdown()) {
            log() << "Rollback shutting down; not writing rollback file for namespace " << nss.ns()
                  << " with uuid " << uuid;
            continue;
        }

        _writeRollbackFileForNamespace(opCtx, uuid, nss, entry.second);
    }

    if (_isInShutdown()) {
        return {ErrorCodes::ShutdownInProgress, "rollback shutting down"};
    }

    return Status::OK();
}

void RollbackImpl::_writeRollbackFileForNamespace(OperationContext* opCtx,
                                                  UUID uuid,
                                                  NamespaceString nss,
                                                  const SimpleBSONObjUnorderedSet& idSet) {
    Helpers::RemoveSaver removeSaver(kRollbackRemoveSaverType, nss.ns(), kRollbackRemoveSaverWhy);
    log() << "Preparing to write deleted documents to a rollback file for collection " << nss.ns()
          << " with uuid " << uuid.toString() << " to " << removeSaver.file().generic_string();

    // The RemoveSaver will save the data files in a directory structure similar to the following:
    //
    //     rollback
    //     ├── db.collection
    //     │   └── removed.2018-03-20T20-23-01.21.bson
    //     ├── otherdb.othercollection
    //     │   ├── removed.2018-03-20T20-23-01.18.bson
    //     │   └── removed.2018-03-20T20-23-01.19.bson
    //
    // If this is the first data directory created, we save the full directory path in
    // _rollbackStats. Otherwise, we store the longest common prefix of the two directories.
    const auto& newDirectoryPath = removeSaver.root().generic_string();
    if (!_rollbackStats.rollbackDataFileDirectory) {
        _rollbackStats.rollbackDataFileDirectory = newDirectoryPath;
    } else {
        const auto& existingDirectoryPath = *_rollbackStats.rollbackDataFileDirectory;
        const auto& prefixEnd = std::mismatch(newDirectoryPath.begin(),
                                              newDirectoryPath.end(),
                                              existingDirectoryPath.begin(),
                                              existingDirectoryPath.end())
                                    .first;
        _rollbackStats.rollbackDataFileDirectory = std::string(newDirectoryPath.begin(), prefixEnd);
    }

    for (auto&& id : idSet) {
        // StorageInterface::findById() does not respect the collation, but because we are using
        // exact _id fields recorded in the oplog, we can get away with binary string
        // comparisons.
        auto document = _findDocumentById(opCtx, uuid, nss, id.firstElement());
        if (document) {
            fassert(50750, removeSaver.goingToDelete(*document));
        }
    }
    _listener->onRollbackFileWrittenForNamespace(std::move(uuid), std::move(nss));
}

StatusWith<Timestamp> RollbackImpl::_recoverToStableTimestamp(OperationContext* opCtx) {
    if (_isInShutdown()) {
        return Status(ErrorCodes::ShutdownInProgress, "rollback shutting down");
    }
    // Recover to the stable timestamp while holding the global exclusive lock.
    {
        Lock::GlobalWrite globalWrite(opCtx);
        try {
            auto stableTimestampSW = _storageInterface->recoverToStableTimestamp(opCtx);
            if (!stableTimestampSW.isOK()) {
                severe() << "RecoverToStableTimestamp failed. "
                         << causedBy(stableTimestampSW.getStatus());
                return {ErrorCodes::UnrecoverableRollbackError,
                        "Recover to stable timestamp failed."};
            }
            return stableTimestampSW;
        } catch (...) {
            return exceptionToStatus();
        }
    }
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

void RollbackImpl::_resetDropPendingState(OperationContext* opCtx) {
    DropPendingCollectionReaper::get(opCtx)->clearDropPendingState();

    std::vector<std::string> dbNames;
    opCtx->getServiceContext()->getStorageEngine()->listDatabases(&dbNames);
    for (const auto& dbName : dbNames) {
        Lock::DBLock dbLock(opCtx, dbName, MODE_X);
        Database* db = DatabaseHolder::getDatabaseHolder().openDb(opCtx, dbName);
        checkForIdIndexesAndDropPendingCollections(opCtx, db);
    }
}

void RollbackImpl::_summarizeRollback(OperationContext* opCtx) const {
    log() << "Rollback summary:";
    log() << "\tstart time: " << _rollbackStats.startTime;
    log() << "\tend time: " << opCtx->getServiceContext()->getFastClockSource()->now();
    log() << "\tsync source: " << _remoteOplog->hostAndPort().toString();
    log() << "\trollback data file directory: "
          << _rollbackStats.rollbackDataFileDirectory.value_or("none; no files written");
    if (_rollbackStats.rollbackId) {
        log() << "\trollback id: " << *_rollbackStats.rollbackId;
    }
    if (_rollbackStats.lastLocalOptime) {
        log() << "\tlast optime on branch of history rolled back: "
              << *_rollbackStats.lastLocalOptime;
    }
    if (_rollbackStats.commonPoint) {
        log() << "\tcommon point optime: " << *_rollbackStats.commonPoint;
    }
    if (_rollbackStats.lastLocalWallClockTime && _rollbackStats.commonPointWallClockTime) {

        auto lastWall = *_rollbackStats.lastLocalWallClockTime;
        auto commonWall = *_rollbackStats.commonPointWallClockTime;
        unsigned long long diff = durationCount<Seconds>(Milliseconds(lastWall - commonWall));

        log() << "\tlast wall clock time on the branch of history rolled back: " << lastWall;
        log() << "\tcommon point wall clock time: " << commonWall;
        log() << "\tdifference in wall clock times: " << diff << " second(s)";
    }
    if (_rollbackStats.truncateTimestamp) {
        log() << "\ttruncate timestamp: " << *_rollbackStats.truncateTimestamp;
    }
    if (_rollbackStats.stableTimestamp) {
        log() << "\tstable timestamp: " << *_rollbackStats.stableTimestamp;
    }
    log() << "\tshard identity document rolled back: " << std::boolalpha
          << _observerInfo.shardIdentityRolledBack;
    log() << "\tconfig server config version document rolled back: " << std::boolalpha
          << _observerInfo.configServerConfigVersionRolledBack;
    log() << "\taffected sessions: " << (_observerInfo.rollbackSessionIds.empty() ? "none" : "");
    for (const auto& sessionId : _observerInfo.rollbackSessionIds) {
        log() << "\t\t" << sessionId;
    }
    log() << "\taffected namespaces: " << (_observerInfo.rollbackNamespaces.empty() ? "none" : "");
    for (const auto& nss : _observerInfo.rollbackNamespaces) {
        log() << "\t\t" << nss.ns();
    }
    log() << "\tcounts of interesting commands rolled back: "
          << (_observerInfo.rollbackCommandCounts.empty() ? "none" : "");
    for (const auto& entry : _observerInfo.rollbackCommandCounts) {
        log() << "\t\t" << entry.first << ": " << entry.second;
    }
    log() << "\ttotal number of entries rolled back (including no-ops): "
          << _observerInfo.numberOfEntriesObserved;
}

/**
 * This amount, measured in seconds, represents the maximum allowed rollback period.
 * It is calculated by taking the difference of the wall clock times of the oplog entries
 * at the top of the local oplog and at the common point.
 */
class RollbackTimeLimitServerParameter final : public ServerParameter {
    MONGO_DISALLOW_COPYING(RollbackTimeLimitServerParameter);

public:
    static constexpr auto kName = "rollbackTimeLimitSecs"_sd;

    RollbackTimeLimitServerParameter()
        : ServerParameter(ServerParameterSet::getGlobal(), kName.toString(), true, true) {}

    virtual void append(OperationContext* opCtx,
                        BSONObjBuilder& builder,
                        const std::string& name) final {
        builder.append(name,
                       static_cast<long long>(kRollbackTimeLimitHolder.getRollbackTimeLimit()));
    }

    virtual Status set(const BSONElement& newValueElement) final {
        long long newValue;
        if (!newValueElement.coerce(&newValue) || newValue <= 0)
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Invalid value for " << kName << ": "
                                                    << newValueElement
                                                    << ". Must be a positive integer.");
        kRollbackTimeLimitHolder.setRollbackTimeLimit(static_cast<unsigned long long>(newValue));
        return Status::OK();
    }

    virtual Status setFromString(const std::string& str) final {
        long long newValue;
        Status status = parseNumberFromString(str, &newValue);
        if (!status.isOK())
            return status;
        if (newValue <= 0)
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Invalid value for " << kName << ": "
                                                    << newValue
                                                    << ". Must be a positive integer.");

        kRollbackTimeLimitHolder.setRollbackTimeLimit(static_cast<unsigned long long>(newValue));
        return Status::OK();
    }
} rollbackTimeLimitSecs;

constexpr decltype(RollbackTimeLimitServerParameter::kName) RollbackTimeLimitServerParameter::kName;

}  // namespace repl
}  // namespace mongo
