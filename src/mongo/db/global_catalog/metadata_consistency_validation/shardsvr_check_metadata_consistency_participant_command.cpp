// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_types_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/testing_proctor.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardsvrCheckMetadataConsistencyParticipantCommand final
    : public TypedCommand<ShardsvrCheckMetadataConsistencyParticipantCommand> {
public:
    using Request = ShardsvrCheckMetadataConsistencyParticipant;
    using Response = CursorInitialReply;

    bool adminOnly() const override {
        return false;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            const auto nss = ns();
            const auto& primaryShardId = request().getPrimaryShardId();
            const auto checkRangeDeletionIndexes =
                request().getCommonFields().getCheckRangeDeletionIndexes();
            const auto checkIndexes = request().getCommonFields().getCheckIndexes();

            _invokeCommandOnSecondaries(opCtx, primaryShardId);

            auto [inconsistencies, _] =
                metadata_consistency_util::runCheckMetadataConsistencyOnParticipant(
                    opCtx,
                    nss,
                    primaryShardId,
                    checkRangeDeletionIndexes,
                    checkIndexes,
                    metadata_consistency_util::RSNodeMode::kPrimary);

            // Build a streaming executor that merges the secondaries' cursors (or null if there are
            // none). The locally-computed inconsistencies are prepended by
            // 'createInitialCursorReplyMongod' so they are emitted before the streamed results.
            auto secondaryCursorsExec = _mergeSecondaryCursors(opCtx);

            return metadata_consistency_util::createInitialCursorReplyMongod(
                opCtx,
                nss,
                std::move(inconsistencies),
                request().getCursor(),
                request().toBSON(),
                std::move(secondaryCursorsExec));
        }

    private:
        void _invokeCommandOnSecondaries(OperationContext* opCtx,
                                         const mongo::ShardId& primaryShardId) {
            if (sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ==
                AuthoritativeMetadataAccessLevelEnum::kNone) {
                return;
            }
            if (!TestingProctor::instance().isEnabled()) {
                return;
            }

            const auto secondaryCheckMode = request().getCommonFields().get_checkSecondariesMode();
            // TODO (SERVER-98118): this should be a uassert or even a tassert.
            if (!secondaryCheckMode) {
                LOGV2_WARNING(
                    13165400,
                    "Called _shardsvrCheckMetadataConsistencyParticipant with empty "
                    "secondaryCheckMode, skipping checkMetadataConsistency on secondary nodes");
                return;
            }

            if (secondaryCheckMode ==
                CheckMetadataConsistencySecondaryModeEnum::kNoSecondaryCheck) {
                return;
            }

            const auto& nss = ns();
            const auto* const replCoord = repl::ReplicationCoordinator::get(opCtx);
            const auto replSetConfig = replCoord->getConfig();
            const auto& members = replSetConfig.members();

            ShardsvrCheckMetadataConsistencySecondaryParticipant command{nss};
            command.setCommonFields(request().getCommonFields());
            command.setPrimaryShardId(request().getPrimaryShardId());
            command.setCursor(request().getCursor());

            // Secondaries may be lagged, so we need to make sure they see a consistent metadata
            // view.
            // With kCheckAtPrimaryTimestamp, we achieve this by sending a readConcern with
            // afterClusterTime set at majority commit time.
            // With kCheckAtSecondaryTimestamp, we don't set any readConcern. That signals the
            // secondary to perform checkMetadataConsistency assuming it may be lagged.
            if (*secondaryCheckMode ==
                CheckMetadataConsistencySecondaryModeEnum::kCheckAtPrimaryTimestamp) {
                const auto snapshotTimestamp = replCoord->getCurrentCommittedSnapshotOpTime();
                if (snapshotTimestamp.isNull()) {
                    LOGV2_WARNING(13017701,
                                  "The majority committed timestamp is null. Skipping calling "
                                  "checkMetadataConsistency on secondary nodes");
                    return;
                }
                repl::ReadConcernArgs readConcern{
                    LogicalTime{snapshotTimestamp.getTimestamp()} /* afterClusterTime */,
                    repl::ReadConcernLevel::kLocalReadConcern};
                command.setReadConcern(std::move(readConcern));
            }

            _executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
            const auto commandBSON = command.toBSON();

            for (const auto& member : members) {
                if (member.getHostAndPort() == replCoord->getMyHostAndPort()) {
                    continue;
                }
                if (member.isArbiter()) {
                    continue;
                }

                _secondaryJobs.emplace_back();
                auto& [hostAndPort, handle, response] = _secondaryJobs.back();
                response = std::make_shared<executor::RemoteCommandResponse>();

                executor::RemoteCommandRequest request{
                    member.getHostAndPort(), nss.dbName(), commandBSON, opCtx};

                auto statusWithHandle = _executor->scheduleRemoteCommand(
                    request,
                    [response](const executor::TaskExecutor::RemoteCommandCallbackArgs& cbk) {
                        *response = cbk.response;
                    });

                if (!statusWithHandle.isOK()) {
                    // The executor is shutting down. Leave this entry's handle invalid so the
                    // caller skips it; no callback was scheduled, so 'response' stays unset.
                    LOGV2_WARNING(
                        12922002,
                        "Failed to schedule command on member (the node is likely shutting down)",
                        "hostAndPort"_attr = member.getHostAndPort(),
                        "error"_attr = statusWithHandle.getStatus());
                    _secondaryJobs.pop_back();
                    continue;
                }

                hostAndPort = member.getHostAndPort();
                handle = statusWithHandle.getValue();
            }
        }

        // Waits for the commands scheduled on the secondaries, collects their cursors and returns a
        // streaming plan executor that merges them. Returns null if there are no secondary cursors
        // to merge.
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> _mergeSecondaryCursors(
            OperationContext* opCtx) {
            if (_secondaryJobs.empty()) {
                return nullptr;
            }

            invariant(_executor);

            const auto& nss = ns();
            const auto& shardId = ShardingState::get(opCtx)->shardId();

            std::vector<RemoteCursor> remoteCursors;
            remoteCursors.reserve(_secondaryJobs.size());

            for (const auto& [hostAndPort, handle, response] : _secondaryJobs) {
                _executor->wait(handle, opCtx);
                // There's no replica set topology stability guarantees, i.e. this is a best effort.
                // If we receive command invokation errors (for example, network errors) just log it
                // and continue.
                if (!response->isOK()) {
                    LOGV2_WARNING(
                        12922001,
                        "Error from checkMetadataConsistency command invocation on secondary node",
                        "hostAndPort"_attr = hostAndPort,
                        "error"_attr = response->status);
                    continue;
                }

                auto cursorWithStatus = CursorResponse::parseFromBSON(response->data);

                if (cursorWithStatus.getStatus().code() == ErrorCodes::NotYetInitialized) {
                    // The secondary has not completed replica set initialization yet.
                    LOGV2_WARNING(12922003,
                                  "Secondary node hasn't completed replica set initialization",
                                  "hostAndPort"_attr = hostAndPort,
                                  "error"_attr = cursorWithStatus.getStatus());
                    continue;
                }

                if (cursorWithStatus.getStatus().isA<ErrorCategory::SnapshotError>()) {
                    // The secondary or config server can't serve the snapshot read.
                    LOGV2_WARNING(13017700,
                                  "Secondary node can't read at the requested timestamp",
                                  "hostAndPort"_attr = hostAndPort,
                                  "error"_attr = cursorWithStatus.getStatus());
                    continue;
                }

                if (cursorWithStatus.getStatus().code() == ErrorCodes::CallbackCanceled) {
                    // Thrown if interrupted while waiting for readConcern.
                    LOGV2_WARNING(12922308,
                                  "Secondary node was interrupted",
                                  "hostAndPort"_attr = hostAndPort,
                                  "error"_attr = cursorWithStatus.getStatus());
                    continue;
                }

                auto cursor = uassertStatusOK(std::move(cursorWithStatus));

                // All other members belong to this same shard, so they share its shardId.
                remoteCursors.emplace_back(shardId.toString(), response->target, std::move(cursor));
            }

            if (remoteCursors.empty()) {
                return nullptr;
            }

            ResolvedNamespaceMap resolvedNamespaces;
            resolvedNamespaces[nss] = {nss, {}};

            auto expCtx = ExpressionContextBuilder{}
                              .opCtx(opCtx)
                              .mongoProcessInterface(MongoProcessInterface::create(opCtx))
                              .ns(nss)
                              .resolvedNamespace(std::move(resolvedNamespaces))
                              .build();

            // 'DocumentSourceMergeCursors' is the only source: a pipeline that streams the
            // secondaries' cursors as they are consumed.
            AsyncResultsMergerParams armParams{std::move(remoteCursors), nss};
            auto mergeStage = DocumentSourceMergeCursors::create(expCtx, std::move(armParams));
            auto pipeline = Pipeline::create({std::move(mergeStage)}, expCtx);
            return plan_executor_factory::make(expCtx, std::move(pipeline));
        }

        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }

        std::shared_ptr<executor::TaskExecutor> _executor;
        std::vector<std::tuple<HostAndPort,
                               executor::TaskExecutor::CallbackHandle,
                               std::shared_ptr<executor::RemoteCommandResponse>>>
            _secondaryJobs;
    };
};
MONGO_REGISTER_COMMAND(ShardsvrCheckMetadataConsistencyParticipantCommand).forShard();

}  // namespace
}  // namespace mongo
