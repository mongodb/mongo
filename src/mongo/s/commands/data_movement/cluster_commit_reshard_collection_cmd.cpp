// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/commit_reshard_collection_gen.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class CommitReshardCollectionCmd : public TypedCommand<CommitReshardCollectionCmd> {
public:
    using Request = CommitReshardCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            LOGV2(5391600, "Beginning commitReshardCollection", logAttrs(ns()));
            ConfigsvrCommitReshardCollection cmd(ns());
            cmd.setDbName(request().getDbName());
            generic_argument_util::setMajorityWriteConcern(cmd, &opCtx->getWriteConcern());

            auto cfg = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto response =
                uassertStatusOK(cfg->runCommand(opCtx,
                                                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                                DatabaseName::kAdmin,
                                                cmd.toBSON(),
                                                Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(response.commandStatus);
            uassertStatusOK(response.writeConcernStatus);
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::reshardCollection));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Allow a resharding operation in progress to commit as soon "
               "as possible. This may mean a longer critical interval "
               "during which writes are blocked.";
    }
};
MONGO_REGISTER_COMMAND(CommitReshardCollectionCmd).forRouter();

}  // namespace
}  // namespace mongo
