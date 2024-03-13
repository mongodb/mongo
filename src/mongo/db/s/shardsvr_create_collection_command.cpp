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
#include "mongo/db/s/ddl_lock_manager.h"
#include "mongo/db/s/sharding_ddl_coordinator_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/sharding_state.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

void runCreateCommandDirectClient(OperationContext* opCtx,
                                  NamespaceString ns,
                                  const CreateCommand& cmd) {
    BSONObj createRes;
    DBDirectClient localClient(opCtx);
    // Forward the api check rules enforced by the client
    localClient.runCommand(ns.dbName(), cmd.toBSON(APIParameters::get(opCtx).toBSON()), createRes);
    auto createStatus = getStatusFromCommandResult(createRes);
    uassertStatusOK(createStatus);
}

bool isAlwaysUntracked(OperationContext* opCtx,
                       NamespaceString&& nss,
                       const ShardsvrCreateCollection& request) {
    bool isFromCreateCommand = !request.getIsFromCreateUnsplittableCollectionTestCommand();
    bool isTimeseries = request.getTimeseries().has_value();
    bool isView = request.getViewOn().has_value();
    bool hasCustomCollation = request.getCollation().has_value();
    bool isEncryptedCollection =
        request.getEncryptedFields().has_value() || nss.isFLE2StateCollection();
    bool hasApiParams = APIParameters::get(opCtx).getParamsPassed();

    // TODO SERVER-83878 Remove isFromCreateCommand && isTimeseries
    // TODO SERVER-81936 Remove hasCustomCollation
    // TODO SERVER-79248 or SERVER-79254 remove isEncryptedCollection once we both cleanup
    // and compaction coordinator work on unsplittable collections
    // TODO SERVER-86018 Remove hasApiParams
    return isView || nss.isNamespaceAlwaysUntracked() || (isFromCreateCommand && isTimeseries) ||
        hasCustomCollation || isEncryptedCollection || hasApiParams;
}

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

    bool allowedInTransactions() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            bool isUnsplittable = request().getUnsplittable();
            bool isFromCreateUnsplittableCommand =
                request().getIsFromCreateUnsplittableCollectionTestCommand();
            bool hasShardKey = request().getShardKey().has_value();
            if (opCtx->inMultiDocumentTransaction()) {
                uassert(ErrorCodes::InvalidOptions,
                        "cannot shard a collection in a transaction",
                        isUnsplittable);

                auto cmd = create_collection_util::makeCreateCommand(
                    opCtx, ns(), request().getShardsvrCreateCollectionRequest());
                runCreateCommandDirectClient(opCtx, ns(), cmd);
                return CreateCollectionResponse{ShardVersion::UNSHARDED()};
            }

            uassert(ErrorCodes::NotImplemented,
                    "Create Collection path has not been implemented",
                    isUnsplittable || hasShardKey);

            tassert(ErrorCodes::InvalidOptions,
                    "unsplittable collections must be created with shard key {_id: 1}",
                    !isUnsplittable || !hasShardKey ||
                        request().getShardKey()->woCompare(
                            sharding_ddl_util::unsplittableCollectionShardKey().toBSON()) == 0);

            // The request might be coming from an "insert" operation. To keep "insert" behaviour
            // back-compatible, a serialization is expected instead of returning "conflicting
            // operation in progress". Because we are holding a FCVFixedRegion (which locks
            // upgrade/downgrade), release the lock before serializing and re-acquire it once the
            // on-going creation is over.
            while (true) {
                boost::optional<FixedFCVRegion> optFixedFcvRegion{boost::in_place_init, opCtx};
                // In case of "unsplittable" collections, create the collection locally if either
                // the feature flag is disabled or the nss identifies a collection which should
                // always be local
                // TODO (SERVER-86295) change this check according to the "trackCollectionIfExists"
                // coordinator parameter
                if (isUnsplittable && !isFromCreateUnsplittableCommand) {
                    bool isTrackUnshardedUponCreationDisabled =
                        !feature_flags::gTrackUnshardedCollectionsUponCreation.isEnabled(
                            (*optFixedFcvRegion)->acquireFCVSnapshot());
                    if (isTrackUnshardedUponCreationDisabled) {
                        auto cmd = create_collection_util::makeCreateCommand(
                            opCtx, ns(), request().getShardsvrCreateCollectionRequest());
                        runCreateCommandDirectClient(opCtx, ns(), cmd);
                        return CreateCollectionResponse{ShardVersion::UNSHARDED()};
                    } else if (isAlwaysUntracked(opCtx, ns(), request())) {
                        // Acquire the DDL lock to serialize with other DDL operations.
                        // A parallel coordinator for an unsplittable collection will attempt to
                        // access the collection outside of the critical section on the local
                        // catalog to check the options. We need to serialize any create
                        // collection/view to prevent wrong results
                        static constexpr StringData lockReason{"CreateCollectionUntracked"_sd};
                        const DDLLockManager::ScopedCollectionDDLLock collDDLLock{
                            opCtx, ns(), lockReason, MODE_X};
                        auto cmd = create_collection_util::makeCreateCommand(
                            opCtx, ns(), request().getShardsvrCreateCollectionRequest());
                        runCreateCommandDirectClient(opCtx, ns(), cmd);
                        return CreateCollectionResponse{ShardVersion::UNSHARDED()};
                    }
                }

                auto requestToForward = request().getShardsvrCreateCollectionRequest();
                // Validates and sets missing time-series options fields automatically. This may
                // modify the options by setting default values. Due to modifying the durable
                // format it is feature flagged to 7.1+
                if (requestToForward.getTimeseries() &&
                    gFeatureFlagValidateAndDefaultValuesForShardedTimeseries.isEnabled(
                        (*optFixedFcvRegion)->acquireFCVSnapshot())) {
                    auto timeseriesOptions = *requestToForward.getTimeseries();
                    uassertStatusOK(
                        timeseries::validateAndSetBucketingParameters(timeseriesOptions));
                    requestToForward.setTimeseries(std::move(timeseriesOptions));
                }

                if (isUnsplittable && !hasShardKey) {
                    requestToForward.setShardKey(
                        sharding_ddl_util::unsplittableCollectionShardKey().toBSON());
                }

                auto coordinatorDoc = [&] {
                    if (feature_flags::gAuthoritativeShardCollection.isEnabled(
                            (*optFixedFcvRegion)->acquireFCVSnapshot())) {
                        const DDLCoordinatorTypeEnum coordType =
                            DDLCoordinatorTypeEnum::kCreateCollection;
                        auto doc = CreateCollectionCoordinatorDocument();
                        doc.setShardingDDLCoordinatorMetadata({{ns(), coordType}});
                        doc.setShardsvrCreateCollectionRequest(requestToForward);
                        return doc.toBSON();
                    } else {
                        const DDLCoordinatorTypeEnum coordType =
                            DDLCoordinatorTypeEnum::kCreateCollectionPre80Compatible;
                        auto doc = CreateCollectionCoordinatorDocumentLegacy();
                        doc.setShardingDDLCoordinatorMetadata({{ns(), coordType}});
                        doc.setShardsvrCreateCollectionRequest(requestToForward);
                        return doc.toBSON();
                    }
                }();

                auto service = ShardingDDLCoordinatorService::getService(opCtx);
                auto instance = service->getOrCreateInstance(
                    opCtx, coordinatorDoc.copy(), false /* checkOptions */);
                try {
                    instance->checkIfOptionsConflict(coordinatorDoc);
                } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>& ex) {
                    if (isUnsplittable) {
                        LOGV2_DEBUG(8119001,
                                    1,
                                    "Found an incompatible create collection coordinator "
                                    "already running while "
                                    "attempting to create an unsharded collection. Waiting for "
                                    "it to complete and then retrying",
                                    "namespace"_attr = ns(),
                                    "error"_attr = ex);
                        // Release FCV region and wait for incompatible coordinator to finish
                        optFixedFcvRegion.reset();
                        (dynamic_pointer_cast<ShardingDDLCoordinator>(instance))
                            ->getCompletionFuture()
                            .getNoThrow(opCtx)
                            .ignore();
                        continue;
                    }

                    // If this is not a creation of an unsplittable collection just propagate the
                    // conflicting exception
                    throw;
                }

                // Release FCV region and wait for coordinator completion
                optFixedFcvRegion.reset();
                return (dynamic_pointer_cast<CreateCollectionResponseProvider>(instance))
                    ->getResult(opCtx);
            }
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
