/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/local_catalog/drop_database.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardsvrDropDatabaseParticipantCommand final
    : public TypedCommand<ShardsvrDropDatabaseParticipantCommand> {
public:
    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Internal command, which is exported by secondary sharding servers. Do not call "
               "directly. Participates in droping a database.";
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    using Request = ShardsvrDropDatabaseParticipant;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            const auto& dbName = request().getDbName();
            const bool fromMigrate = request().getFromMigrate();

            auto txnParticipant = TransactionParticipant::get(opCtx);
            if (txnParticipant) {
                // Using the original operation context, the next operations to drop the database
                // would use the same txnNumber, which would cause one of those to fail. A tactical
                // solution is to use an alternative client as well as a new operation context.

                // For the authoritative shards use the replay protection
                auto newClient = getGlobalServiceContext()
                                     ->getService(ClusterRole::ShardServer)
                                     ->makeClient("ShardsvrDropDatabaseParticipant");

                AlternativeClientRegion acr(newClient);
                auto cancelableOperationContext = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx)->getExecutorPool()->getFixedExecutor());
                cancelableOperationContext->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                _handleDropDatabase(cancelableOperationContext.get(), dbName, fromMigrate);
            } else {
                // For the non authoritative shards we do not care about replay protection
                _handleDropDatabase(opCtx, dbName, fromMigrate);
            }

            if (txnParticipant) {
                // To use the original opCtx from the command, we need to be in a different scope
                // than the AlternativeClientRegion. Therefore, the client associated with the opCtx
                // is reattached to the current thread.

                // Since no write that generated a retryable write oplog entry with this sessionId
                // and txnNumber happened, we need to make a dummy write so that the session gets
                // durably persisted on the oplog. This must be the last operation done on this
                // command.
                DBDirectClient dbClient(opCtx);
                dbClient.update(NamespaceString::kServerConfigurationNamespace,
                                BSON("_id" << Request::kCommandName),
                                BSON("$inc" << BSON("count" << 1)),
                                true /* upsert */,
                                false /* multi */);
            }
        }

    private:
        void _handleDropDatabase(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const bool& fromMigrate) {
            try {
                uassertStatusOK(dropDatabase(opCtx, dbName, fromMigrate));
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                LOGV2_DEBUG(5281101,
                            1,
                            "Received a ShardsvrDropDatabaseParticipant but did not find the "
                            "database locally",
                            "database"_attr = dbName);
            }
        }

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
MONGO_REGISTER_COMMAND(ShardsvrDropDatabaseParticipantCommand).forShard();

}  // namespace
}  // namespace mongo
