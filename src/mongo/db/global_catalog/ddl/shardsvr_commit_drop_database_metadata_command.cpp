/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/type_oplog_catalog_metadata_gen.h"
#include "mongo/db/query/write_ops/delete.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {

void commitDropDatabaseMetadataLocally(OperationContext* opCtx, const DatabaseName& dbName) {
    auto dbNameStr = DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());

    LOGV2_DEBUG(10105907,
                1,
                "Updating database sharding in-memory state onDropDatabaseMetadata",
                "dbName"_attr = dbName);

    // Remove database metadata from `config.shard.catalog.databases`.
    {
        write_ops::DeleteCommandRequest deleteOp(
            NamespaceString::kConfigShardCatalogDatabasesNamespace);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(BSON(DatabaseType::kDbNameFieldName << dbNameStr));
            entry.setMulti(false);
            return entry;
        }()});
        deleteOp.setWriteConcern(defaultMajorityWriteConcern());

        DBDirectClient client(opCtx);
        const auto result = client.remove(std::move(deleteOp));
        write_ops::checkWriteErrors(result);
    }

    // Write an oplog 'c' entry to inform secondaries on how to populate the DSS.
    {
        repl::MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(NamespaceString::makeCommandNamespace(dbName));
        oplogEntry.setObject(DropDatabaseMetadataOplogEntry{dbNameStr, dbName}.toBSON());
        oplogEntry.setOpTime(OplogSlot());
        oplogEntry.setWallClockTime(opCtx->fastClockSource().now());

        writeConflictRetry(opCtx, "dropDatabaseMetadata", NamespaceString::kRsOplogNamespace, [&] {
            AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);
            WriteUnitOfWork wuow(opCtx);
            repl::OpTime opTime = repl::logOp(opCtx, &oplogEntry);
            uassert(9980401,
                    str::stream() << "Failed to create new oplog entry for dropDatabaseMetadata"
                                  << " with opTime: " << oplogEntry.getOpTime().toString() << ": "
                                  << redact(oplogEntry.toBSON()),
                    !opTime.isNull());
            wuow.commit();
        });
    }

    // Update DSR in primary node.
    auto scopedDsr = DatabaseShardingRuntime::acquireExclusive(opCtx, dbName);
    scopedDsr->clearDbMetadata();
}

class ShardsvrCommitDropDatabaseMetadataCommand final
    : public TypedCommand<ShardsvrCommitDropDatabaseMetadataCommand> {
public:
    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Internal command. This command aims to commit a dropDatabase operation to the "
               "shard catalog.";
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    using Request = ShardsvrCommitDropDatabaseMetadata;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            uassert(ErrorCodes::InvalidOptions,
                    fmt::format("{} expected to be called within a retryable write ",
                                Request::kCommandName),
                    TransactionParticipant::get(opCtx));

            const auto dbName = request().getDbName();

            LOGV2(10105902,
                  "About to commit dropDatabase metadata in the shard catalog",
                  "dbName"_attr = dbName);

            {
                // Using the original operation context, the write operations to update the
                // shard catalog would fail since retryable writes are not compatible with
                // applying the WriteUnitOfWork as a transaction (kGroupForTransaction). A tactical
                // solution is to use an alternative client as well as a new operation context.

                auto newClient = getGlobalServiceContext()
                                     ->getService(ClusterRole::ShardServer)
                                     ->makeClient("ShardsvrCommitDropDatabaseMetadata");
                AlternativeClientRegion acr(newClient);
                auto newOpCtx = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
                newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                const auto dbNameStr =
                    DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());

                auto newOpCtxPtr = newOpCtx.get();

                commitDropDatabaseMetadataLocally(newOpCtxPtr, dbName);
            }

            LOGV2(10105903,
                  "Committed dropDatabase metadata in the shard catalog",
                  "dbName"_attr = dbName);

            // Since no write that generated a retryable write oplog entry with this sessionId and
            // txnNumber happened, we need to make a dummy write so that the session gets durably
            // persisted on the oplog. This must be the last operation done on this command.
            DBDirectClient dbClient(opCtx);
            dbClient.update(NamespaceString::kServerConfigurationNamespace,
                            BSON("_id" << Request::kCommandName),
                            BSON("$inc" << BSON("count" << 1)),
                            true /* upsert */,
                            false /* multi */);
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return true;
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

MONGO_REGISTER_COMMAND(ShardsvrCommitDropDatabaseMetadataCommand).forShard();

}  // namespace
}  // namespace mongo
