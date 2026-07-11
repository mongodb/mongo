// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/drop_collection_if_uuid_not_matching_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/drop_collection.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/write_concern.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardsvrDropCollectionIfUUIDNotMatchingWithWriteConcernCommand final
    : public TypedCommand<ShardsvrDropCollectionIfUUIDNotMatchingWithWriteConcernCommand> {
public:
    bool skipApiVersionCheck() const override {
        /* Internal command (server to server) */
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    std::string help() const override {
        return "Internal command aimed to remove stale entries from the local collection catalog.";
    }

    using Request = ShardsvrDropCollectionIfUUIDNotMatchingWithWriteConcernRequest;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassertStatusOK(dropCollectionIfUUIDNotMatching(
                opCtx, ns(), request().getExpectedCollectionUUID(), true /* fromMigrate */));
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
MONGO_REGISTER_COMMAND(ShardsvrDropCollectionIfUUIDNotMatchingWithWriteConcernCommand).forShard();
}  // namespace
}  // namespace mongo
