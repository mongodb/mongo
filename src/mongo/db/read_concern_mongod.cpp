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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/db/read_concern.h"
#include "mongo/base/status.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_concern_mongod_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/s/grid.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/log.h"

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
        stdx::unique_lock<stdx::mutex> lock(_mutex);
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
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        auto el = _writeRequests.find(clusterTime.asTimestamp());
        invariant(el != _writeRequests.end());
        invariant(el->second);
        el->second.reset();
        _writeRequests.erase(el);
    }

private:
    stdx::mutex _mutex;
    std::map<Timestamp, std::shared_ptr<Notification<Status>>> _writeRequests;
};

/**
 *  Schedule a write via appendOplogNote command to the primary of this replica set.
 */
Status makeNoopWriteIfNeeded(OperationContext* opCtx, LogicalTime clusterTime) {
    repl::ReplicationCoordinator* const replCoord = repl::ReplicationCoordinator::get(opCtx);
    invariant(replCoord->isReplEnabled());

    auto& writeRequests = getWriteRequestsSynchronizer(opCtx->getClient()->getServiceContext());

    auto lastAppliedOpTime = LogicalTime(replCoord->getMyLastAppliedOpTime().getTimestamp());

    // secondaries may lag primary so wait first to avoid unnecessary noop writes.
    if (clusterTime > lastAppliedOpTime && replCoord->getMemberState().secondary()) {
        auto deadline = Date_t::now() + Milliseconds(waitForSecondaryBeforeNoopWriteMS.load());
        auto readConcernArgs =
            repl::ReadConcernArgs(clusterTime, repl::ReadConcernLevel::kLocalReadConcern);
        auto waitStatus = replCoord->waitUntilOpTimeForReadUntil(opCtx, readConcernArgs, deadline);
        lastAppliedOpTime = LogicalTime(replCoord->getMyLastAppliedOpTime().getTimestamp());
        if (!waitStatus.isOK()) {
            LOG(1) << "Wait for clusterTime: " << clusterTime.toString()
                   << " until deadline: " << deadline << " failed with " << waitStatus.toString();
        }
    }

    auto status = Status::OK();
    int remainingAttempts = 3;
    // this loop addresses the case when two or more threads need to advance the opLog time but the
    // one that waits for the notification gets the later clusterTime, so when the request finishes
    // it needs to be repeated with the later time.
    while (clusterTime > lastAppliedOpTime) {
        // standalone replica set, so there is no need to advance the OpLog on the primary.
        if (serverGlobalParams.clusterRole == ClusterRole::None) {
            return Status::OK();
        }

        bool isConfig = (serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

        if (!isConfig && !ShardingState::get(opCtx)->enabled()) {
            return {ErrorCodes::ShardingStateNotInitialized,
                    "Failed noop write because sharding state has not been initialized"};
        }

        auto myShard = isConfig ? Grid::get(opCtx)->shardRegistry()->getConfigShard()
                                : Grid::get(opCtx)->shardRegistry()->getShard(
                                      opCtx, ShardingState::get(opCtx)->shardId());

        if (!myShard.isOK()) {
            return myShard.getStatus();
        }

        if (!remainingAttempts--) {
            std::stringstream ss;
            ss << "Requested clusterTime " << clusterTime.toString()
               << " is greater than the last primary OpTime: " << lastAppliedOpTime.toString()
               << " no retries left";
            return Status(ErrorCodes::InternalError, ss.str());
        }

        auto myWriteRequest = writeRequests.getOrCreateWriteRequest(clusterTime);
        if (std::get<0>(myWriteRequest)) {  // Its a new request
            try {
                LOG(2) << "New appendOplogNote request on clusterTime: " << clusterTime.toString()
                       << " remaining attempts: " << remainingAttempts;
                auto swRes = myShard.getValue()->runCommand(
                    opCtx,
                    ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                    "admin",
                    BSON("appendOplogNote" << 1 << "maxClusterTime" << clusterTime.asTimestamp()
                                           << "data"
                                           << BSON("noop write for afterClusterTime read concern"
                                                   << 1)),
                    Shard::RetryPolicy::kIdempotent);
                status = swRes.getStatus();
                std::get<1>(myWriteRequest)->set(status);
                writeRequests.deleteWriteRequest(clusterTime);
            } catch (const DBException& ex) {
                status = ex.toStatus();
                // signal the writeRequest to unblock waiters
                std::get<1>(myWriteRequest)->set(status);
                writeRequests.deleteWriteRequest(clusterTime);
            }
        } else {
            LOG(2) << "Join appendOplogNote request on clusterTime: " << clusterTime.toString()
                   << " remaining attempts: " << remainingAttempts;
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
        lastAppliedOpTime = LogicalTime(replCoord->getMyLastAppliedOpTime().getTimestamp());
    }
    // This is when the noop write failed but the opLog caught up to clusterTime by replicating.
    if (!status.isOK()) {
        LOG(1) << "Reached clusterTime " << lastAppliedOpTime.toString()
               << " but failed noop write due to " << status.toString();
    }
    return Status::OK();
}

/**
 * Returns the PrepareConflictBehavior that a command should use given the requested behavior and
 * readConcern options.
 */
PrepareConflictBehavior getPrepareBehaviorForReadConcern(
    PrepareConflictBehavior requestedBehavior,
    repl::ReadConcernLevel readConcernLevel,
    boost::optional<LogicalTime> afterClusterTime,
    boost::optional<LogicalTime> atClusterTime) {

    // Only these read concern levels are eligible for ignoring prepare conflicts.
    if (readConcernLevel != repl::ReadConcernLevel::kLocalReadConcern &&
        readConcernLevel != repl::ReadConcernLevel::kAvailableReadConcern &&
        readConcernLevel != repl::ReadConcernLevel::kMajorityReadConcern) {
        return PrepareConflictBehavior::kEnforce;
    }

    if (afterClusterTime || atClusterTime) {
        return PrepareConflictBehavior::kEnforce;
    }

    return requestedBehavior;
}
}  // namespace

MONGO_REGISTER_SHIM(waitForReadConcern)
(OperationContext* opCtx,
 const repl::ReadConcernArgs& readConcernArgs,
 bool allowAfterClusterTime,
 PrepareConflictBehavior prepareConflictBehavior)
    ->Status {
    // If we are in a direct client within a transaction, then we may be holding locks, so it is
    // illegal to wait for read concern. This is fine, since the outer operation should have handled
    // waiting for read concern. We don't want to ignore prepare conflicts because snapshot reads
    // should block on prepared transactions.
    if (opCtx->getClient()->isInDirectClient() &&
        readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
        return Status::OK();
    }

    repl::ReplicationCoordinator* const replCoord = repl::ReplicationCoordinator::get(opCtx);
    invariant(replCoord);

    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kLinearizableReadConcern) {
        if (replCoord->getReplicationMode() != repl::ReplicationCoordinator::modeReplSet) {
            // For standalone nodes, Linearizable Read is not supported.
            return {ErrorCodes::NotAReplicaSet,
                    "node needs to be a replica set member to use read concern"};
        }

        if (readConcernArgs.getArgsOpTime()) {
            return {ErrorCodes::FailedToParse,
                    "afterOpTime not compatible with linearizable read concern"};
        }

        if (!replCoord->getMemberState().primary()) {
            return {ErrorCodes::NotMaster,
                    "cannot satisfy linearizable read concern on non-primary node"};
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

            if (!replCoord->isReplEnabled()) {
                return {ErrorCodes::IllegalOperation,
                        str::stream() << "Cannot specify " << readConcernName
                                      << " readConcern without replication enabled"};
            }

            auto currentTime = LogicalClock::get(opCtx)->getClusterTime();
            if (currentTime < *targetClusterTime) {
                return {ErrorCodes::InvalidOptions,
                        str::stream() << "readConcern " << readConcernName
                                      << " value must not be greater than the current clusterTime. "
                                         "Requested clusterTime: "
                                      << targetClusterTime->toString()
                                      << "; current clusterTime: "
                                      << currentTime.toString()};
            }

            auto status = makeNoopWriteIfNeeded(opCtx, *targetClusterTime);
            if (!status.isOK()) {
                LOG(0) << "Failed noop write at clusterTime: " << targetClusterTime->toString()
                       << " due to " << status.toString();
            }
        }

        if (replCoord->isReplEnabled() || !afterClusterTime) {
            auto status = replCoord->waitUntilOpTimeForRead(opCtx, readConcernArgs);
            if (!status.isOK()) {
                return status;
            }
        }
    }

    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
        if (replCoord->getReplicationMode() != repl::ReplicationCoordinator::modeReplSet) {
            return {ErrorCodes::NotAReplicaSet,
                    "node needs to be a replica set member to use readConcern: snapshot"};
        }
    }

    if (atClusterTime) {
        opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                      atClusterTime->asTimestamp());
    } else if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern &&
               replCoord->getReplicationMode() == repl::ReplicationCoordinator::Mode::modeReplSet) {
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
            opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoOverlap);
            auto& speculativeReadInfo = repl::SpeculativeMajorityReadInfo::get(opCtx);
            speculativeReadInfo.setIsSpeculativeRead();
            return Status::OK();
        }

        const int debugLevel = serverGlobalParams.clusterRole == ClusterRole::ConfigServer ? 1 : 2;

        LOG(debugLevel) << "Waiting for 'committed' snapshot to be available for reading: "
                        << readConcernArgs;

        opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kMajorityCommitted);
        Status status = opCtx->recoveryUnit()->obtainMajorityCommittedSnapshot();

        // Wait until a snapshot is available.
        while (status == ErrorCodes::ReadConcernMajorityNotAvailableYet) {
            LOG(debugLevel) << "Snapshot not available yet.";
            replCoord->waitUntilSnapshotCommitted(opCtx, Timestamp());
            status = opCtx->recoveryUnit()->obtainMajorityCommittedSnapshot();
        }

        if (!status.isOK()) {
            return status;
        }

        LOG(debugLevel) << "Using 'committed' snapshot: " << CurOp::get(opCtx)->opDescription()
                        << " with readTs: " << opCtx->recoveryUnit()->getPointInTimeReadTimestamp();
    }

    // DBDirectClient should inherit whether or not to ignore prepare conflicts from its parent.
    if (!opCtx->getClient()->isInDirectClient()) {
        // Set whether this command should ignore prepare conflicts or not.
        const auto behavior = getPrepareBehaviorForReadConcern(
            prepareConflictBehavior, readConcernArgs.getLevel(), afterClusterTime, atClusterTime);
        opCtx->recoveryUnit()->setPrepareConflictBehavior(behavior);
    }

    return Status::OK();
}

MONGO_REGISTER_SHIM(waitForLinearizableReadConcern)
(OperationContext* opCtx, const int readConcernTimeout)->Status {
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangBeforeLinearizableReadConcern, opCtx, "hangBeforeLinearizableReadConcern", [opCtx]() {
            log() << "batch update - hangBeforeLinearizableReadConcern fail point enabled. "
                     "Blocking until fail point is disabled.";
        });

    repl::ReplicationCoordinator* replCoord =
        repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());

    {
        Lock::DBLock lk(opCtx, "local", MODE_IX);
        Lock::CollectionLock lock(opCtx, NamespaceString("local.oplog.rs"), MODE_IX);

        if (!replCoord->canAcceptWritesForDatabase(opCtx, "admin")) {
            return {ErrorCodes::NotMaster,
                    "No longer primary when waiting for linearizable read concern"};
        }

        // With linearizable readConcern, read commands may write to the oplog, which is an
        // exception to the rule that writes are not allowed while ignoring prepare conflicts. If we
        // are ignoring prepare conflicts (during a read command), force the prepare conflict
        // behavior to permit writes.
        auto originalBehavior = opCtx->recoveryUnit()->getPrepareConflictBehavior();
        if (originalBehavior == PrepareConflictBehavior::kIgnoreConflicts) {
            opCtx->recoveryUnit()->setPrepareConflictBehavior(
                PrepareConflictBehavior::kIgnoreConflictsAllowWrites);
        }

        writeConflictRetry(
            opCtx,
            "waitForLinearizableReadConcern",
            NamespaceString::kRsOplogNamespace.ns(),
            [&opCtx] {
                WriteUnitOfWork uow(opCtx);
                opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(
                    opCtx,
                    BSON("msg"
                         << "linearizable read"));
                uow.commit();
            });
    }
    WriteConcernOptions wc = WriteConcernOptions(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, readConcernTimeout);

    repl::OpTime lastOpApplied = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    auto awaitReplResult = replCoord->awaitReplication(opCtx, lastOpApplied, wc);

    if (awaitReplResult.status == ErrorCodes::WriteConcernFailed) {
        return Status(ErrorCodes::LinearizableReadConcernError,
                      "Failed to confirm that read was linearizable.");
    }
    return awaitReplResult.status;
}

MONGO_REGISTER_SHIM(waitForSpeculativeMajorityReadConcern)
(OperationContext* opCtx, repl::SpeculativeMajorityReadInfo speculativeReadInfo)->Status {
    invariant(speculativeReadInfo.isSpeculativeRead());

    // Select the timestamp to wait on. A command may have selected a specific timestamp to wait on.
    // If not, then we use the timestamp selected by the read source.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    Timestamp waitTs;
    auto speculativeReadTimestamp = speculativeReadInfo.getSpeculativeReadTimestamp();
    if (speculativeReadTimestamp) {
        waitTs = *speculativeReadTimestamp;
    } else {
        // Speculative majority reads are required to use the 'kNoOverlap' read source.
        invariant(opCtx->recoveryUnit()->getTimestampReadSource() ==
                  RecoveryUnit::ReadSource::kNoOverlap);
        boost::optional<Timestamp> readTs = opCtx->recoveryUnit()->getPointInTimeReadTimestamp();
        invariant(readTs);
        waitTs = *readTs;
    }

    // Block to make sure returned data is majority committed.
    LOG(1) << "Servicing speculative majority read, waiting for timestamp " << waitTs
           << " to become committed, current commit point: " << replCoord->getLastCommittedOpTime();

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
        LOG(1) << "Timestamp " << waitTs << " became majority committed, waited " << t.millis()
               << "ms for speculative majority read to be satisfied.";
    }
    return waitStatus;
}


}  // namespace mongo
