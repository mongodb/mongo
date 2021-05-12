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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/reshard_collection_gen.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"

namespace mongo {
namespace {

class ReshardCollectionCmd : public TypedCommand<ReshardCollectionCmd> {
public:
    using Request = ReshardCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const auto& nss = ns();
            ShardsvrReshardCollection shardsvrReshardCollection(nss, request().getKey());
            shardsvrReshardCollection.setDbName(request().getDbName());
            shardsvrReshardCollection.setUnique(request().getUnique());
            shardsvrReshardCollection.setCollation(request().getCollation());
            shardsvrReshardCollection.set_presetReshardedChunks(
                request().get_presetReshardedChunks());
            shardsvrReshardCollection.setZones(request().getZones());
            shardsvrReshardCollection.setNumInitialChunks(request().getNumInitialChunks());

            auto catalogCache = Grid::get(opCtx)->catalogCache();
            const auto dbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, nss.db()));

            auto cmdResponse = executeCommandAgainstDatabasePrimary(
                opCtx,
                "admin",
                dbInfo,
                CommandHelpers::appendMajorityWriteConcern(shardsvrReshardCollection.toBSON({}),
                                                           opCtx->getWriteConcern()),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kIdempotent);

            const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
            uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));
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
        return "Reshard an already sharded collection on a new shard key.";
    }
};

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(ReshardCollectionCmd, resharding::gFeatureFlagResharding);

}  // namespace
}  // namespace mongo
