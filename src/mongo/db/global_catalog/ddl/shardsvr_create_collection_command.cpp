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
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/global_catalog/ddl/create_collection_coordinator.h"
#include "mongo/db/global_catalog/ddl/create_collection_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

bool requestsShouldBeSerialized(OperationContext* opCtx,
                                const ShardsvrCreateCollectionRequest& incomingOp,
                                const ShardsvrCreateCollectionRequest& ongoingOp) {
    // The incoming requests could be either one of the following types:
    //  - implicit collection creation
    //  - explicit collection creation
    //  - register collection
    //  - shard    collection
    //
    // Implicit collection creations should serialized with all the other operations. The reason is
    // that all the implicit requests are performed as part of user write operations and we do not
    // want those to make explicit DDL operation to fail.
    //
    // For backward compatibility reason we also want to serialize explicit create requests. In fact
    // before 8.0 explicit requests were always serialized.
    //
    // 'register' and 'shard' collection are normal DDL operation, thus we should throw in case they
    // are issued concurrently with conflicting options.

    const auto& incomingIsShardColl = !incomingOp.getUnsplittable();
    const auto& ongoingIsShardColl = !ongoingOp.getUnsplittable();
    if (incomingIsShardColl && ongoingIsShardColl) {
        return false;
    }

    const auto& incomingIsRegister = incomingOp.getRegisterExistingCollectionInGlobalCatalog();
    const auto& ongoingIsRegister = ongoingOp.getRegisterExistingCollectionInGlobalCatalog();
    if (incomingIsRegister && ongoingIsRegister) {
        return false;
    }

    return true;
}

boost::optional<ScopedSetShardRole> setShardRoleToShardVersionIgnoredIfNeeded(
    OperationContext* opCtx, const NamespaceString& nss) {
    auto& oss = OperationShardingState::get(opCtx);
    if (!oss.getShardVersion(nss) && OperationShardingState::isComingFromRouter(opCtx)) {
        return ScopedSetShardRole{opCtx,
                                  nss,
                                  ShardVersionFactory::make(ChunkVersion::IGNORED()),
                                  oss.getDbVersion(nss.dbName())};
    }
    return boost::none;
}

void runCreateCommandDirectClient(OperationContext* opCtx,
                                  NamespaceString ns,
                                  const CreateCommand& cmd) {

    // Set the ShardVersion to IGNORED on the OperationShardingState for the given namespace to
    // make sure we check the critical section once the ShardVersion is checked. Note that the
    // critical section will only be checked if there is a ShardVersion attached to the
    // OperationShardingState.
    // It's important to check the critical section because it's the mechanism used to serialize the
    // current create operation with other DDL operations.
    auto shardRole = setShardRoleToShardVersionIgnoredIfNeeded(opCtx, ns);

    // Preventively set the ShardVersion to IGNORED for the timeseries buckets namespace as well.
    auto bucketsShardRole = [&]() -> boost::optional<ScopedSetShardRole> {
        if (cmd.getTimeseries() && !ns.isTimeseriesBucketsCollection()) {
            return setShardRoleToShardVersionIgnoredIfNeeded(opCtx,
                                                             ns.makeTimeseriesBucketsNamespace());
        }
        return boost::none;
    }();

    BSONObj createRes;
    DBDirectClient localClient(opCtx);
    CreateCommand c = cmd;
    APIParameters::get(opCtx).setInfo(c);
    // Forward the api check rules enforced by the client
    localClient.runCommand(ns.dbName(), c.toBSON(), createRes);
    auto createStatus = getStatusFromWriteCommandReply(createRes);
    uassertStatusOK(createStatus);
}

bool isAlwaysUntracked(OperationContext* opCtx,
                       NamespaceString&& nss,
                       const ShardsvrCreateCollection& request) {
    bool isView = request.getViewOn().has_value();

    // TODO SERVER-83713 Reconsider isFLE2StateCollection check
    // TODO SERVER-83714 Reconsider isFLE2StateCollection check
    return isView || nss.isFLE2StateCollection() || nss.isNamespaceAlwaysUntracked();
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
            bool isTrackCollectionIfExists =
                request().getRegisterExistingCollectionInGlobalCatalog();
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

            tassert(ErrorCodes::InvalidOptions,
                    "Expected `unsplittable=true` when trying to register an existing unsharded "
                    "collection",
                    !isTrackCollectionIfExists || isUnsplittable);

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
                // the feature flags are disabled or the request is for a collection type that is
                // not tracked yet or must always be local
                if (isUnsplittable) {
                    if (isAlwaysUntracked(opCtx, ns(), request())) {
                        uassert(ErrorCodes::IllegalOperation,
                                fmt::format("Tracking of collection '{}' is not supported.",
                                            ns().toStringForErrorMsg()),
                                !isTrackCollectionIfExists);
                        optFixedFcvRegion.reset();
                        return _createUntrackedCollection(opCtx);
                    }

                    bool mustTrackOnMoveCollection =
                        feature_flags::gTrackUnshardedCollectionsUponMoveCollection.isEnabled(
                            (*optFixedFcvRegion)->acquireFCVSnapshot()) &&
                        request().getRegisterExistingCollectionInGlobalCatalog();

                    if (!mustTrackOnMoveCollection && !isFromCreateUnsplittableCommand) {
                        optFixedFcvRegion.reset();
                        return _createUntrackedCollection(opCtx);
                    }
                }

                // Check whether we should create config.system.sessions from the config server on
                // the first shard using dataShard. Will throw if the feature flag is enabled and
                // we are on a shard server.
                //
                // TODO (SERVER-100309): remove once 9.0 becomes last LTS.
                bool useNewCoordinatorPathForSessionsColl =
                    _checkSessionsFeatureFlagAndClusterRole(opCtx, ns(), *optFixedFcvRegion);

                // If we are in the old world (where the config.system.sessions coordinator is run
                // on the first shard) we need to route the command to the first shard unless we
                // are the first shard.
                //
                // TODO (SERVER-100309): remove once 9.0 becomes last LTS.
                if (boost::optional<Response> remoteCreateResponse =
                        _routeSessionsCollectionCreateIfNeeded(
                            opCtx,
                            ns(),
                            useNewCoordinatorPathForSessionsColl,
                            request().getShardsvrCreateCollectionRequest()))
                    return *remoteCreateResponse;

                auto requestToForward = request().getShardsvrCreateCollectionRequest();
                // Validates and sets missing time-series options fields automatically. This may
                // modify the options by setting default values. Due to modifying the durable
                // format it is feature flagged to 7.1+
                if (requestToForward.getTimeseries()) {
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
                    const DDLCoordinatorTypeEnum coordType =
                        DDLCoordinatorTypeEnum::kCreateCollection;
                    auto doc = CreateCollectionCoordinatorDocument();
                    doc.setShardingDDLCoordinatorMetadata({{ns(), coordType}});
                    doc.setShardsvrCreateCollectionRequest(requestToForward);
                    if (useNewCoordinatorPathForSessionsColl) {
                        doc.setCreateSessionsCollectionRemotelyOnFirstShard(true);
                    }
                    return doc.toBSON();
                }();

                const auto coordinator = [&] {
                    auto service = ShardingDDLCoordinatorService::getService(opCtx);
                    return checked_pointer_cast<CreateCollectionCoordinator>(
                        service->getOrCreateInstance(opCtx,
                                                     coordinatorDoc.copy(),
                                                     *optFixedFcvRegion,
                                                     false /*checkOptions*/));
                }();
                try {
                    coordinator->checkIfOptionsConflict(coordinatorDoc);
                } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>& ex) {
                    const auto& ongoingCoordinatorReq = coordinator->getOriginalRequest();
                    const auto shouldSerializeRequests =
                        requestsShouldBeSerialized(opCtx, requestToForward, ongoingCoordinatorReq);
                    if (shouldSerializeRequests) {
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
                        coordinator->getCompletionFuture().getNoThrow(opCtx).ignore();
                        continue;
                    }

                    // If this is not a creation of an unsplittable collection just propagate the
                    // conflicting exception
                    throw;
                }

                // Release FCV region and wait for coordinator completion
                optFixedFcvRegion.reset();
                return coordinator->getResult(opCtx);
            }
        }

    private:
        CreateCollectionResponse _createUntrackedCollection(OperationContext* opCtx) {
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

        // TODO (SERVER-100309): remove once 9.0 becomes last LTS.
        bool _checkSessionsFeatureFlagAndClusterRole(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     const FixedFCVRegion& fixedFcvRegion) {
            if (ns() != NamespaceString::kLogicalSessionsNamespace)
                return false;

            // If the feature flag is enabled, we must be running on the config server
            // and we want to tell the coordinator that it must not create the
            // collection locally.
            auto clusterRole = ShardingState::get(opCtx)->pollClusterRole();
            if (feature_flags::gSessionsCollectionCoordinatorOnConfigServer.isEnabled(
                    VersionContext::getDecoration(opCtx), fixedFcvRegion->acquireFCVSnapshot())) {

                uassert(ErrorCodes::CommandNotSupported,
                        "Sessions collection can only be sharded on the config server",
                        !clusterRole->hasExclusively(ClusterRole::ShardServer));
                return true;
            }
            return false;
        }

        // TODO (SERVER-100309): remove once 9.0 becomes last LTS.
        boost::optional<Response> _routeSessionsCollectionCreateIfNeeded(
            OperationContext* opCtx,
            const NamespaceString& nss,
            bool useNewPathForSessionsColl,
            ShardsvrCreateCollectionRequest request) {
            if (ns() != NamespaceString::kLogicalSessionsNamespace || useNewPathForSessionsColl)
                return boost::none;
            // If the feature flag is disabled, we should check whether we are the first
            // shard (which can be the case in embedded config scenarios). If so, we
            // should run the coordinator normally. Otherwise we should route the
            // command appropriately.
            auto clusterRole = ShardingState::get(opCtx)->pollClusterRole();
            if (clusterRole->has(ClusterRole::ConfigServer)) {
                auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                std::sort(allShardIds.begin(), allShardIds.end());
                if (allShardIds.size() > 0 &&
                    allShardIds[0] != ShardingState::get(opCtx)->shardId()) {
                    ShardsvrCreateCollection requestToForward(ns());
                    requestToForward.setShardsvrCreateCollectionRequest(request);
                    requestToForward.setDbName(ns().dbName());
                    return cluster::createCollection(opCtx, std::move(requestToForward), true);
                }
            }
            return boost::none;
        }

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
