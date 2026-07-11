// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/drop_database.h"
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
                auto newClient = getGlobalServiceContext()->getService()->makeClient(
                    "ShardsvrDropDatabaseParticipant");

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
