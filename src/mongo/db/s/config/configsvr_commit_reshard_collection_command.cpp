/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/commit_reshard_collection_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {


UUID retrieveReshardingUUID(OperationContext* opCtx, const NamespaceString& ns) {
    repl::ReadConcernArgs::get(opCtx) =
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

    const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
    const auto collEntry = catalogClient->getCollection(opCtx, ns);

    uassert(
        ErrorCodes::NoSuchReshardCollection,
        format(FMT_STRING("Could not find resharding metadata for {}"), ns.toStringForErrorMsg()),
        collEntry.getReshardingFields());

    return collEntry.getReshardingFields()->getReshardingUUID();
}


class ConfigsvrCommitReshardCollectionCommand final
    : public TypedCommand<ConfigsvrCommitReshardCollectionCommand> {
public:
    using Request = ConfigsvrCommitReshardCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    format(FMT_STRING("{} command not enabled"), definition()->getName()),
                    resharding::gFeatureFlagResharding.isEnabled(
                        serverGlobalParams.featureCompatibility));
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            uassert(
                ErrorCodes::IllegalOperation,
                format(FMT_STRING("{} can only be run on config servers"), definition()->getName()),
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            UUID reshardingUUID = retrieveReshardingUUID(opCtx, ns());

            auto machine = resharding::tryGetReshardingStateMachine<ReshardingCoordinatorService,
                                                                    ReshardingCoordinator,
                                                                    ReshardingCoordinatorDocument>(
                opCtx, reshardingUUID);

            uassert(ErrorCodes::NoSuchReshardCollection,
                    "Could not find in-progress resharding operation to commit",
                    machine);
            (*machine)->onOkayToEnterCritical();
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Allows a reshard operation to enter critical section ASAP.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} configsvrCommitReshardCollectionCommand;

}  // namespace
}  // namespace mongo
