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
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/ddl_lock_manager.h"
#include "mongo/db/s/metadata_consistency_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangShardCheckMetadataBeforeDDLLock);
MONGO_FAIL_POINT_DEFINE(tripwireShardCheckMetadataAfterDDLLock);

MONGO_FAIL_POINT_DEFINE(hangShardCheckMetadataBeforeEstablishCursors);
MONGO_FAIL_POINT_DEFINE(throwExceededTimeLimitOnCheckMetadataBeforeEstablishCursors);
MONGO_FAIL_POINT_DEFINE(tripwireShardCheckMetadataAfterEstablishCursors);

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
        auto db = DatabaseType::parseOwned(IDLParserContext("DatabaseType"), std::move(rawDb));
        if (db.getName() == DatabaseName::kAdmin.db()) {
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
        databases.emplace_back(DatabaseName::kConfig.db().toString(),
                               ShardId::kConfigServerId,
                               DatabaseVersion::makeFixed());
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

            const auto response = [&] {
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
        decltype(auto) _runWithDbMetadaLockDeadline(OperationContext* opCtx,
                                                    Milliseconds timeout,
                                                    const NamespaceString& nss,
                                                    Callback&& cb) {
            const auto now = opCtx->getServiceContext()->getFastClockSource()->now();
            const auto deadline = now + timeout;
            try {
                return opCtx->runWithDeadline(deadline, ErrorCodes::ExceededTimeLimit, cb);
            } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
                const auto now = opCtx->getServiceContext()->getFastClockSource()->now();
                if (now >= deadline) {
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
                const NamespaceString dbNss{db.getName(), nss.coll()};
                ScopedSetShardRole scopedSetShardRole(opCtx,
                                                      dbNss,
                                                      boost::none /* shardVersion */,
                                                      db.getVersion() /* databaseVersion */);


                const auto dbCheckWithDeadlineIfSet = [&] {
                    auto checkMetadataForDb = [&]() {
                        try {
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
                            LOGV2_DEBUG(8840400,
                                        1,
                                        "Received StaleDbVersion error while trying to run "
                                        "database metadata checks",
                                        logAttrs(dbNss.dbName()),
                                        "error"_attr = redact(ex));
                            return ex.toStatus();
                        }
                    };

                    if (request().getDbMetadataLockMaxTimeMS().is_initialized()) {
                        const auto timeout =
                            Milliseconds(request().getDbMetadataLockMaxTimeMS().value());
                        return _runWithDbMetadaLockDeadline(
                            opCtx, timeout, dbNss, checkMetadataForDb);
                    } else {
                        return checkMetadataForDb();
                    }
                };

                bool skippedMetadataChecks = false;
                auto status = dbCheckWithDeadlineIfSet();
                if (!status.isOK()) {
                    auto extraInfo = status.extraInfo<StaleDbRoutingVersion>();
                    if (!extraInfo || extraInfo->getVersionWanted()) {
                        // In case there is a wanted shard version means that the metadata is stale
                        // and we are going to skip the checks.
                        skippedMetadataChecks = true;
                    } else {
                        // In case the shard doesn't know about the collection, we perform a
                        // refresh and re-try the metadata checks.
                        (void)onDbVersionMismatchNoExcept(
                            opCtx, dbNss.dbName().db(), extraInfo->getVersionReceived());

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
                    return _establishCursorOnParticipants(opCtx, nss);
                };

                if (request().getDbMetadataLockMaxTimeMS().is_initialized()) {
                    const auto timeout =
                        Milliseconds(request().getDbMetadataLockMaxTimeMS().value());
                    return _runWithDbMetadaLockDeadline(opCtx, timeout, nss, establishDBCursors);
                } else {
                    return establishDBCursors();
                }
            }();

            return _mergeCursors(opCtx, nss, std::move(dbCursors));
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
            // Shard requests
            const auto shardOpKey = UUID::gen();
            ShardsvrCheckMetadataConsistencyParticipant participantRequest{nss};
            participantRequest.setCommonFields(request().getCommonFields());
            participantRequest.setPrimaryShardId(ShardingState::get(opCtx)->shardId());
            participantRequest.setCursor(request().getCursor());
            const auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
            auto participantRequestWithOpKey =
                appendOpKey(shardOpKey, participantRequest.toBSON({}));
            std::vector<std::pair<ShardId, BSONObj>> requests;
            requests.reserve(participants.size() + 1);
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
            static constexpr StringData kLockReason{"checkMetadataConsistency"_sd};
            auto ddlLockManager = DDLLockManager::get(opCtx);
            hangShardCheckMetadataBeforeDDLLock.pauseWhileSet();
            const auto dbDDLLock = ddlLockManager->lock(
                opCtx, nss.db(), kLockReason, DDLLockManager::kDefaultLockTimeout);
            tassert(9504001,
                    "Expected interrupt before tripwireShardCheckMetadataAfterDDLLock",
                    !tripwireShardCheckMetadataAfterDDLLock.shouldFail());

            hangShardCheckMetadataBeforeEstablishCursors.pauseWhileSet();
            if (throwExceededTimeLimitOnCheckMetadataBeforeEstablishCursors.shouldFail()) {
                uasserted(ErrorCodes::ExceededTimeLimit,
                          str::stream() << "Timing out before establishing cursors on "
                                           "checkMetadataConsistency for nss "
                                        << nss.toStringForErrorMsg());
            }
            {
                Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IS);
                const auto scopedDss =
                    DatabaseShardingState::assertDbLockedAndAcquireShared(opCtx, nss.dbName());
                scopedDss->assertIsPrimaryShardForDb(opCtx);
            }

            hangShardCheckMetadataBeforeEstablishCursors.pauseWhileSet();
            auto cursors = establishCursors(opCtx,
                                            Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                                            nss,
                                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                            requests,
                                            false /* allowPartialResults */,
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
                {Privilege(ResourcePattern::forClusterResource(), ActionType::internal)}};

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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} shardsvrCheckMetadataConsistencyCommand;

}  // namespace
}  // namespace mongo
