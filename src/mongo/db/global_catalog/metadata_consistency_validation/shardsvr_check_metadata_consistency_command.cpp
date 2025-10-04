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
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_types_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_util.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"
#include "mongo/s/query/exec/establish_cursors.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangShardCheckMetadataBeforeDDLLock);
MONGO_FAIL_POINT_DEFINE(tripwireShardCheckMetadataAfterDDLLock);

MONGO_FAIL_POINT_DEFINE(hangShardCheckMetadataBeforeEstablishCursors);
MONGO_FAIL_POINT_DEFINE(throwExceededTimeLimitOnCheckMetadataBeforeEstablishCursors);
MONGO_FAIL_POINT_DEFINE(tripwireShardCheckMetadataAfterEstablishCursors);

constexpr StringData kDDLLockReason = "checkMetadataConsistency"_sd;

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
        auto db = DatabaseType::parseOwned(std::move(rawDb), IDLParserContext("DatabaseType"));
        if (db.getDbName() == DatabaseName::kAdmin) {
            // TODO (SERVER-101175): Convert this into a new metadata inconsistency.
            // The 'admin' database should not be explicitly assigned a primary shard. It may exist
            // in the global catalog due to upgrade from an older MongoDB version.
            LOGV2_INFO(
                9866400,
                "Found internal 'admin' database registered in the global catalog during the "
                "execution of checkMetadataConsistency command. Skipping consistency check for "
                "this database.",
                "dbMetadata"_attr = db);
            continue;
        }
        databases.emplace_back(std::move(db));
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
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

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
                const auto commandLevel = metadata_consistency_util::getCommandLevel(nss);
                switch (commandLevel) {
                    case MetadataConsistencyCommandLevelEnum::kClusterLevel:
                        return _runClusterLevel(opCtx, nss);
                    case MetadataConsistencyCommandLevelEnum::kDatabaseLevel:
                        return _runDatabaseLevel(opCtx, nss);
                    case MetadataConsistencyCommandLevelEnum::kCollectionLevel:
                        return _runCollectionLevel(opCtx, nss);
                    default:
                        tasserted(1011706,
                                  str::stream()
                                      << "Unexpected parameter during the internal execution of "
                                         "checkMetadataConsistency command. The shard server was "
                                         "expecting to receive a cluster, database or collection "
                                         "level parameter, but received "
                                      << MetadataConsistencyCommandLevel_serializer(commandLevel)
                                      << " with namespace " << nss.toStringForErrorMsg());
                }
            }();

            // Make sure the response gets invalidated in case of interruption
            opCtx->checkForInterrupt();

            return response;
        }

    private:
        template <typename Callback>
        decltype(auto) _runWithDbMetadataLockDeadline(OperationContext* opCtx,
                                                      Milliseconds timeout,
                                                      const NamespaceString& nss,
                                                      Callback&& cb) {
            const auto now = opCtx->fastClockSource().now();
            const auto deadline = now + timeout;
            const auto guard = opCtx->makeDeadlineGuard(deadline, ErrorCodes::MaxTimeMSExpired);
            try {
                return std::forward<Callback>(cb)();
            } catch (const ExceptionFor<ErrorCategory::ExceededTimeLimitError>&) {
                // Need to catch the entire category of errors because there are parts across the
                // code base where we throw a specific error, ignoring the one set on the opCtx.
                // Please, see SERVER-104462 for more details.
                const auto now = opCtx->fastClockSource().now();
                if (now >= deadline) {
                    // Convert the error code to a specific one, indicating that the
                    // specific deadline related to DbMetadataLockMaxTimeMS has been exceeded
                    uasserted(9944001,
                              str::stream()
                                  << "Exceeded maximum time " << timeout
                                  << " while processing namespace " << nss.toStringForErrorMsg());
                }
                throw;
            }
        }

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

                const auto dbCheckWithDeadlineIfSet = [&] {
                    auto checkMetadataForDb = [&]() {
                        try {
                            hangShardCheckMetadataBeforeDDLLock.pauseWhileSet();
                            auto backoffStrategy = std::invoke([]() {
                                static const size_t retryCount{60};
                                static const size_t baseWaitTimeMs{50};
                                static const size_t maxWaitTimeMs{1000};

                                return DDLLockManager::TruncatedExponentialBackoffStrategy<
                                    retryCount,
                                    baseWaitTimeMs,
                                    maxWaitTimeMs>();
                            });
                            DDLLockManager::ScopedDatabaseDDLLock dbDDLLock{
                                opCtx, dbNss.dbName(), kDDLLockReason, MODE_S, backoffStrategy};
                            tassert(
                                9504001,
                                "Expected interrupt before tripwireShardCheckMetadataAfterDDLLock",
                                !tripwireShardCheckMetadataAfterDDLLock.shouldFail());

                            auto dbCursors = _establishCursorOnParticipants(opCtx, dbNss);
                            cursors.insert(cursors.end(),
                                           std::make_move_iterator(dbCursors.begin()),
                                           std::make_move_iterator(dbCursors.end()));
                            return Status::OK();
                        } catch (const ExceptionFor<ErrorCodes::StaleDbVersion>& ex) {
                            // Receiving a StaleDbVersion is because of one of these scenarios:
                            // - A movePrimary is changing the db primary shard.
                            // - The database is being dropped.
                            // - This shard doesn't know about the existence of the db.
                            LOGV2_DEBUG(
                                8840400,
                                1,
                                "Received StaleDbVersion error while trying to run database "
                                "metadata checks",
                                logAttrs(dbNss.dbName()),
                                "error"_attr = redact(ex));
                            return ex.toStatus();
                        }
                    };

                    if (request().getDbMetadataLockMaxTimeMS().is_initialized()) {
                        const auto timeout =
                            Milliseconds(request().getDbMetadataLockMaxTimeMS().value());
                        return _runWithDbMetadataLockDeadline(
                            opCtx, timeout, dbNss, checkMetadataForDb);
                    } else {
                        return checkMetadataForDb();
                    }
                };

                bool skippedMetadataChecks = false;
                auto status = dbCheckWithDeadlineIfSet();
                if (!status.isOK()) {
                    auto extraInfo = status.extraInfo<StaleDbRoutingVersion>();
                    tassert(9980500, "StaleDbVersion must have extraInfo", extraInfo);

                    if (feature_flags::gShardAuthoritativeDbMetadataCRUD.isEnabled(
                            VersionContext::getDecoration(opCtx),
                            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                        // If versionWanted exists:
                        //    - This means that the wanted version is higher than the one initially
                        //      looked at the config server. There has been a drop-create under the
                        //      same primary shard or a move out and move in again. There is no harm
                        //      to skip it.
                        //
                        // Otherwise:
                        //    - In the authoritative world, this means that this shard is no longer
                        //      the db primary shard. There has been a drop or move between taking
                        //      the information about the databases from the config server and
                        //      taking the DDL lock.
                        skippedMetadataChecks = true;
                    } else if (extraInfo->getVersionWanted()) {
                        // In case there is a wanted shard version means that the metadata is stale
                        // and we are going to skip the checks.
                        skippedMetadataChecks = true;
                    } else {
                        // In case the shard doesn't know about the database, we perform a refresh
                        // and re-try the metadata checks.
                        (void)FilteringMetadataCache::get(opCtx)->onDbVersionMismatch(
                            opCtx, dbNss.dbName(), extraInfo->getVersionReceived());

                        skippedMetadataChecks = !dbCheckWithDeadlineIfSet().isOK();
                    }
                }

                // All the other scenarios, we skip the metadata checks for this db.
                if (skippedMetadataChecks) {
                    LOGV2_DEBUG(
                        7328700,
                        1,
                        "Skipping database metadata check since the database version is stale",
                        logAttrs(dbNss.dbName()));
                }
            }

            return _mergeCursors(opCtx, nss, std::move(cursors));
        }

        Response _runDatabaseLevel(OperationContext* opCtx, const NamespaceString& nss) {
            auto dbCursors = [&]() {
                auto establishDBCursors = [&]() {
                    hangShardCheckMetadataBeforeDDLLock.pauseWhileSet();
                    DDLLockManager::ScopedDatabaseDDLLock dbDDLLock{
                        opCtx, nss.dbName(), kDDLLockReason, MODE_S};
                    tassert(9504002,
                            "Expected interrupt before tripwireShardCheckMetadataAfterDDLLock",
                            !tripwireShardCheckMetadataAfterDDLLock.shouldFail());
                    return _establishCursorOnParticipants(opCtx, nss);
                };

                if (request().getDbMetadataLockMaxTimeMS().is_initialized()) {
                    const auto timeout =
                        Milliseconds(request().getDbMetadataLockMaxTimeMS().value());
                    return _runWithDbMetadataLockDeadline(opCtx, timeout, nss, establishDBCursors);
                } else {
                    return establishDBCursors();
                }
            }();

            return _mergeCursors(opCtx, nss, std::move(dbCursors));
        }

        Response _runCollectionLevel(OperationContext* opCtx, const NamespaceString& nss) {
            auto collCursors = [&]() {
                hangShardCheckMetadataBeforeDDLLock.pauseWhileSet();
                DDLLockManager::ScopedCollectionDDLLock dbDDLLock{
                    opCtx, nss, kDDLLockReason, MODE_S};
                tassert(9504003,
                        "Expected interrupt before tripwireShardCheckMetadataAfterDDLLock",
                        !tripwireShardCheckMetadataAfterDDLLock.shouldFail());
                return _establishCursorOnParticipants(opCtx, nss);
            }();

            return _mergeCursors(opCtx, nss, std::move(collCursors));
        }

        /*
         * Forwards metadata consistency command to all participants to establish remote cursors.
         * Forwarding is done under the DDL lock to serialize with concurrent DDL operations.
         */
        std::vector<RemoteCursor> _establishCursorOnParticipants(OperationContext* opCtx,
                                                                 const NamespaceString& nss) {
            hangShardCheckMetadataBeforeEstablishCursors.pauseWhileSet();
            if (throwExceededTimeLimitOnCheckMetadataBeforeEstablishCursors.shouldFail()) {
                uasserted(ErrorCodes::ExceededTimeLimit,
                          str::stream() << "Timing out before establishing cursors on "
                                           "checkMetadataConsistency for nss "
                                        << nss.toStringForErrorMsg());
            }

            // Shard requests
            const auto shardOpKey = UUID::gen();
            ShardsvrCheckMetadataConsistencyParticipant participantRequest{nss};
            participantRequest.setCommonFields(request().getCommonFields());
            participantRequest.setPrimaryShardId(ShardingState::get(opCtx)->shardId());
            participantRequest.setCursor(request().getCursor());
            auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
            if (std::find(participants.begin(), participants.end(), ShardId::kConfigServerId) ==
                participants.end()) {
                // Include config server as shard when it is not embedded
                participants.push_back(ShardId::kConfigServerId);
            }
            BSONObjBuilder participantRequestBob;
            participantRequest.serialize(&participantRequestBob);
            appendOpKey(shardOpKey, &participantRequestBob);
            auto participantRequestWithOpKey = participantRequestBob.obj();

            std::vector<AsyncRequestsSender::Request> requests;
            requests.reserve(participants.size() + 1);
            for (const auto& shardId : participants) {
                requests.emplace_back(shardId, participantRequestWithOpKey.getOwned());
            }

            // Config server request
            const auto configOpKey = UUID::gen();
            ConfigsvrCheckMetadataConsistency configRequest{nss};
            configRequest.setCursor(request().getCursor());

            BSONObjBuilder configRequestBob;
            configRequest.serialize(&configRequestBob);
            appendOpKey(configOpKey, &configRequestBob);
            requests.emplace_back(ShardId::kConfigServerId, configRequestBob.obj());

            auto cursors = establishCursors(opCtx,
                                            Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                                            nss,
                                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                            requests,
                                            false /* allowPartialResults */,
                                            nullptr /* RoutingContext */,
                                            Shard::RetryPolicy::kIdempotentOrCursorInvalidated,
                                            {shardOpKey, configOpKey});
            tassert(9504004,
                    "Expected interrupt before tripwireShardCheckMetadataAfterEstablishCursors",
                    !tripwireShardCheckMetadataAfterEstablishCursors.shouldFail());
            return cursors;
        }

        CursorInitialReply _mergeCursors(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         std::vector<RemoteCursor>&& cursors) {

            ResolvedNamespaceMap resolvedNamespaces;
            resolvedNamespaces[nss] = {nss, std::vector<BSONObj>{}};

            auto expCtx = ExpressionContextBuilder{}
                              .opCtx(opCtx)
                              .mongoProcessInterface(MongoProcessInterface::create(opCtx))
                              .ns(nss)
                              .resolvedNamespace(std::move(resolvedNamespaces))
                              .build();

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
                    return query_request_helper::getDefaultBatchSize();
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
                request().toBSON(),
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
