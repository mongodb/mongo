/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/commands.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/untrack_unsplittable_collection_gen.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/sharding_environment/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ClusterUntrackUnsplittableCollectionCommand final
    : public TypedCommand<ClusterUntrackUnsplittableCollectionCommand> {
public:
    using Request = ClusterUntrackUnsplittableCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardsvrUntrackUnsplittableCollection shardsvrRequest(ns());
            shardsvrRequest.setDbName(NamespaceString::kAdminCommandNamespace.dbName());
            generic_argument_util::setMajorityWriteConcern(shardsvrRequest);

            // Route the command to the primary shard.
            sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(), ns().dbName());
            router.route(
                opCtx,
                Request::kCommandParameterFieldName,
                [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                    auto response = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                        opCtx,
                        ns().dbName(),
                        dbInfo,
                        shardsvrRequest.toBSON(),
                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                        Shard::RetryPolicy::kIdempotent);

                    uassertStatusOK(AsyncRequestsSender::Response::getEffectiveStatus(response));
                });
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
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exposed for emergency use only. Safely unregisters from "
               "the sharding catalog a tracked unsharded collection. Requires the collection to be "
               "placed on its primary shard to succeed.";
    }
};

MONGO_REGISTER_COMMAND(ClusterUntrackUnsplittableCollectionCommand).forRouter();

}  // namespace
}  // namespace mongo
