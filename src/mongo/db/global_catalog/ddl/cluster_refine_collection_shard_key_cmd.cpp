// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/refine_collection_shard_key_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangRefineCollectionShardKeyAfterRefresh);

class RefineCollectionShardKeyCommand final : public TypedCommand<RefineCollectionShardKeyCommand> {
public:
    using Request = RefineCollectionShardKey;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const NamespaceString& nss = ns();

            if (MONGO_unlikely(hangRefineCollectionShardKeyAfterRefresh.shouldFail())) {
                LOGV2(22756, "Hit hangRefineCollectionShardKeyAfterRefresh failpoint");
                hangRefineCollectionShardKeyAfterRefresh.pauseWhileSet(opCtx);
            }

            // Send it to the primary shard
            RefineCollectionShardKeyRequest requestParamObj;
            requestParamObj.setNewShardKey(request().getKey());
            requestParamObj.setCollectionUUID(request().getCollectionUUID());
            requestParamObj.setEnforceUniquenessCheck(request().getEnforceUniquenessCheck());
            ShardsvrRefineCollectionShardKey refineCollectionShardKeyCommand(nss);
            refineCollectionShardKeyCommand.setRefineCollectionShardKeyRequest(requestParamObj);
            generic_argument_util::setMajorityWriteConcern(refineCollectionShardKeyCommand,
                                                           &opCtx->getWriteConcern());

            sharding::router::DBPrimaryRouter router(opCtx, nss.dbName());
            router.route(Request::kCommandParameterFieldName,
                         [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                             auto cmdResponse =
                                 executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                                     opCtx,
                                     nss.dbName(),
                                     dbInfo,
                                     refineCollectionShardKeyCommand.toBSON(),
                                     ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                     Shard::RetryPolicy::kIdempotent);

                             const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
                             uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));
                         });
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::refineCollectionShardKey));
        }
    };

    std::string help() const override {
        return "Adds a suffix to the shard key of an existing collection ('refines the shard "
               "key').";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(RefineCollectionShardKeyCommand).forRouter();

}  // namespace
}  // namespace mongo
