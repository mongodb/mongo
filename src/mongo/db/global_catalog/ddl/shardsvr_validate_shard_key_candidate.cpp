// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/shard_key_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrValidateShardKeyCandidateCommand final
    : public TypedCommand<ShardsvrValidateShardKeyCandidateCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the primary sharding server. Do not call "
               "directly. Validates a collection shard key candidate.";
    }

    using Request = ShardsvrValidateShardKeyCandidate;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {

            const ShardKeyPattern keyPattern(request().getKey());
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            {
                const auto coll =
                    acquireCollection(opCtx,
                                      CollectionAcquisitionRequest::fromOpCtx(
                                          opCtx, ns(), AcquisitionPrerequisites::kRead),
                                      MODE_IS);

                uassert(ErrorCodes::NamespaceNotSharded,
                        str::stream()
                            << "Can't execute " << Request::kCommandName
                            << " on unsharded collection " << redact(ns().toStringForErrorMsg()),
                        coll.getShardingDescription().isSharded());

                shardkeyutil::validateShardKeyIndexExistsOrCreateIfPossible(
                    opCtx,
                    ns(),
                    keyPattern,
                    boost::none,
                    coll.getShardingDescription().isUniqueShardKey(),
                    request().getEnforceUniquenessCheck().value_or(true),
                    shardkeyutil::ValidationBehaviorsLocalRefineShardKey(opCtx,
                                                                         coll.getCollectionPtr()),
                    coll.getCollectionPtr()->getTimeseriesOptions());
            }
            shardkeyutil::validateShardKeyIsNotEncrypted(opCtx, ns(), keyPattern);
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

        /**
         * The ns() for when Request's IDL specifies "namespace: concatenate_with_db".
         */
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }
    };
};
MONGO_REGISTER_COMMAND(ShardsvrValidateShardKeyCandidateCommand).forShard();

}  // namespace
}  // namespace mongo
