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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/create_collection_coordinator.h"
#include "mongo/db/s/shard_collection_legacy.h"
#include "mongo/db/s/sharding_ddl_50_upgrade_downgrade.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace {

using FeatureCompatibility = ServerGlobalParams::FeatureCompatibility;
using FCVersion = FeatureCompatibility::Version;

BSONObj inferCollationFromLocalCollection(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const ShardsvrCreateCollection& request) {
    auto& collation = request.getCollation().value();
    auto collator = uassertStatusOK(
        CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
    uassert(ErrorCodes::BadValue,
            str::stream() << "The collation for shardCollection must be {locale: 'simple'}, "
                          << "but found: " << collation,
            !collator);

    BSONObj res, defaultCollation, collectionOptions;
    DBDirectClient client(opCtx);

    auto allRes = client.getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));

    if (!allRes.empty())
        res = allRes.front().getOwned();

    if (!res.isEmpty() && res["options"].type() == BSONType::Object) {
        collectionOptions = res["options"].Obj();
    }

    // Get collection default collation.
    BSONElement collationElement;
    auto status =
        bsonExtractTypedField(collectionOptions, "collation", BSONType::Object, &collationElement);
    if (status.isOK()) {
        defaultCollation = collationElement.Obj();
        uassert(ErrorCodes::BadValue,
                "Default collation in collection metadata cannot be empty.",
                !defaultCollation.isEmpty());
    } else if (status != ErrorCodes::NoSuchKey) {
        uassertStatusOK(status);
    }

    return defaultCollation.getOwned();
}

// TODO (SERVER-54879): Remove this path after 5.0 branches
CreateCollectionResponse createCollectionLegacy(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const ShardsvrCreateCollection& request,
                                                const FixedFCVRegion& fcvRegion) {
    const auto dbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, nss.db()));

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "sharding not enabled for db " << nss.db(),
            dbInfo.shardingEnabled());

    if (nss.db() == NamespaceString::kConfigDb) {
        // Only allowlisted collections in config may be sharded (unless we are in test mode)
        uassert(ErrorCodes::IllegalOperation,
                "only special collections in the config db may be sharded",
                nss == NamespaceString::kLogicalSessionsNamespace);
    }

    ShardKeyPattern shardKeyPattern(request.getShardKey().value().getOwned());

    // Ensure that hashed and unique are not both set.
    uassert(ErrorCodes::InvalidOptions,
            "Hashed shard keys cannot be declared unique. It's possible to ensure uniqueness on "
            "the hashed field by declaring an additional (non-hashed) unique index on the field.",
            !shardKeyPattern.isHashedPattern() ||
                !(request.getUnique() && request.getUnique().value()));

    // Ensure that a time-series collection cannot be sharded
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "can't shard time-series collection " << nss,
            !timeseries::getTimeseriesOptions(opCtx, nss, true));

    // Ensure the namespace is valid.
    uassert(ErrorCodes::IllegalOperation,
            "can't shard system namespaces",
            !nss.isSystem() || nss == NamespaceString::kLogicalSessionsNamespace ||
                nss.isTemporaryReshardingCollection() || nss.isTimeseriesBucketsCollection());

    auto optNumInitialChunks = request.getNumInitialChunks();
    if (optNumInitialChunks) {
        // Ensure numInitialChunks is within valid bounds.
        // Cannot have more than 8192 initial chunks per shard. Setting a maximum of 1,000,000
        // chunks in total to limit the amount of memory this command consumes so there is less
        // danger of an OOM error.

        const auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
        const int maxNumInitialChunksForShards = shardIds.size() * 8192;
        const int maxNumInitialChunksTotal = 1000 * 1000;  // Arbitrary limit to memory consumption
        int numChunks = optNumInitialChunks.value();
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "numInitialChunks cannot be more than either: "
                              << maxNumInitialChunksForShards << ", 8192 * number of shards; or "
                              << maxNumInitialChunksTotal,
                numChunks >= 0 && numChunks <= maxNumInitialChunksForShards &&
                    numChunks <= maxNumInitialChunksTotal);
    }

    ShardsvrShardCollectionRequest shardsvrShardCollectionRequest;
    shardsvrShardCollectionRequest.set_shardsvrShardCollection(nss);
    shardsvrShardCollectionRequest.setKey(request.getShardKey().value());
    if (request.getUnique())
        shardsvrShardCollectionRequest.setUnique(request.getUnique().value());
    if (request.getNumInitialChunks())
        shardsvrShardCollectionRequest.setNumInitialChunks(request.getNumInitialChunks().value());
    if (request.getPresplitHashedZones())
        shardsvrShardCollectionRequest.setPresplitHashedZones(
            request.getPresplitHashedZones().value());
    if (request.getInitialSplitPoints())
        shardsvrShardCollectionRequest.setInitialSplitPoints(
            request.getInitialSplitPoints().value());

    if (request.getCollation()) {
        shardsvrShardCollectionRequest.setCollation(
            inferCollationFromLocalCollection(opCtx, nss, request));
    }

    return shardCollectionLegacy(opCtx,
                                 nss,
                                 shardsvrShardCollectionRequest.toBSON(),
                                 false /* requestIsFromCSRS */,
                                 fcvRegion);
}

CreateCollectionResponse createCollection(OperationContext* opCtx,
                                          NamespaceString nss,
                                          const ShardsvrCreateCollection& request) {
    uassert(
        ErrorCodes::NotImplemented, "create collection not implemented yet", request.getShardKey());

    auto bucketsNs = nss.makeTimeseriesBucketsNamespace();
    auto bucketsColl =
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForRead(opCtx, bucketsNs);
    CreateCollectionRequest createCmdRequest = request.getCreateCollectionRequest();

    // If the 'system.buckets' exists or 'timeseries' parameters are passed in, we know that we are
    // trying shard a timeseries collection.
    if (bucketsColl || createCmdRequest.getTimeseries()) {
        uassert(5731502,
                "Sharding a timeseries collection feature is not enabled",
                feature_flags::gFeatureFlagShardedTimeSeries.isEnabled(
                    serverGlobalParams.featureCompatibility));

        if (bucketsColl) {
            uassert(6159000,
                    str::stream() << "the collection '" << bucketsNs
                                  << "' does not have 'timeseries' options",
                    bucketsColl->getTimeseriesOptions());

            if (createCmdRequest.getTimeseries()) {
                uassert(5731500,
                        str::stream()
                            << "the 'timeseries' spec provided must match that of exists '" << nss
                            << "' collection",
                        timeseries::optionsAreEqual(*createCmdRequest.getTimeseries(),
                                                    *bucketsColl->getTimeseriesOptions()));
            } else {
                createCmdRequest.setTimeseries(bucketsColl->getTimeseriesOptions());
            }
        }
        auto timeField = createCmdRequest.getTimeseries()->getTimeField();
        auto metaField = createCmdRequest.getTimeseries()->getMetaField();
        BSONObjIterator iter{*createCmdRequest.getShardKey()};
        while (auto elem = iter.next()) {
            if (elem.fieldNameStringData() == timeField) {
                uassert(5914000,
                        str::stream() << "the time field '" << timeField
                                      << "' can be only at the end of the shard key pattern",
                        !iter.more());
            } else {
                uassert(5914001,
                        str::stream() << "only the time field or meta field can be "
                                         "part of shard key pattern",
                        metaField &&
                            (elem.fieldNameStringData() == *metaField ||
                             elem.fieldNameStringData().startsWith(*metaField + ".")));
            }
        }
        nss = bucketsNs;
        createCmdRequest.setShardKey(
            uassertStatusOK(timeseries::createBucketsShardKeySpecFromTimeseriesShardKeySpec(
                *createCmdRequest.getTimeseries(), *createCmdRequest.getShardKey())));
    }

    auto coordinatorDoc = CreateCollectionCoordinatorDocument();
    coordinatorDoc.setShardingDDLCoordinatorMetadata(
        {{std::move(nss), DDLCoordinatorTypeEnum::kCreateCollection}});
    coordinatorDoc.setCreateCollectionRequest(std::move(createCmdRequest));
    auto service = ShardingDDLCoordinatorService::getService(opCtx);
    auto createCollectionCoordinator = checked_pointer_cast<CreateCollectionCoordinator>(
        service->getOrCreateInstance(opCtx, coordinatorDoc.toBSON()));
    return createCollectionCoordinator->getResult(opCtx);
}

class ShardsvrCreateCollectionCommand final : public TypedCommand<ShardsvrCreateCollectionCommand> {
public:
    using Request = ShardsvrCreateCollection;
    using Response = CreateCollectionResponse;

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
            auto const shardingState = ShardingState::get(opCtx);
            uassertStatusOK(shardingState->canAcceptShardedCommands());

            opCtx->setAlwaysInterruptAtStepDownOrUp();

            uassert(
                ErrorCodes::InvalidOptions,
                str::stream()
                    << "_shardsvrCreateCollection must be called with majority writeConcern, got "
                    << request().toBSON(BSONObj()),
                opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            uassert(ErrorCodes::NotImplemented,
                    "Create Collection path has not been implemented",
                    request().getShardKey());

            FixedFCVRegion fcvRegion(opCtx);

            bool useNewPath = [&] {
                return fcvRegion->getVersion() == FCVersion::kVersion50 &&
                    feature_flags::gShardingFullDDLSupport.isEnabled(*fcvRegion);
            }();

            if (!useNewPath) {
                {
                    Lock::GlobalLock lock(opCtx, MODE_IX);
                    uassert(ErrorCodes::PrimarySteppedDown,
                            str::stream() << "Not primary while running " << Request::kCommandName,
                            repl::ReplicationCoordinator::get(opCtx)->getMemberState().primary());
                }

                LOGV2_DEBUG(5277911,
                            1,
                            "Running legacy create collection procedure",
                            "namespace"_attr = ns());
                return createCollectionLegacy(opCtx, ns(), request(), fcvRegion);
            }

            LOGV2_DEBUG(
                5277910, 1, "Running new create collection procedure", "namespace"_attr = ns());

            return createCollection(opCtx, ns(), request());
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} shardsvrCreateCollectionCommand;

}  // namespace
}  // namespace mongo
