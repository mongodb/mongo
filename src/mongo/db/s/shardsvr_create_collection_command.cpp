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


#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/create_collection_coordinator.h"
#include "mongo/db/s/create_collection_coordinator_document_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardsvrCreateCollectionCommand final : public TypedCommand<ShardsvrCreateCollectionCommand> {
public:
    using Request = ShardsvrCreateCollection;
    using Response = CreateCollectionResponse;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. Creates a collection.";
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            uassert(ErrorCodes::NotImplemented,
                    "Create Collection path has not been implemented",
                    request().getShardKey());

            const auto createCollectionCoordinator = [&] {
                auto requestToForward = request().getShardsvrCreateCollectionRequest();
                // Validates and sets missing time-series options fields automatically.
                if (requestToForward.getTimeseries()) {
                    auto timeseriesOptions = *requestToForward.getTimeseries();
                    uassertStatusOK(
                        timeseries::validateAndSetBucketingParameters(timeseriesOptions));
                    requestToForward.setTimeseries(std::move(timeseriesOptions));
                }

                FixedFCVRegion fixedFcvRegion{opCtx};

                auto coordinatorDoc = [&] {
                    if (feature_flags::gAuthoritativeShardCollection.isEnabled(*fixedFcvRegion)) {
                        const DDLCoordinatorTypeEnum coordType =
                            DDLCoordinatorTypeEnum::kCreateCollection;
                        auto doc = CreateCollectionCoordinatorDocument();
                        doc.setShardingDDLCoordinatorMetadata({{ns(), coordType}});
                        doc.setShardsvrCreateCollectionRequest(requestToForward);
                        return doc.toBSON();
                    } else {
                        const DDLCoordinatorTypeEnum coordType =
                            DDLCoordinatorTypeEnum::kCreateCollectionPre71Compatible;
                        auto doc = CreateCollectionCoordinatorDocumentLegacy();
                        doc.setShardingDDLCoordinatorMetadata({{ns(), coordType}});
                        doc.setShardsvrCreateCollectionRequest(requestToForward);
                        return doc.toBSON();
                    }
                }();

                auto service = ShardingDDLCoordinatorService::getService(opCtx);
                return dynamic_pointer_cast<CreateCollectionResponseProvider>(
                    service->getOrCreateInstance(opCtx, std::move(coordinatorDoc)));
            }();

            return createCollectionCoordinator->getResult(opCtx);
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
MONGO_REGISTER_COMMAND(ShardsvrCreateCollectionCommand).forShard();

}  // namespace
}  // namespace mongo
