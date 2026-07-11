// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ShardsvrCreateCollectionParticipantCommand final
    : public TypedCommand<ShardsvrCreateCollectionParticipantCommand> {
public:
    using Request = ShardsvrCreateCollectionParticipant;

    bool adminOnly() const override {
        return false;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. Creates a collection.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto const shardingState = ShardingState::get(opCtx);
            shardingState->assertCanAcceptShardedCommands();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            const auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(6077300,
                    str::stream() << Request::kCommandName << " must be run as a retryable write",
                    txnParticipant);

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            MigrationDestinationManager::cloneCollectionIndexesAndOptions(
                opCtx,
                ns(),
                {*request().getCollectionUUID(),
                 request().getIndexes(),
                 request().getIdIndex(),
                 request().getOptions()});

            // Since no write that generated a retryable write oplog entry with this sessionId and
            // txnNumber happened, we need to make a dummy write so that the session gets durably
            // persisted on the oplog. This must be the last operation done on this command.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace,
                          BSON("_id" << Request::kCommandName),
                          BSON("$inc" << BSON("count" << 1)),
                          true /* upsert */,
                          false /* multi */);
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
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
MONGO_REGISTER_COMMAND(ShardsvrCreateCollectionParticipantCommand).forShard();

}  // namespace
}  // namespace mongo
