// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/collection_crud/capped_utils.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/util/assert_util.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardSvrConvertToCappedParticipantCommand final
    : public TypedCommand<ShardSvrConvertToCappedParticipantCommand> {
public:
    using Request = ShardsvrConvertToCappedParticipant;

    std::string help() const override {
        return "Internal command, which is exported by the shards. Do not call "
               "directly. Processes convertToCapped.";
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            const auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(8577201,
                    str::stream() << Request::kCommandName << " must be run as a retryable write",
                    txnParticipant);

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            const auto size = request().getSize();
            uassert(ErrorCodes::InvalidOptions,
                    "Capped collection size must be greater than zero",
                    size > 0);

            // Note: the receiver of this command is expected to be the only data bearing shard for
            // the collection - change stream readers expect to observe the effects of the DDL from
            // here.
            convertToCapped(opCtx, ns(), size, false /*fromMigrate*/, request().getTargetUUID());

            // In case no write that generated a retryable write oplog entry with this sessionId
            // and txnNumber has happened, we need to make a dummy write so that the session gets
            // durably persisted on the oplog. This must be the last operation done on this
            // command.
            DBDirectClient dbClient(opCtx);
            dbClient.update(NamespaceString::kServerConfigurationNamespace,
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
MONGO_REGISTER_COMMAND(ShardSvrConvertToCappedParticipantCommand).forShard();

}  // namespace
}  // namespace mongo
