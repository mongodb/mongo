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


#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/metadata_consistency_types_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/cursor_response_gen.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/ddl_lock_manager.h"
#include "mongo/db/s/metadata_consistency_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/async_results_merger_params_gen.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

/*
 * Retrieve from config server the list of databases for which this shard is primary for.
 */
std::vector<DatabaseType> getDatabasesThisShardIsPrimaryFor(OperationContext* opCtx) {
    const auto thisShardId = ShardingState::get(opCtx)->shardId();
    const auto configServer = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto rawDatabases{uassertStatusOK(configServer->exhaustiveFindOnConfig(
                                          opCtx,
                                          ReadPreferenceSetting{ReadPreference::Nearest},
                                          repl::ReadConcernLevel::kMajorityReadConcern,
                                          NamespaceString::kConfigDatabasesNamespace,
                                          BSON(DatabaseType::kPrimaryFieldName << thisShardId),
                                          BSONObj() /* No sorting */,
                                          boost::none /* No limit */))
                          .docs};
    std::vector<DatabaseType> databases;
    databases.reserve(rawDatabases.size());
    for (auto&& rawDb : rawDatabases) {
        databases.emplace_back(
            DatabaseType::parseOwned(IDLParserContext("DatabaseType"), std::move(rawDb)));
    }
    if (thisShardId == ShardId::kConfigServerId) {
        // Config database
        databases.emplace_back(
            DatabaseName::kConfig, ShardId::kConfigServerId, DatabaseVersion::makeFixed());
    }
    return databases;
}

class ShardsvrCheckMetadataConsistencyCommand final
    : public TypedCommand<ShardsvrCheckMetadataConsistencyCommand> {
public:
    using Request = ShardsvrCheckMetadataConsistency;
    using Response = CursorInitialReply;

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

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            {
                // Ensure that opCtx will get interrupted in the event of a stepdown.
                Lock::GlobalLock lk(opCtx, MODE_IX);
                uassert(ErrorCodes::InterruptedDueToReplStateChange,
                        "Not primary while attempting to start a metadata consistency check",
                        repl::ReplicationCoordinator::get(opCtx)->getMemberState().primary());
            }

            auto response = [&] {
                const auto nss = ns();
                switch (metadata_consistency_util::getCommandLevel(nss)) {
                    case MetadataConsistencyCommandLevelEnum::kClusterLevel:
                        return _runClusterLevel(opCtx, nss);
                    case MetadataConsistencyCommandLevelEnum::kDatabaseLevel:
                        return _runDatabaseLevel(opCtx, nss);
                    case MetadataConsistencyCommandLevelEnum::kCollectionLevel:
                        return _runCollectionLevel(opCtx, nss);
                    default:
                        MONGO_UNREACHABLE;
                }
            }();

            // Make sure the response gets invalidated in case of interruption
            opCtx->checkForInterrupt();

            return response;
        }

    private:
        Response _runClusterLevel(OperationContext* opCtx, const NamespaceString& nss) {
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << Request::kCommandName
                                  << " command on admin database can only be run without "
                                     "collection name. Found unexpected collection name: "
                                  << nss.coll(),
                    nss.isCollectionlessCursorNamespace());

            std::vector<RemoteCursor> cursors;

            // Need to retrieve a list of databases which this shard is primary for and run the
            // command on each of them.
            for (const auto& db : getDatabasesThisShardIsPrimaryFor(opCtx)) {
                const auto dbNss = NamespaceStringUtil::deserialize(db.getDbName(), nss.coll());
                ScopedSetShardRole scopedSetShardRole(opCtx,
                                                      dbNss,
                                                      boost::none /* shardVersion */,
                                                      db.getVersion() /* databaseVersion */);
                try {
                    auto dbCursors = _establishCursorOnParticipants(opCtx, dbNss);
                    cursors.insert(cursors.end(),
                                   std::make_move_iterator(dbCursors.begin()),
                                   std::make_move_iterator(dbCursors.end()));
                } catch (const ExceptionFor<ErrorCodes::StaleDbVersion>& ex) {
                    LOGV2_DEBUG(7328700,
                                1,
                                "Skipping database metadata check since the database is "
                                "currently being migrated",
                                logAttrs(dbNss.dbName()),
                                "error"_attr = redact(ex));
                }
            }

            return _mergeCursors(opCtx, nss, std::move(cursors));
        }

        Response _runDatabaseLevel(OperationContext* opCtx, const NamespaceString& nss) {
            return _mergeCursors(opCtx, nss, _establishCursorOnParticipants(opCtx, nss));
        }

        Response _runCollectionLevel(OperationContext* opCtx, const NamespaceString& nss) {
            return _mergeCursors(opCtx, nss, _establishCursorOnParticipants(opCtx, nss));
        }

        /*
         * Forwards metadata consistency command to all participants to establish remote cursors.
         * Forwarding is done under the DDL lock to serialize with concurrent DDL operations.
         */
        std::vector<RemoteCursor> _establishCursorOnParticipants(OperationContext* opCtx,
                                                                 const NamespaceString& nss) {
            std::vector<std::pair<ShardId, BSONObj>> requests;

            // Shard requests
            const auto shardOpKey = UUID::gen();
            ShardsvrCheckMetadataConsistencyParticipant participantRequest{nss};
            participantRequest.setCommonFields(request().getCommonFields());
            participantRequest.setPrimaryShardId(ShardingState::get(opCtx)->shardId());
            participantRequest.setCursor(request().getCursor());
            const auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
            auto participantRequestWithOpKey =
                appendOpKey(shardOpKey, participantRequest.toBSON({}));
            for (const auto& shardId : participants) {
                requests.emplace_back(shardId, participantRequestWithOpKey.getOwned());
            }

            // Config server request
            const auto configOpKey = UUID::gen();
            ConfigsvrCheckMetadataConsistency configRequest{nss};
            participantRequest.setCursor(request().getCursor());
            requests.emplace_back(ShardId::kConfigServerId,
                                  appendOpKey(configOpKey, configRequest.toBSON({})));

            // Take a DDL lock on the database
            const DDLLockManager::ScopedDatabaseDDLLock dbDDLLock{
                opCtx, nss.dbName(), "checkMetadataConsistency", MODE_S};

            return establishCursors(opCtx,
                                    Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                                    nss,
                                    ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                    requests,
                                    false /* allowPartialResults */,
                                    Shard::RetryPolicy::kIdempotentOrCursorInvalidated,
                                    {shardOpKey, configOpKey});
        }

        CursorInitialReply _mergeCursors(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         std::vector<RemoteCursor>&& cursors) {

            StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
            resolvedNamespaces[nss.coll()] = {nss, std::vector<BSONObj>{}};

            auto expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                            boost::none, /* explain */
                                                            false,       /* fromMongos */
                                                            false,       /* needsMerge */
                                                            false,       /* allowDiskUse */
                                                            false, /* bypassDocumentValidation */
                                                            false, /* isMapReduceCommand */
                                                            nss,
                                                            boost::none, /* runtimeConstants */
                                                            nullptr,     /* collator */
                                                            MongoProcessInterface::create(opCtx),
                                                            std::move(resolvedNamespaces),
                                                            boost::none /* collection UUID */);

            AsyncResultsMergerParams armParams{std::move(cursors), nss};
            auto docSourceMergeStage =
                DocumentSourceMergeCursors::create(expCtx, std::move(armParams));
            auto pipeline = Pipeline::create({std::move(docSourceMergeStage)}, expCtx);
            auto exec = plan_executor_factory::make(expCtx, std::move(pipeline));

            const auto batchSize = [&]() -> long long {
                const auto& cursorOpts = request().getCursor();
                if (cursorOpts && cursorOpts->getBatchSize()) {
                    return *cursorOpts->getBatchSize();
                } else {
                    return query_request_helper::kDefaultBatchSize;
                }
            }();

            ClientCursorParams cursorParams{
                std::move(exec),
                nss,
                AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
                APIParameters::get(opCtx),
                opCtx->getWriteConcern(),
                repl::ReadConcernArgs::get(opCtx),
                ReadPreferenceSetting::get(opCtx),
                request().toBSON({}),
                {Privilege(ResourcePattern::forClusterResource(nss.tenantId()),
                           ActionType::internal)}};

            return metadata_consistency_util::createInitialCursorReplyMongod(
                opCtx, std::move(cursorParams), batchSize);
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
    };
};
MONGO_REGISTER_COMMAND(ShardsvrCheckMetadataConsistencyCommand).forShard();

}  // namespace
}  // namespace mongo
