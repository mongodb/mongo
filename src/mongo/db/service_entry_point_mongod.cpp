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

#include "mongo/platform/basic.h"

#include "mongo/db/service_entry_point_mongod.h"

#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/curop.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/s/implicit_create_collection.h"
#include "mongo/db/s/scoped_operation_completion_sharding_actions.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_config_optime_gossip.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_entry_point_common.h"
#include "mongo/logger/redaction.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/s/cannot_implicitly_create_collection_info.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"

namespace mongo {

constexpr auto kLastCommittedOpTimeFieldName = "lastCommittedOpTime"_sd;

class ServiceEntryPointMongod::Hooks final : public ServiceEntryPointCommon::Hooks {
public:
    bool lockedForWriting() const override {
        return mongo::lockedForWriting();
    }

    void setPrepareConflictBehaviorForReadConcern(
        OperationContext* opCtx, const CommandInvocation* invocation) const override {
        const auto prepareConflictBehavior = invocation->canIgnorePrepareConflicts()
            ? PrepareConflictBehavior::kIgnoreConflicts
            : PrepareConflictBehavior::kEnforce;
        mongo::setPrepareConflictBehaviorForReadConcern(
            opCtx, repl::ReadConcernArgs::get(opCtx), prepareConflictBehavior);
    }

    void waitForReadConcern(OperationContext* opCtx,
                            const CommandInvocation* invocation,
                            const OpMsgRequest& request) const override {
        Status rcStatus = mongo::waitForReadConcern(
            opCtx, repl::ReadConcernArgs::get(opCtx), invocation->allowsAfterClusterTime());

        if (!rcStatus.isOK()) {
            if (ErrorCodes::isExceededTimeLimitError(rcStatus.code())) {
                const int debugLevel =
                    serverGlobalParams.clusterRole == ClusterRole::ConfigServer ? 0 : 2;
                LOG(debugLevel) << "Command on database " << request.getDatabase()
                                << " timed out waiting for read concern to be satisfied. Command: "
                                << redact(ServiceEntryPointCommon::getRedactedCopyForLogging(
                                       invocation->definition(), request.body));
            }

            uassertStatusOK(rcStatus);
        }
    }

    void waitForSpeculativeMajorityReadConcern(OperationContext* opCtx) const override {
        auto speculativeReadInfo = repl::SpeculativeMajorityReadInfo::get(opCtx);
        if (!speculativeReadInfo.isSpeculativeRead()) {
            return;
        }
        uassertStatusOK(mongo::waitForSpeculativeMajorityReadConcern(opCtx, speculativeReadInfo));
    }


    void waitForWriteConcern(OperationContext* opCtx,
                             const CommandInvocation* invocation,
                             const repl::OpTime& lastOpBeforeRun,
                             BSONObjBuilder& commandResponseBuilder) const override {
        auto lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

        auto waitForWriteConcernAndAppendStatus = [&]() {
            WriteConcernResult res;
            auto waitForWCStatus =
                mongo::waitForWriteConcern(opCtx, lastOpAfterRun, opCtx->getWriteConcern(), &res);

            CommandHelpers::appendCommandWCStatus(commandResponseBuilder, waitForWCStatus, res);
        };

        if (lastOpAfterRun != lastOpBeforeRun) {
            invariant(lastOpAfterRun > lastOpBeforeRun);
            waitForWriteConcernAndAppendStatus();
            return;
        }

        // Ensures that if we tried to do a write, we wait for write concern, even if that write was
        // a noop.
        //
        // Transactions do not stash their lockers on commit and abort, so after commit and abort,
        // wasGlobalLockTakenForWrite will return whether any statement in the transaction as a
        // whole acquired the global write lock.
        //
        // Speculative majority semantics dictate that "abortTransaction" should not wait for write
        // concern on operations the transaction observed. As a result, "abortTransaction" only ever
        // waits on an oplog entry it wrote (and has already set lastOp to) or previous writes on
        // the same client.
        if (opCtx->lockState()->wasGlobalLockTakenForWrite()) {
            if (invocation->definition()->getName() != "abortTransaction") {
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
                lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            }
            waitForWriteConcernAndAppendStatus();
            return;
        }

        // Waits for write concern if we tried to explicitly set the lastOp forward but lastOp was
        // already up to date. We still want to wait for write concern on the lastOp. This is
        // primarily to make sure back to back retryable write retries still wait for write concern.
        //
        // WARNING: Retryable writes that expect to wait for write concern on retries must ensure
        // this is entered by calling setLastOp() or setLastOpToSystemLastOpTime().
        if (repl::ReplClientInfo::forClient(opCtx->getClient())
                .lastOpWasSetExplicitlyByClientForCurrentOperation(opCtx)) {
            waitForWriteConcernAndAppendStatus();
            return;
        }

        // If no write was attempted and the client's lastOp was not changed by the current network
        // operation then we skip waiting for writeConcern.
    }

    void waitForLinearizableReadConcern(OperationContext* opCtx) const override {
        // When a linearizable read command is passed in, check to make sure we're reading
        // from the primary.
        if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
            repl::ReadConcernLevel::kLinearizableReadConcern) {
            uassertStatusOK(mongo::waitForLinearizableReadConcern(opCtx, 0));
        }
    }

    void uassertCommandDoesNotSpecifyWriteConcern(const BSONObj& cmd) const override {
        if (commandSpecifiesWriteConcern(cmd)) {
            uasserted(ErrorCodes::InvalidOptions, "Command does not support writeConcern");
        }
    }

    void attachCurOpErrInfo(OperationContext* opCtx, const BSONObj& replyObj) const override {
        CurOp::get(opCtx)->debug().errInfo = getStatusFromCommandResult(replyObj);
    }

    void handleException(const DBException& e, OperationContext* opCtx) const override {
        // If we got a stale config, wait in case the operation is stuck in a critical section
        if (auto sce = e.extraInfo<StaleConfigInfo>()) {
            if (!opCtx->getClient()->isInDirectClient()) {
                // We already have the StaleConfig exception, so just swallow any errors due to
                // refresh
                onShardVersionMismatchNoExcept(opCtx, sce->getNss(), sce->getVersionReceived())
                    .ignore();
            }
        } else if (auto sce = e.extraInfo<StaleDbRoutingVersion>()) {
            if (!opCtx->getClient()->isInDirectClient()) {
                onDbVersionMismatchNoExcept(
                    opCtx, sce->getDb(), sce->getVersionReceived(), sce->getVersionWanted())
                    .ignore();
            }
        } else if (auto cannotImplicitCreateCollInfo =
                       e.extraInfo<CannotImplicitlyCreateCollectionInfo>()) {
            if (ShardingState::get(opCtx)->enabled()) {
                onCannotImplicitlyCreateCollection(opCtx, cannotImplicitCreateCollInfo->getNss())
                    .ignore();
            }
        }
    }

    // Called from the error contexts where request may not be available.
    void appendReplyMetadataOnError(OperationContext* opCtx,
                                    BSONObjBuilder* metadataBob) const override {
        const bool isConfig = serverGlobalParams.clusterRole == ClusterRole::ConfigServer;
        if (ShardingState::get(opCtx)->enabled() || isConfig) {
            auto lastCommittedOpTime =
                repl::ReplicationCoordinator::get(opCtx)->getLastCommittedOpTime();
            metadataBob->append(kLastCommittedOpTimeFieldName, lastCommittedOpTime.getTimestamp());
        }
    }

    void appendReplyMetadata(OperationContext* opCtx,
                             const OpMsgRequest& request,
                             BSONObjBuilder* metadataBob) const override {
        const bool isShardingAware = ShardingState::get(opCtx)->enabled();
        const bool isConfig = serverGlobalParams.clusterRole == ClusterRole::ConfigServer;
        auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
        const bool isReplSet =
            replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;

        if (isReplSet) {
            // Attach our own last opTime.
            repl::OpTime lastOpTimeFromClient =
                repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            replCoord->prepareReplMetadata(request.body, lastOpTimeFromClient, metadataBob);
            // For commands from mongos, append some info to help getLastError(w) work.
            // TODO: refactor out of here as part of SERVER-18236
            if (isShardingAware || isConfig) {
                rpc::ShardingMetadata(lastOpTimeFromClient, replCoord->getElectionId())
                    .writeToMetadata(metadataBob)
                    .transitional_ignore();
            }

            if (isShardingAware || isConfig) {
                auto lastCommittedOpTime = replCoord->getLastCommittedOpTime();
                metadataBob->append(kLastCommittedOpTimeFieldName,
                                    lastCommittedOpTime.getTimestamp());
            }
        }

        // If we're a shard other than the config shard, attach the last configOpTime we know about.
        if (isShardingAware && !isConfig) {
            auto opTime = Grid::get(opCtx)->configOpTime();
            rpc::ConfigServerMetadata(opTime).writeToMetadata(metadataBob);
        }
    }

    void advanceConfigOpTimeFromRequestMetadata(OperationContext* opCtx) const override {
        // Handle config optime information that may have been sent along with the command.
        rpc::advanceConfigOpTimeFromRequestMetadata(opCtx);
    }

    std::unique_ptr<PolymorphicScoped> scopedOperationCompletionShardingActions(
        OperationContext* opCtx) const override {
        return std::make_unique<ScopedOperationCompletionShardingActions>(opCtx);
    }
};

DbResponse ServiceEntryPointMongod::handleRequest(OperationContext* opCtx, const Message& m) {
    return ServiceEntryPointCommon::handleRequest(opCtx, m, Hooks{});
}

}  // namespace mongo
