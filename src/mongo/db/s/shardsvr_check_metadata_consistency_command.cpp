/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/metadata_consistency_types_gen.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/s/ddl_lock_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ShardsvrCheckMetadataConsistencyCommand final
    : public TypedCommand<ShardsvrCheckMetadataConsistencyCommand> {
public:
    using Request = ShardsvrCheckMetadataConsistency;
    using Response = CheckMetadataConsistencyResponse;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly.";
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            const auto& nss = ns();
            const auto& primaryShardId = ShardingState::get(opCtx)->shardId();
            std::vector<AsyncRequestsSender::Response> responses;

            {
                // Take a DDL lock on the database
                static constexpr StringData lockReason{"checkMetadataConsistency"_sd};
                auto ddlLockManager = DDLLockManager::get(opCtx);
                const auto dbDDLLock = ddlLockManager->lock(
                    opCtx, nss.db(), lockReason, DDLLockManager::kDefaultLockTimeout);

                // Send command to all shards
                ShardsvrCheckMetadataConsistencyParticipant participantRequest{nss};
                participantRequest.setPrimaryShardId(primaryShardId);
                const auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                responses = sharding_util::sendCommandToShards(
                    opCtx,
                    nss.db(),
                    participantRequest.toBSON({}),
                    participants,
                    Grid::get(opCtx)->getExecutorPool()->getFixedExecutor());
            }

            // Merge responses from shards
            std::vector<MetadataInconsistencyItem> inconsistenciesMerged;
            for (auto&& asyncResponse : responses) {
                auto response = uassertStatusOK(std::move(asyncResponse.swResponse));
                uassertStatusOK(getStatusFromCommandResult(response.data));

                auto data = MetadataInconsistencies::parseOwned(
                    IDLParserContext("MetadataInconsistencies"), std::move(response.data));

                auto& shardInconsistencies = data.getInconsistencies();
                inconsistenciesMerged.insert(inconsistenciesMerged.end(),
                                             std::make_move_iterator(shardInconsistencies.begin()),
                                             std::make_move_iterator(shardInconsistencies.end()));
            }

            return Response{_makeCursor(opCtx, inconsistenciesMerged, nss)};
        }

    private:
        CheckMetadataConsistencyResponseCursor _makeCursor(
            OperationContext* opCtx,
            const std::vector<MetadataInconsistencyItem>& inconsistencies,
            const NamespaceString& nss) {
            auto& cmd = request();

            const auto batchSize = [&] {
                if (cmd.getCursor() && cmd.getCursor()->getBatchSize()) {
                    return (long long)*cmd.getCursor()->getBatchSize();
                } else {
                    return std::numeric_limits<long long>::max();
                }
            }();

            auto expCtx = make_intrusive<ExpressionContext>(
                opCtx, std::unique_ptr<CollatorInterface>(nullptr), nss);
            auto ws = std::make_unique<WorkingSet>();
            auto root = std::make_unique<QueuedDataStage>(expCtx.get(), ws.get());

            for (auto&& inconsistency : inconsistencies) {
                WorkingSetID id = ws->allocate();
                WorkingSetMember* member = ws->get(id);
                member->keyData.clear();
                member->recordId = RecordId();
                member->resetDocument(SnapshotId(), inconsistency.toBSON().getOwned());
                member->transitionToOwnedObj();
                root->pushBack(id);
            }

            auto exec = uassertStatusOK(
                plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(root),
                                            &CollectionPtr::null,
                                            PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                            false, /* whether returned BSON must be owned */
                                            nss));

            std::vector<MetadataInconsistencyItem> firstBatch;
            size_t bytesBuffered = 0;
            for (long long objCount = 0; objCount < batchSize; objCount++) {
                BSONObj nextDoc;
                PlanExecutor::ExecState state = exec->getNext(&nextDoc, nullptr);
                if (state == PlanExecutor::IS_EOF) {
                    break;
                }
                invariant(state == PlanExecutor::ADVANCED);

                // If we can't fit this result inside the current batch, then we stash it for
                // later.
                if (!FindCommon::haveSpaceForNext(nextDoc, objCount, bytesBuffered)) {
                    exec->stashResult(nextDoc);
                    break;
                }

                int objsize = nextDoc.objsize();
                firstBatch.push_back(MetadataInconsistencyItem::parseOwned(
                    IDLParserContext("MetadataInconsistencyItem"), std::move(nextDoc)));
                bytesBuffered += objsize;
            }

            if (exec->isEOF()) {
                return CheckMetadataConsistencyResponseCursor(
                    0 /* cursorId */, nss, std::move(firstBatch));
            }

            exec->saveState();
            exec->detachFromOperationContext();

            // TODO: SERVER-72667: Add privileges for getMore()
            // Global cursor registration must be done without holding any locks.
            auto pinnedCursor = CursorManager::get(opCtx)->registerCursor(
                opCtx,
                {std::move(exec),
                 nss,
                 AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
                 APIParameters::get(opCtx),
                 opCtx->getWriteConcern(),
                 repl::ReadConcernArgs::get(opCtx),
                 ReadPreferenceSetting::get(opCtx),
                 cmd.toBSON({}),
                 {}});

            pinnedCursor->incNBatches();
            pinnedCursor->incNReturnedSoFar(firstBatch.size());

            return CheckMetadataConsistencyResponseCursor(
                pinnedCursor.getCursor()->cursorid(), nss, std::move(firstBatch));
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} shardsvrCheckMetadataConsistencyCommand;

}  // namespace
}  // namespace mongo
