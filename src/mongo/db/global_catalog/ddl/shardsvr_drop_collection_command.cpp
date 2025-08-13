/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/drop_collection_coordinator.h"
#include "mongo/db/global_catalog/ddl/drop_collection_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/global_catalog/router_role_api/collection_routing_info_targeter.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardsvrDropCollectionCommand final : public TypedCommand<ShardsvrDropCollectionCommand> {
public:
    using Request = ShardsvrDropCollection;

    std::string help() const override {
        return "Internal command, which is exported by the primary sharding server. Do not call "
               "directly. Drops a collection.";
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            try {
                const auto coll = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, ns());

                uassert(ErrorCodes::IllegalOperation,
                        "Sharded time-series buckets collections cannot be dropped directly; drop "
                        "the logical namespace instead",
                        !coll.getTimeseriesFields() || !ns().isTimeseriesBucketsCollection() ||
                            coll.getUnsplittable());

            } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // The collection is not tracked or doesn't exist.
            }

            // Since this operation is not directly writing locally we need to force its db
            // profile level increase in order to be logged in "<db>.system.profile"
            CurOp::get(opCtx)->raiseDbProfileLevel(
                DatabaseProfileSettings::get(opCtx->getServiceContext())
                    .getDatabaseProfileLevel(ns().dbName()));

            auto coordinatorDoc = DropCollectionCoordinatorDocument();
            coordinatorDoc.setShardingDDLCoordinatorMetadata(
                {{ns(), DDLCoordinatorTypeEnum::kDropCollection}});
            coordinatorDoc.setCollectionUUID(request().getCollectionUUID());

            auto service = ShardingDDLCoordinatorService::getService(opCtx);
            auto dropCollCoordinator =
                checked_pointer_cast<DropCollectionCoordinator>(service->getOrCreateInstance(
                    opCtx, coordinatorDoc.toBSON(), FixedFCVRegion{opCtx}));

            dropCollCoordinator->getCompletionFuture().get(opCtx);
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
MONGO_REGISTER_COMMAND(ShardsvrDropCollectionCommand).forShard();

}  // namespace
}  // namespace mongo
