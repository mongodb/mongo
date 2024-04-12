/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <tuple>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/shim.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/database_name.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_concern_mongod_gen.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeLinearizableReadConcern);

/**
 *  Synchronize writeRequests
 */

class WriteRequestSynchronizer;
const auto getWriteRequestsSynchronizer =
    ServiceContext::declareDecoration<WriteRequestSynchronizer>();

class WriteRequestSynchronizer {
public:
    WriteRequestSynchronizer() = default;

    /**
     * Returns a tuple <false, existingWriteRequest> if it can  find the one that happened after or
     * at clusterTime.
     * Returns a tuple <true, newWriteRequest> otherwise.
     */
    std::tuple<bool, std::shared_ptr<Notification<Status>>> getOrCreateWriteRequest(
        LogicalTime clusterTime) {
        stdx::unique_lock<Latch> lock(_mutex);
        auto lastEl = _writeRequests.rbegin();
        if (lastEl != _writeRequests.rend() && lastEl->first >= clusterTime.asTimestamp()) {
            return std::make_tuple(false, lastEl->second);
        } else {
            auto newWriteRequest = std::make_shared<Notification<Status>>();
            _writeRequests[clusterTime.asTimestamp()] = newWriteRequest;
            return std::make_tuple(true, newWriteRequest);
        }
    }

    /**
     * Erases writeRequest that happened at clusterTime
     */
    void deleteWriteRequest(LogicalTime clusterTime) {
        stdx::unique_lock<Latch> lock(_mutex);
        auto el = _writeRequests.find(clusterTime.asTimestamp());
        invariant(el != _writeRequests.end());
        invariant(el->second);
        el->second.reset();
        _writeRequests.erase(el);
    }

private:
    Mutex _mutex = MONGO_MAKE_LATCH("WriteRequestSynchronizer::_mutex");
    std::map<Timestamp, std::shared_ptr<Notification<Status>>> _writeRequests;
};

/**
 *  Schedule a write via appendOplogNote command to the primary of this replica set.
 */
Status makeNoopWriteIfNeeded(OperationContext* opCtx,
                             LogicalTime clusterTime,
                             const DatabaseName& dbName) {
    repl::ReplicationCoordinator* const replCoord = repl::ReplicationCoordinator::get(opCtx);
    invariant(replCoord->getSettings().isReplSet());

    auto& writeRequests = getWriteRequestsSynchronizer(opCtx->getClient()->getServiceContext());

    auto lastWrittenOpTime = LogicalTime(replCoord->getMyLastWrittenOpTime().getTimestamp());

    // secondaries may lag primary so wait first to avoid unnecessary noop writes.
    if (clusterTime > lastWrittenOpTime && replCoord->getMemberState().secondary()) {
        auto deadline = Date_t::now() + Milliseconds(waitForSecondaryBeforeNoopWriteMS.load());
        auto waitStatus = replCoord->waitUntilOpTimeWrittenUntil(opCtx, clusterTime, deadline);
        lastWrittenOpTime = LogicalTime(replCoord->getMyLastWrittenOpTime().getTimestamp());
        if (!waitStatus.isOK()) {
            LOGV2_DEBUG(20986,
                        1,
                        "Wait for clusterTime: {clusterTime} until deadline: {deadline} failed "
                        "with {waitStatus}",
                        "clusterTime"_attr = clusterTime.toString(),
                        "deadline"_attr = deadline,
                        "waitStatus"_attr = waitStatus.toString());
        }
    }

    auto status = Status::OK();
    int remainingAttempts = 3;
    // this loop addresses the case when two or more threads need to advance the opLog time but the
    // one that waits for the notification gets the later clusterTime, so when the request finishes
    // it needs to be repeated with the later time.
    while (clusterTime > lastWrittenOpTime) {
        // Standalone replica set, so there is no need to advance the OpLog on the primary. The only
        // exception is after a tenant migration because the target time may be from the other
        // replica set and is not guaranteed to be in the oplog of this node's set.
        if (serverGlobalParams.clusterRole.has(ClusterRole::None) &&
            !tenant_migration_access_blocker::hasActiveTenantMigration(opCtx, dbName)) {
            return Status::OK();
        }

        if (!remainingAttempts--) {
            std::stringstream ss;
            ss << "Requested clusterTime " << clusterTime.toString()
               << " is greater than the last primary OpTime: " << lastWrittenOpTime.toString()
               << " no retries left";
            return Status(ErrorCodes::InternalError, ss.str());
        }

        auto myWriteRequest = writeRequests.getOrCreateWriteRequest(clusterTime);
        if (std::get<0>(myWriteRequest)) {  // Its a new request
            try {
                LOGV2_DEBUG(20987,
                            2,
                            "New appendOplogNote request on clusterTime: {clusterTime} remaining "
                            "attempts: {remainingAttempts}",
                            "clusterTime"_attr = clusterTime.toString(),
                            "remainingAttempts"_attr = remainingAttempts);

                auto onRemoteCmdScheduled = [](executor::TaskExecutor::CallbackHandle handle) {
                };
                auto onRemoteCmdComplete = [](executor::TaskExecutor::CallbackHandle handle) {
                };
                auto appendOplogNoteResponse = replCoord->runCmdOnPrimaryAndAwaitResponse(
                    opCtx,
                    DatabaseName::kAdmin,
                    BSON("appendOplogNote"
                         << 1 << "maxClusterTime" << clusterTime.asTimestamp() << "data"
                         << BSON("noop write for afterClusterTime read concern" << 1)
                         << WriteConcernOptions::kWriteConcernField
                         << WriteConcernOptions::Acknowledged),
                    onRemoteCmdScheduled,
                    onRemoteCmdComplete);

                status = getStatusFromCommandResult(appendOplogNoteResponse);
                std::get<1>(myWriteRequest)->set(status);
                writeRequests.deleteWriteRequest(clusterTime);
            } catch (const DBException& ex) {
                status = ex.toStatus();
                // signal the writeRequest to unblock waiters
                std::get<1>(myWriteRequest)->set(status);
                writeRequests.deleteWriteRequest(clusterTime);
            }
        } else {
            LOGV2_DEBUG(20988,
                        2,
                        "Join appendOplogNote request on clusterTime: {clusterTime} remaining "
                        "attempts: {remainingAttempts}",
                        "clusterTime"_attr = clusterTime.toString(),
                        "remainingAttempts"_attr = remainingAttempts);
            try {
                status = std::get<1>(myWriteRequest)->get(opCtx);
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        }
        // If the write status is ok need to wait for the oplog to replicate.
        if (status.isOK()) {
            return status;
        }

        // If the write failed with StaleClusterTime it means that the noop write to the primary was
        // not necessary to bump the clusterTime. It could be a race where the secondary decides to
        // issue the noop write while some writes have already happened on the primary that have
        // bumped the clusterTime beyond the 'clusterTime' the noop write requested.
        if (status == ErrorCodes::StaleClusterTime) {
            LOGV2_DEBUG(54102,
                        2,
                        "appendOplogNote request on clusterTime {clusterTime} failed with "
                        "StaleClusterTime",
                        "clusterTime"_attr = clusterTime.asTimestamp());
            return Status::OK();
        }

        lastWrittenOpTime = LogicalTime(replCoord->getMyLastWrittenOpTime().getTimestamp());
    }
    // This is when the noop write failed but the opLog caught up to clusterTime by replicating.
    if (!status.isOK()) {
        LOGV2_DEBUG(20989,
                    1,
                    "Reached clusterTime {lastWrittenOpTime} but failed noop write due to {error}",
                    "lastWrittenOpTime"_attr = lastWrittenOpTime.toString(),
                    "error"_attr = status.toString());
    }
    return Status::OK();
}

/**
 * Evaluates if it's safe for the command to ignore prepare conflicts.
 */
bool canIgnorePrepareConflicts(OperationContext* opCtx,
                               const repl::ReadConcernArgs& readConcernArgs) {
    if (opCtx->inMultiDocumentTransaction()) {
        return false;
    }

    auto readConcernLevel = readConcernArgs.getLevel();

    // Only these read concern levels are eligible for ignoring prepare conflicts.
    if (readConcernLevel != repl::ReadConcernLevel::kLocalReadConcern &&
        readConcernLevel != repl::ReadConcernLevel::kAvailableReadConcern &&
        readConcernLevel != repl::ReadConcernLevel::kMajorityReadConcern) {
        return false;
    }

    auto afterClusterTime = readConcernArgs.getArgsAfterClusterTime();
    auto atClusterTime = readConcernArgs.getArgsAtClusterTime();

    if (afterClusterTime || atClusterTime) {
        return false;
    }

    return true;
}

void setPrepareConflictBehaviorForReadConcernImpl(OperationContext* opCtx,
                                                  const repl::ReadConcernArgs& readConcernArgs,
                                                  PrepareConflictBehavior prepareConflictBehavior) {
    // DBDirectClient should inherit whether or not to ignore prepare conflicts from its parent.
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    // Enforce prepare conflict behavior if the command is not eligible to ignore prepare conflicts.
    if (!(prepareConflictBehavior == PrepareConflictBehavior::kEnforce ||
          canIgnorePrepareConflicts(opCtx, readConcernArgs))) {
        prepareConflictBehavior = PrepareConflictBehavior::kEnforce;
    }

    shard_role_details::getRecoveryUnit(opCtx)->setPrepareConflictBehavior(prepareConflictBehavior);
}

Status waitForReadConcernImpl(OperationContext* opCtx,
                              const repl::ReadConcernArgs& readConcernArgs,
                              const DatabaseName& dbName,
                              bool allowAfterClusterTime) {
    // If we are in a direct client that's holding a global lock, then this means it is illegal to
    // wait for read concern. This is fine, since the outer operation should have handled waiting
    // for read concern. We don't want to ignore prepare conflicts because reads in transactions
    // should block on prepared transactions.
    if (opCtx->getClient()->isInDirectClient() &&
        shard_role_details::getLocker(opCtx)->isLocked()) {
        return Status::OK();
    }

    repl::ReplicationCoordinator* const replCoord = repl::ReplicationCoordinator::get(opCtx);
    invariant(replCoord);

    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kLinearizableReadConcern) {
        if (!replCoord->getSettings().isReplSet()) {
            // For standalone nodes, Linearizable Read is not supported.
            return {ErrorCodes::NotAReplicaSet,
                    "node needs to be a replica set member to use read concern"};
        }

        if (readConcernArgs.getArgsOpTime()) {
            return {ErrorCodes::FailedToParse,
                    "afterOpTime not compatible with linearizable read concern"};
        }

        if (!replCoord->getMemberState().primary()) {
            return {ErrorCodes::NotWritablePrimary,
                    "cannot satisfy linearizable read concern on non-primary node"};
        }
    }

    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
        if (!replCoord->getSettings().isReplSet()) {
            return {ErrorCodes::NotAReplicaSet,
                    "node needs to be a replica set member to use readConcern: snapshot"};
        }
        if (!opCtx->inMultiDocumentTransaction() && !serverGlobalParams.enableMajorityReadConcern) {
            return {ErrorCodes::ReadConcernMajorityNotEnabled,
                    "read concern level snapshot is not supported when "
                    "enableMajorityReadConcern=false"};
        }
    }

    auto afterClusterTime = readConcernArgs.getArgsAfterClusterTime();
    auto atClusterTime = readConcernArgs.getArgsAtClusterTime();

    if (afterClusterTime) {
        if (!allowAfterClusterTime) {
            return {ErrorCodes::InvalidOptions, "afterClusterTime is not allowed for this command"};
        }
    }

    if (!readConcernArgs.isEmpty()) {
        invariant(!afterClusterTime || !atClusterTime);
        auto targetClusterTime = afterClusterTime ? afterClusterTime : atClusterTime;

        if (targetClusterTime) {
            std::string readConcernName = afterClusterTime ? "afterClusterTime" : "atClusterTime";

            if (!replCoord->getSettings().isReplSet()) {
                return {ErrorCodes::IllegalOperation,
                        str::stream() << "Cannot specify " << readConcernName
                                      << " readConcern without replication enabled"};
            }

            // We must read the member state before obtaining the cluster time. Otherwise, we can
            // run into a race where the cluster time is read as uninitialized, but the member state
            // is set to RECOVERING by another thread before we invariant that the node is in
            // STARTUP or STARTUP2.
            const auto memberState = replCoord->getMemberState();

            const auto currentTime = VectorClock::get(opCtx)->getTime();
            const auto clusterTime = currentTime.clusterTime();
            if (!VectorClock::isValidComponentTime(clusterTime)) {
                // currentTime should only be uninitialized if we are in startup recovery or initial
                // sync.
                invariant(memberState.startup() || memberState.startup2());
                return {ErrorCodes::NotPrimaryOrSecondary,
                        str::stream() << "Current clusterTime is uninitialized, cannot service the "
                                         "requested clusterTime. Requested clusterTime: "
                                      << targetClusterTime->toString()
                                      << "; current clusterTime: " << clusterTime.toString()};
            }
            if (clusterTime < *targetClusterTime) {
                return {ErrorCodes::InvalidOptions,
                        str::stream() << "readConcern " << readConcernName
                                      << " value must not be greater than the current clusterTime. "
                                         "Requested clusterTime: "
                                      << targetClusterTime->toString()
                                      << "; current clusterTime: " << clusterTime.toString()};
            }

            auto status = makeNoopWriteIfNeeded(opCtx, *targetClusterTime, dbName);
            if (!status.isOK()) {
                LOGV2(20990,
                      "Failed noop write",
                      "targetClusterTime"_attr = targetClusterTime,
                      "error"_attr = status);
            }
        }

        if (replCoord->getSettings().isReplSet() || !afterClusterTime) {
            auto status = replCoord->waitUntilOpTimeForRead(opCtx, readConcernArgs);
            if (!status.isOK()) {
                return status;
            }
        }
    }

    auto ru = shard_role_details::getRecoveryUnit(opCtx);
    if (atClusterTime) {
        ru->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                   atClusterTime->asTimestamp());
    } else if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern &&
               replCoord->getSettings().isReplSet() && !opCtx->inMultiDocumentTransaction()) {
        auto opTime = replCoord->getCurrentCommittedSnapshotOpTime();
        uassert(ErrorCodes::SnapshotUnavailable,
                "No committed OpTime for snapshot read",
                !opTime.isNull());
        ru->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, opTime.getTimestamp());
        repl::ReadConcernArgs::get(opCtx).setArgsAtClusterTimeForSnapshot(opTime.getTimestamp());
    } else if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern &&
               replCoord->getSettings().isReplSet()) {
        // This block is not used for kSnapshotReadConcern because snapshots are always speculative;
        // we wait for majority when the transaction commits.
        // It is not used for atClusterTime because waitUntilOpTimeForRead handles waiting for
        // the majority snapshot in that case.

        // Handle speculative majority reads.
        if (readConcernArgs.getMajorityReadMechanism() ==
            repl::ReadConcernArgs::MajorityReadMechanism::kSpeculative) {
            // For speculative majority reads, we utilize the "no overlap" read source as a means of
            // always reading at the minimum of the all-committed and lastApplied timestamps. This
            // allows for safe behavior on both primaries and secondaries, where the behavior of the
            // all-committed and lastApplied timestamps differ significantly.
            ru->setTimestampReadSource(RecoveryUnit::ReadSource::kNoOverlap);
            auto& speculativeReadInfo = repl::SpeculativeMajorityReadInfo::get(opCtx);
            speculativeReadInfo.setIsSpeculativeRead();
            return Status::OK();
        }

        const int debugLevel =
            serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) ? 1 : 2;

        LOGV2_DEBUG(
            20991,
            debugLevel,
            "Waiting for 'committed' snapshot to be available for reading: {readConcernArgs}",
            "readConcernArgs"_attr = readConcernArgs);

        ru->setTimestampReadSource(RecoveryUnit::ReadSource::kMajorityCommitted);
        Status status = ru->majorityCommittedSnapshotAvailable();

        // Wait until a snapshot is available.
        while (status == ErrorCodes::ReadConcernMajorityNotAvailableYet) {
            LOGV2_DEBUG(20992, debugLevel, "Snapshot not available yet.");
            replCoord->waitUntilSnapshotCommitted(opCtx, Timestamp());
            status = ru->majorityCommittedSnapshotAvailable();
        }

        if (!status.isOK()) {
            return status;
        }

        LOGV2_DEBUG(20993,
                    debugLevel,
                    "Using 'committed' snapshot",
                    "operation_description"_attr = CurOp::get(opCtx)->opDescription());
    }

    if (readConcernArgs.waitLastStableRecoveryTimestamp()) {
        uassert(8138101,
                "readConcern level 'snapshot' is required when specifying "
                "$_waitLastStableRecoveryTimestamp",
                readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern);
        uassert(ErrorCodes::InvalidOptions,
                "atClusterTime is required for $_waitLastStableRecoveryTimestamp",
                atClusterTime);
        uassert(
            ErrorCodes::Unauthorized,
            "Unauthorized",
            AuthorizationSession::get(opCtx->getClient())
                ->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(dbName.tenantId()), ActionType::internal));
        auto* const storageEngine = opCtx->getServiceContext()->getStorageEngine();
        Lock::GlobalLock global(opCtx,
                                MODE_IS,
                                Date_t::max(),
                                Lock::InterruptBehavior::kThrow,
                                Lock::GlobalLockSkipOptions{.skipRSTLLock = true});
        auto lastStableRecoveryTimestamp = storageEngine->getLastStableRecoveryTimestamp();
        if (!lastStableRecoveryTimestamp ||
            *lastStableRecoveryTimestamp < atClusterTime->asTimestamp()) {
            // If the lastStableRecoveryTimestamp hasn't passed atClusterTime, we invoke
            // flushAllFiles explicitly here to push it. By default, fsync will run every minute to
            // call flushAllFiles. The lastStableRecoveryTimestamp should already be updated after
            // flushAllFiles return but we add a retry to make sure we wait until the timestamp gets
            // advanced.
            storageEngine->flushAllFiles(opCtx, /*callerHoldsReadLock*/ true);
            while (true) {
                lastStableRecoveryTimestamp = storageEngine->getLastStableRecoveryTimestamp();
                if (lastStableRecoveryTimestamp &&
                    *lastStableRecoveryTimestamp >= atClusterTime->asTimestamp()) {
                    break;
                }

                opCtx->sleepFor(Milliseconds(100));
            }
        }
    }
    return Status::OK();
}

Status waitForLinearizableReadConcernImpl(OperationContext* opCtx,
                                          const Milliseconds readConcernTimeout) {
    // If we are in a direct client that's holding a global lock, then this means this is a
    // sub-operation of the parent. In this case we delegate the wait to the parent.
    if (opCtx->getClient()->isInDirectClient() &&
        shard_role_details::getLocker(opCtx)->isLocked()) {
        return Status::OK();
    }
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangBeforeLinearizableReadConcern, opCtx, "hangBeforeLinearizableReadConcern", [opCtx]() {
            LOGV2(20994,
                  "batch update - hangBeforeLinearizableReadConcern fail point enabled. "
                  "Blocking until fail point is disabled.");
        });

    repl::ReplicationCoordinator* replCoord =
        repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());

    {
        AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
        if (!replCoord->canAcceptWritesForDatabase(opCtx, DatabaseName::kAdmin)) {
            return {ErrorCodes::NotWritablePrimary,
                    "No longer primary when waiting for linearizable read concern"};
        }

        // With linearizable readConcern, read commands may write to the oplog, which is an
        // exception to the rule that writes are not allowed while ignoring prepare conflicts. If we
        // are ignoring prepare conflicts (during a read command), force the prepare conflict
        // behavior to permit writes.
        auto originalBehavior =
            shard_role_details::getRecoveryUnit(opCtx)->getPrepareConflictBehavior();
        if (originalBehavior == PrepareConflictBehavior::kIgnoreConflicts) {
            shard_role_details::getRecoveryUnit(opCtx)->setPrepareConflictBehavior(
                PrepareConflictBehavior::kIgnoreConflictsAllowWrites);
        }

        writeConflictRetry(
            opCtx, "waitForLinearizableReadConcern", NamespaceString::kRsOplogNamespace, [&opCtx] {
                WriteUnitOfWork uow(opCtx);
                opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(
                    opCtx,
                    BSON("msg"
                         << "linearizable read"));
                uow.commit();
            });
    }
    WriteConcernOptions wc = WriteConcernOptions{
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, readConcernTimeout};
    repl::OpTime lastOpApplied = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    auto awaitReplResult = replCoord->awaitReplication(opCtx, lastOpApplied, wc);

    if (awaitReplResult.status == ErrorCodes::WriteConcernFailed) {
        return Status(ErrorCodes::LinearizableReadConcernError,
                      "Failed to confirm that read was linearizable.");
    }
    return awaitReplResult.status;
}

Status waitForSpeculativeMajorityReadConcernImpl(
    OperationContext* opCtx, repl::SpeculativeMajorityReadInfo speculativeReadInfo) {
    invariant(speculativeReadInfo.isSpeculativeRead());

    // If we are in a direct client that's holding a global lock, then this means this is a
    // sub-operation of the parent. In this case we delegate the wait to the parent.
    if (opCtx->getClient()->isInDirectClient() &&
        shard_role_details::getLocker(opCtx)->isLocked()) {
        return Status::OK();
    }

    // Select the timestamp to wait on. A command may have selected a specific timestamp to wait on.
    // If not, then we use the timestamp selected by the read source.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    Timestamp waitTs;
    auto speculativeReadTimestamp = speculativeReadInfo.getSpeculativeReadTimestamp();
    if (speculativeReadTimestamp) {
        waitTs = *speculativeReadTimestamp;
    } else {
        // Speculative majority reads are required to use the 'kNoOverlap' read source.
        invariant(shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource() ==
                  RecoveryUnit::ReadSource::kNoOverlap);

        // Storage engine operations require at least Global IS.
        Lock::GlobalLock lk(opCtx, MODE_IS);
        boost::optional<Timestamp> readTs =
            shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp(opCtx);
        invariant(readTs);
        waitTs = *readTs;
    }

    // Block to make sure returned data is majority committed.
    LOGV2_DEBUG(20995,
                1,
                "Servicing speculative majority read, waiting for timestamp {waitTs} to become "
                "committed, current commit point: {replCoord_getLastCommittedOpTime}",
                "waitTs"_attr = waitTs,
                "replCoord_getLastCommittedOpTime"_attr = replCoord->getLastCommittedOpTime());

    if (!opCtx->hasDeadline()) {
        // This hard-coded value represents the maximum time we are willing to wait for a timestamp
        // to majority commit when doing a speculative majority read if no maxTimeMS value has been
        // set for the command. We make this value rather conservative. This exists primarily to
        // address the fact that getMore commands do not respect maxTimeMS properly. In this case,
        // we still want speculative majority reads to time out after some period if a timestamp
        // cannot majority commit.
        auto timeout = Seconds(15);
        opCtx->setDeadlineAfterNowBy(timeout, ErrorCodes::MaxTimeMSExpired);
    }
    Timer t;
    auto waitStatus = replCoord->awaitTimestampCommitted(opCtx, waitTs);
    if (waitStatus.isOK()) {
        LOGV2_DEBUG(20996,
                    1,
                    "Timestamp {waitTs} became majority committed, waited {t_millis}ms for "
                    "speculative majority read to be satisfied.",
                    "waitTs"_attr = waitTs,
                    "t_millis"_attr = t.millis());
    }
    return waitStatus;
}

auto setPrepareConflictBehaviorForReadConcernRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    setPrepareConflictBehaviorForReadConcern, setPrepareConflictBehaviorForReadConcernImpl);
auto waitForReadConcernRegistration =
    MONGO_WEAK_FUNCTION_REGISTRATION(waitForReadConcern, waitForReadConcernImpl);
auto waitForLinearizableReadConcernRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    waitForLinearizableReadConcern, waitForLinearizableReadConcernImpl);
auto waitForSpeculativeMajorityReadConcernRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    waitForSpeculativeMajorityReadConcern, waitForSpeculativeMajorityReadConcernImpl);
}  // namespace

}  // namespace mongo
