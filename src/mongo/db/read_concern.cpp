/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/read_concern.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

// This is a special flag that allows for testing of snapshot behavior by skipping the replication
// related checks and isolating the storage/query side of snapshotting.
bool testingSnapshotBehaviorInIsolation = false;
ExportedServerParameter<bool, ServerParameterType::kStartupOnly> TestingSnapshotBehaviorInIsolation(
    ServerParameterSet::getGlobal(),
    "testingSnapshotBehaviorInIsolation",
    &testingSnapshotBehaviorInIsolation);

/**
 *  Schedule a write via appendOplogNote command to the primary of this replica set.
 */
Status makeNoopWriteIfNeeded(OperationContext* opCtx, LogicalTime clusterTime) {
    repl::ReplicationCoordinator* const replCoord = repl::ReplicationCoordinator::get(opCtx);
    invariant(replCoord->isReplEnabled());

    auto lastAppliedTime = LogicalTime(replCoord->getMyLastAppliedOpTime().getTimestamp());
    if (clusterTime > lastAppliedTime) {
        auto shardingState = ShardingState::get(opCtx);
        // standalone replica set, so there is no need to advance the OpLog on the primary.
        if (!shardingState->enabled()) {
            return Status::OK();
        }

        auto myShard =
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardingState->getShardName());
        if (!myShard.isOK()) {
            return myShard.getStatus();
        }

        auto swRes = myShard.getValue()->runCommand(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            "admin",
            BSON("appendOplogNote" << 1 << "maxClusterTime" << clusterTime.asTimestamp() << "data"
                                   << BSON("noop write for afterClusterTime read concern" << 1)),
            Shard::RetryPolicy::kIdempotent);
        return swRes.getStatus();
    }
    return Status::OK();
}
}  // namespace

Status waitForReadConcern(OperationContext* opCtx, const repl::ReadConcernArgs& readConcernArgs) {
    repl::ReplicationCoordinator* const replCoord = repl::ReplicationCoordinator::get(opCtx);
    invariant(replCoord);

    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kLinearizableReadConcern) {
        if (replCoord->getReplicationMode() != repl::ReplicationCoordinator::modeReplSet) {
            // For master/slave and standalone nodes, Linearizable Read is not supported.
            return {ErrorCodes::NotAReplicaSet,
                    "node needs to be a replica set member to use read concern"};
        }

        // Replica sets running pv0 do not support linearizable read concern until further testing
        // is completed (SERVER-27025).
        if (!replCoord->isV1ElectionProtocol()) {
            return {
                ErrorCodes::IncompatibleElectionProtocol,
                "Replica sets running protocol version 0 do not support readConcern: linearizable"};
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

    auto afterClusterTime = readConcernArgs.getArgsClusterTime();
    if (afterClusterTime) {
        auto currentTime = LogicalClock::get(opCtx)->getClusterTime();
        if (currentTime < *afterClusterTime) {
            return {ErrorCodes::InvalidOptions,
                    "readConcern afterClusterTime must not be greater than clusterTime value"};
        }
    }

    // Skip waiting for the OpTime when testing snapshot behavior
    if (!testingSnapshotBehaviorInIsolation && !readConcernArgs.isEmpty()) {
        if (replCoord->isReplEnabled() && afterClusterTime) {
            auto status = makeNoopWriteIfNeeded(opCtx, *afterClusterTime);
            if (!status.isOK()) {
                LOG(1) << "failed noop write due to " << status.toString();
            }
        }

        if (replCoord->isReplEnabled() || !afterClusterTime) {
            auto status = replCoord->waitUntilOpTimeForRead(opCtx, readConcernArgs);
            if (!status.isOK()) {
                return status;
            }
        }
    }

    if ((replCoord->getReplicationMode() == repl::ReplicationCoordinator::Mode::modeReplSet ||
         testingSnapshotBehaviorInIsolation) &&
        readConcernArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern) {
        // ReadConcern Majority is not supported in ProtocolVersion 0.
        if (!testingSnapshotBehaviorInIsolation && !replCoord->isV1ElectionProtocol()) {
            return {ErrorCodes::ReadConcernMajorityNotEnabled,
                    str::stream() << "Replica sets running protocol version 0 do not support "
                                     "readConcern: majority"};
        }

        const int debugLevel = serverGlobalParams.clusterRole == ClusterRole::ConfigServer ? 1 : 2;

        LOG(debugLevel) << "Waiting for 'committed' snapshot to be available for reading: "
                        << readConcernArgs;

        Status status = opCtx->recoveryUnit()->setReadFromMajorityCommittedSnapshot();

        // Wait until a snapshot is available.
        while (status == ErrorCodes::ReadConcernMajorityNotAvailableYet) {
            LOG(debugLevel) << "Snapshot not available yet.";
            replCoord->waitUntilSnapshotCommitted(opCtx, SnapshotName::min());
            status = opCtx->recoveryUnit()->setReadFromMajorityCommittedSnapshot();
        }

        if (!status.isOK()) {
            return status;
        }

        LOG(debugLevel) << "Using 'committed' snapshot: " << CurOp::get(opCtx)->opDescription();
    }

    return Status::OK();
}

Status waitForLinearizableReadConcern(OperationContext* opCtx) {

    repl::ReplicationCoordinator* replCoord =
        repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());

    {
        Lock::DBLock lk(opCtx, "local", MODE_IX);
        Lock::CollectionLock lock(opCtx->lockState(), "local.oplog.rs", MODE_IX);

        if (!replCoord->canAcceptWritesForDatabase(opCtx, "admin")) {
            return {ErrorCodes::NotMaster,
                    "No longer primary when waiting for linearizable read concern"};
        }

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {

            WriteUnitOfWork uow(opCtx);
            opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(
                opCtx,
                BSON("msg"
                     << "linearizable read"));
            uow.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
            opCtx, "waitForLinearizableReadConcern", "local.rs.oplog");
    }
    WriteConcernOptions wc = WriteConcernOptions(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, 0);

    repl::OpTime lastOpApplied = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    auto awaitReplResult = replCoord->awaitReplication(opCtx, lastOpApplied, wc);
    if (awaitReplResult.status == ErrorCodes::WriteConcernFailed) {
        return Status(ErrorCodes::LinearizableReadConcernError,
                      "Failed to confirm that read was linearizable.");
    }
    return awaitReplResult.status;
}

}  // namespace mongo
