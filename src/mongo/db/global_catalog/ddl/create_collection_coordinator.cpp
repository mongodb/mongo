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


#include "mongo/db/global_catalog/ddl/create_collection_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/audit.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/create_collection_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/notify_sharding_event_gen.h"
#include "mongo/db/global_catalog/ddl/remove_chunks_gen.h"
#include "mongo/db/global_catalog/ddl/shard_key_index_util.h"
#include "mongo/db/global_catalog/ddl/shard_key_util.h"
#include "mongo/db/global_catalog/ddl/shard_util.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_catalog_helper.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch.h"
#include "mongo/db/local_catalog/ddl/create_gen.h"
#include "mongo/db/local_catalog/ddl/list_collections_gen.h"
#include "mongo/db/local_catalog/ddl/list_indexes_gen.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding
MONGO_FAIL_POINT_DEFINE(failAtCommitCreateCollectionCoordinator);
MONGO_FAIL_POINT_DEFINE(hangBeforeCommitOnShardingCatalog);

namespace mongo {

namespace create_collection_util {
std::unique_ptr<InitialSplitPolicy> createPolicy(
    OperationContext* opCtx,
    const ShardKeyPattern& shardKeyPattern,
    const bool presplitHashedZones,
    std::vector<TagsType> tags,
    size_t numShards,
    bool collectionIsEmpty,
    bool isUnsplittable,
    boost::optional<ShardId> dataShard,
    boost::optional<std::vector<ShardId>> availableShardIds) {

    if (isUnsplittable) {
        // Unsharded collections
        uassert(ErrorCodes::InvalidOptions,
                "presplitHashedZones can't be specified for unsharded collections",
                !presplitHashedZones);

        uassert(ErrorCodes::InvalidOptions,
                "Zones should have been ignored while creting unsharded collection",
                tags.empty());

        uassert(ErrorCodes::InvalidOptions,
                "Found non-trivial shard key while creating chunk policy for unsharded collection",
                shardKeyPattern.getKeyPattern().toBSON().woCompare(
                    sharding_ddl_util::unsplittableCollectionShardKey().toBSON()) == 0);
    }

    // if unsplittable, the collection is always equivalent to a single chunk collection
    if (isUnsplittable) {
        if (dataShard) {
            return std::make_unique<SingleChunkOnShardSplitPolicy>(opCtx, *dataShard);
        } else {
            return std::make_unique<SingleChunkOnPrimarySplitPolicy>();
        }
    }

    // If 'presplitHashedZones' flag is set, we always use 'PresplitHashedZonesSplitPolicy', to make
    // sure we throw the correct assertion if further validation fails.
    if (presplitHashedZones) {
        return std::make_unique<PresplitHashedZonesSplitPolicy>(opCtx,
                                                                shardKeyPattern,
                                                                std::move(tags),
                                                                collectionIsEmpty,
                                                                std::move(availableShardIds));
    }

    //  If the collection is empty, some optimizations for the chunk distribution may be available.
    if (collectionIsEmpty) {
        if (tags.empty() && shardKeyPattern.hasHashedPrefix()) {
            // Evenly distribute chunks across shards (in combination with hashed shard keys, this
            // should increase the probability of establishing an already balanced collection).
            return std::make_unique<SplitPointsBasedSplitPolicy>(std::move(availableShardIds));
        }
        if (!tags.empty()) {
            // Enforce zone constraints.
            return std::make_unique<SingleChunkPerTagSplitPolicy>(
                opCtx, std::move(tags), std::move(availableShardIds));
        }
    }

    // Generic case.
    if (dataShard) {
        return std::make_unique<SingleChunkOnShardSplitPolicy>(opCtx, *dataShard);
    } else {
        return std::make_unique<SingleChunkOnPrimarySplitPolicy>();
    }
}

CreateCommand makeCreateCommand(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const ShardsvrCreateCollectionRequest& request) {
    // TODO SERVER-81447: build CreateCommand by simply extracting CreateCollectionRequest
    // from ShardsvrCreateCollectionRequest
    CreateCommand cmd(nss);
    CreateCollectionRequest createRequest;
    createRequest.setCapped(request.getCapped());
    createRequest.setTimeseries(request.getTimeseries());
    createRequest.setSize(request.getSize());
    createRequest.setClusteredIndex(request.getClusteredIndex());
    if (request.getCollation() && !request.getCollation()->isEmpty()) {
        auto collation = Collation::parse(*request.getCollation(), IDLParserContext("collation"));
        createRequest.setCollation(collation);
    }
    createRequest.setEncryptedFields(request.getEncryptedFields());
    createRequest.setChangeStreamPreAndPostImages(request.getChangeStreamPreAndPostImages());
    createRequest.setMax(request.getMax());
    createRequest.setFlags(request.getFlags());
    createRequest.setTemp(request.getTemp());
    createRequest.setIdIndex(request.getIdIndex());
    createRequest.setViewOn(request.getViewOn());
    createRequest.setIndexOptionDefaults(request.getIndexOptionDefaults());
    createRequest.setRecordIdsReplicated(request.getRecordIdsReplicated());
    createRequest.setExpireAfterSeconds(request.getExpireAfterSeconds());
    createRequest.setValidationAction(request.getValidationAction());
    createRequest.setValidationLevel(request.getValidationLevel());
    createRequest.setValidator(request.getValidator());
    createRequest.setPipeline(request.getPipeline());
    createRequest.setStorageEngine(request.getStorageEngine());

    cmd.setCreateCollectionRequest(createRequest);
    return cmd;
}

CollectionOptions makeCollectionOptions(OperationContext* opCtx,
                                        const ShardsvrCreateCollectionRequest& request) {
    return CollectionOptions::fromCreateCommand(
        create_collection_util::makeCreateCommand(opCtx, {}, request));
}
}  // namespace create_collection_util

namespace {

bool isUnsplittable(const ShardsvrCreateCollectionRequest& request) {
    return request.getUnsplittable();
}

bool isSharded(const ShardsvrCreateCollectionRequest& request) {
    return !isUnsplittable(request);
}

bool isTimeseries(const ShardsvrCreateCollectionRequest& request) {
    return request.getTimeseries().has_value();
}

bool isTimeseries(const boost::optional<CollectionAcquisition>& collection) {
    return collection.has_value() && collection->exists() &&
        collection->getCollectionPtr()->getTimeseriesOptions().has_value();
}

bool viewlessTimeseriesEnabled(OperationContext* opCtx) {
    return gFeatureFlagCreateViewlessTimeseriesCollections.isEnabled(
        VersionContext::getDecoration(opCtx),
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
}

void assertTimeseriesOptionsConsistency(const Collection* coll,
                                        const bool viewlessTimeseriesEnabled) {
    tassert(9934501,
            fmt::format("Encountered invalid state for target collection '{}'. ",
                        coll->ns().toStringForErrorMsg()) +
                "The collection namespace is prefixed with 'system.buckets.' but does not have "
                "associated time-series options. Please consider options to correct this, "
                "including renaming the collection or dropping the collection after inspecting "
                "and/or backing up its contents.",
            !coll->ns().isTimeseriesBucketsCollection() ||
                coll->getTimeseriesOptions().has_value());
    tassert(9934502,
            fmt::format("Encountered invalid state for target collection '{}'. ",
                        coll->ns().toStringForErrorMsg()) +
                "The collection namespace is not prefixed with 'system.buckets.' but has "
                "associated time-series options. Please consider options to correct this, "
                "including renaming the collection or dropping the collection after inspecting "
                "and/or backing up its contents.",
            viewlessTimeseriesEnabled || coll->ns().isTimeseriesBucketsCollection() ||
                !coll->getTimeseriesOptions().has_value());
}

// NOTES on the 'collation' optional parameter contained by the shardCollection() request:
// 1. It specifies the ordering criteria that will be applied when comparing chunk boundaries
// during sharding operations (such as move/mergeChunks).
// 2. As per today, the only supported value (and the one applied by default) is 'simple'
// collation.
// 3. If the collection being sharded does not exist yet, it will also be used as the ordering
// criteria to serve user queries over the shard index fields.
// 4. If an existing unsharded collection is being targeted, the original 'collation' will still
// be used to serve user queries, but the shardCollection is required to explicitly include the
// 'collation' parameter to succeed (as an acknowledge of what specified in points 1. and 2.)
// 5. In case of unsplittable collection, simply return the same collator as specified in the
// request

/*
 * Parse + serialization of the input bson to apply the Collation's class default values. This
 * ensure both sharding and local catalog to store the same collation's values.
 */
BSONObj normalizeCollation(OperationContext* opCtx, const BSONObj& collation) {
    auto collator = uassertStatusOK(
        CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
    if (!collator) {
        // In case of simple collation, makeFromBSON returns a null pointer.
        return BSONObj();
    }
    return collator->getSpec().toBSON();
}

BSONObj resolveCollationForUserQueries(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const boost::optional<BSONObj>& collationInRequest,
                                       bool isUnsplittable,
                                       bool isRegisterExistingCollectionInGlobalCatalog) {
    if (isUnsplittable && !isRegisterExistingCollectionInGlobalCatalog) {
        if (collationInRequest) {
            return normalizeCollation(opCtx, *collationInRequest);
        } else {
            return BSONObj();
        }
    }

    // Ensure the collation is valid. Currently we only allow the simple collation.
    auto requestedCollator = CollatorInterface::cloneCollator(kSimpleCollator);
    if (collationInRequest) {
        const auto& collationBson = collationInRequest.value();
        requestedCollator = uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collationBson));
        uassert(ErrorCodes::BadValue,
                str::stream() << "The collation for shardCollection must be {locale: 'simple'}, "
                              << "but found: " << collationBson,
                CollatorInterface::isSimpleCollator(requestedCollator.get()));
    }

    AutoGetCollection autoColl(opCtx, nss, MODE_IS);

    const auto actualCollator = [&]() -> const CollatorInterface* {
        const auto& coll = autoColl.getCollection();
        if (coll) {
            return coll->getDefaultCollator();
        }

        return nullptr;
    }();

    if (!requestedCollator && !actualCollator) {
        return BSONObj();
    }

    auto actualCollatorBSON = actualCollator->getSpec().toBSON();

    if (!collationInRequest && !isRegisterExistingCollectionInGlobalCatalog) {
        auto actualCollatorFilter =
            uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                ->makeFromBSON(actualCollatorBSON));
        uassert(ErrorCodes::BadValue,
                str::stream() << "If no collation was specified, the collection collation must be "
                                 "{locale: 'simple'}, "
                              << "but found: " << actualCollatorBSON,
                CollatorInterface::isSimpleCollator(actualCollatorFilter.get()));
    }
    return normalizeCollation(opCtx, actualCollatorBSON);
}

/*
 * Create the createCommand from ShardsvrCreateCollectionRequest and runs it locally
 */
Status createCollectionLocally(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const ShardsvrCreateCollectionRequest& request) {
    auto cmd = create_collection_util::makeCreateCommand(opCtx, nss, request);
    BSONObj createRes;
    DBDirectClient localClient(opCtx);
    APIParameters::get(opCtx).setInfo(cmd);
    // Forward the api check rules enforced by the client
    auto bson = cmd.toBSON();
    localClient.runCommand(nss.dbName(), bson, createRes);
    return getStatusFromCommandResult(createRes);
}

/**
 * Compares the proposed shard key with the shard key of the collection's existing zones
 * to ensure they are a legal combination.
 */
void validateShardKeyAgainstExistingZones(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const BSONObj& proposedKey,
                                          const std::vector<TagsType>& tags) {
    const AutoGetCollection coll(opCtx, nss, MODE_IS);
    for (const auto& tag : tags) {
        BSONObjIterator tagMinFields(tag.getMinKey());
        BSONObjIterator tagMaxFields(tag.getMaxKey());
        BSONObjIterator proposedFields(proposedKey);

        while (tagMinFields.more() && proposedFields.more()) {
            BSONElement tagMinKeyElement = tagMinFields.next();
            BSONElement tagMaxKeyElement = tagMaxFields.next();
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "the min and max of the existing zone " << tag.getMinKey()
                                  << " -->> " << tag.getMaxKey() << " have non-matching keys",
                    tagMinKeyElement.fieldNameStringData() ==
                        tagMaxKeyElement.fieldNameStringData());

            BSONElement proposedKeyElement = proposedFields.next();
            bool match = ((tagMinKeyElement.fieldNameStringData() ==
                           proposedKeyElement.fieldNameStringData()) &&
                          ((tagMinFields.more() && proposedFields.more()) ||
                           (!tagMinFields.more() && !proposedFields.more())));
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "the proposed shard key " << proposedKey.toString()
                                  << " does not match with the shard key of the existing zone "
                                  << tag.getMinKey() << " -->> " << tag.getMaxKey(),
                    match);

            // If the field is hashed, make sure that the min and max values are of supported type.
            uassert(
                ErrorCodes::InvalidOptions,
                str::stream() << "cannot do hash sharding with the proposed key "
                              << proposedKey.toString() << " because there exists a zone "
                              << tag.getMinKey() << " -->> " << tag.getMaxKey()
                              << " whose boundaries are not of type NumberLong, MinKey or MaxKey",
                !ShardKeyPattern::isHashedPatternEl(proposedKeyElement) ||
                    (ShardKeyPattern::isValidHashedValue(tagMinKeyElement) &&
                     ShardKeyPattern::isValidHashedValue(tagMaxKeyElement)));

            if (coll && coll->getTimeseriesOptions()) {
                const std::string controlTimeField =
                    std::string{timeseries::kControlMinFieldNamePrefix} +
                    coll->getTimeseriesOptions()->getTimeField();
                if (tagMinKeyElement.fieldNameStringData() == controlTimeField) {
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream() << "time field cannot be specified in the zone range for "
                                             "time-series collections",
                            tagMinKeyElement.type() == BSONType::minKey);
                }
                if (tagMaxKeyElement.fieldNameStringData() == controlTimeField) {
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream() << "time field cannot be specified in the zone range for "
                                             "time-series collections",
                            tagMaxKeyElement.type() == BSONType::minKey);
                }
            }
        }
    }
}

std::vector<TagsType> getTagsAndValidate(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const BSONObj& proposedKey,
                                         const bool isUnsplittable) {
    if (isUnsplittable) {
        // Tags should be ignored when creating an unsplittable collection
        return {};
    }

    // Read zone info
    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto tags = uassertStatusOK(catalogClient->getTagsForCollection(opCtx, nss));

    if (!tags.empty()) {
        validateShardKeyAgainstExistingZones(opCtx, nss, proposedKey, tags);
    }

    return tags;
}

bool checkIfCollectionIsEmpty(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const ShardId& dataShard) {
    repl::ReadConcernArgs readConcern =
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);
    const auto recipientShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, dataShard));
    FindCommandRequest findCommand(nss);
    findCommand.setLimit(1);
    findCommand.setReadConcern(readConcern);
    Shard::QueryResponse response = uassertStatusOK(recipientShard->runExhaustiveCursorCommand(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        nss.dbName(),
        findCommand.toBSON(),
        Milliseconds(-1)));
    return response.docs.empty();
}

int getNumShards(OperationContext* opCtx) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    shardRegistry->reload(opCtx);

    return shardRegistry->getNumShards(opCtx);
}

// Broadcasts a drop collection command to all shard Ids excluding the coordinator shard and (if
// specified) the dataShard.
void broadcastDropCollection(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<ShardId>& excludedDataShard,
                             const std::shared_ptr<executor::TaskExecutor>& executor,
                             const OperationSessionInfo& osi,
                             const boost::optional<UUID>& expectedUUID = boost::none) {
    const auto primaryShardId = ShardingState::get(opCtx)->shardId();

    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    // Remove primary shard from participants
    std::erase(participants, primaryShardId);
    if (excludedDataShard) {
        // Remove data shard from participants. If the collection existed prior to this operation,
        // we do not want to drop the user's collection.
        std::erase(participants, *excludedDataShard);
    }

    sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
        opCtx,
        nss,
        participants,
        executor,
        osi,
        true /* fromMigrate */,
        false /* dropSystemCollections */,
        expectedUUID);
}

/**
 * Return the collection acquisition for the target namespace.
 *
 * If the collection exists and is timeseries or
 * if we are attempting to create an unexistent timeseries collection
 * we will use the bucket namespace as target.
 *
 * In all the other cases the acquisition for the @originalNss is returned.
 *
 * This function throw en error in case the namespace already exists and is a normal view.
 *
 * TODO SERVER-101614 once 9.0 becomes last LTS we can simplify this function.
 * In fact in 9.0 we will not need to translated the originalNss to system.buckets collection
 * anymore.
 **/
CollectionAcquisition acquireTargetCollection(OperationContext* opCtx,
                                              const NamespaceString& originalNss,
                                              const ShardsvrCreateCollectionRequest& request) {

    // 1. Acquire the original Nss.
    auto collOrView = acquireCollectionOrViewMaybeLockFree(
        opCtx,
        CollectionOrViewAcquisitionRequest(originalNss,
                                           request.getCollectionUUID(),
                                           PlacementConcern::kPretendUnsharded,
                                           repl::ReadConcernArgs::get(opCtx),
                                           AcquisitionPrerequisites::OperationType::kRead,
                                           AcquisitionPrerequisites::kCanBeView));
    // In case an existing timeseries is found, return the bucket namespace.
    if (collOrView.isView()) {
        // Namespace exists in local catalog and is a view
        tassert(8119030,
                fmt::format("Found view definition on a prohibited bucket namesapce '{}'",
                            originalNss.toStringForErrorMsg()),
                !originalNss.isTimeseriesBucketsCollection());

        uassert(
            (isSharded(request) ? ErrorCodes::CommandNotSupportedOnView
                                : ErrorCodes::NamespaceExists),
            fmt::format(
                "Cannot {} collection '{}' because a view already exists with the same namespace",
                (isSharded(request) ? "shard" : "create"),
                originalNss.toStringForErrorMsg()),
            collOrView.getView().getViewDefinition().timeseries());

        return acquireCollectionMaybeLockFree(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx,
                                                    originalNss.makeTimeseriesBucketsNamespace(),
                                                    AcquisitionPrerequisites::OperationType::kRead,
                                                    request.getCollectionUUID()));
    }
    // Return an non empty acquisition.
    if (collOrView.collectionExists()) {
        return collOrView.getCollection();
    }

    // 2. originalNss is empty, search for the opposite nss: in case the originalNss is a bucket
    // seach for the view. In case it was a collection search for the bucket.
    if (originalNss.isTimeseriesBucketsCollection()) {
        auto mainColl = acquireCollectionOrViewMaybeLockFree(
            opCtx,
            CollectionOrViewAcquisitionRequest(originalNss.getTimeseriesViewNamespace(),
                                               request.getCollectionUUID(),
                                               PlacementConcern::kPretendUnsharded,
                                               repl::ReadConcernArgs::get(opCtx),
                                               AcquisitionPrerequisites::OperationType::kRead,
                                               AcquisitionPrerequisites::kCanBeView));
        if (mainColl.collectionExists()) {
            return mainColl.getCollection();
        }

    } else {
        auto bucketColl = acquireCollectionOrViewMaybeLockFree(
            opCtx,
            CollectionOrViewAcquisitionRequest(originalNss.makeTimeseriesBucketsNamespace(),
                                               request.getCollectionUUID(),
                                               PlacementConcern::kPretendUnsharded,
                                               repl::ReadConcernArgs::get(opCtx),
                                               AcquisitionPrerequisites::OperationType::kRead,
                                               AcquisitionPrerequisites::kCanBeView));
        // In case the buckets collection is found return it.
        //
        // Also if we are attempting to create an unexistent legacy timeseries with a separate
        // bucket collection we need to return the bucket collection as target namespace. This is
        // done to provide nss translation when the collection does not exists.
        if (bucketColl.collectionExists() ||
            (isTimeseries(request) && !viewlessTimeseriesEnabled(opCtx))) {
            return bucketColl.getCollection();
        }
    }

    // Return an empty acquisition. No need for translation.
    return collOrView.getCollection();
}

bool isBucketWithoutTheView(OperationContext* opCtx, const NamespaceString& targetNss) {
    // found a bucket nss but not view.
    if (targetNss.isTimeseriesBucketsCollection()) {
        auto coll = acquireCollectionOrViewMaybeLockFree(
            opCtx,
            CollectionOrViewAcquisitionRequest(targetNss.getTimeseriesViewNamespace(),
                                               PlacementConcern::kPretendUnsharded,
                                               repl::ReadConcernArgs::get(opCtx),
                                               AcquisitionPrerequisites::OperationType::kRead,
                                               AcquisitionPrerequisites::kCanBeView));
        // view must not exist
        return !coll.isView();
    }
    return false;
}

void checkLocalCatalogCollectionOptions(OperationContext* opCtx,
                                        const NamespaceString& targetNss,
                                        const ShardsvrCreateCollectionRequest& request,
                                        boost::optional<CollectionAcquisition>&& targetColl) {
    tassert(9934500,
            "expected the target collection to exist",
            targetColl.has_value() && targetColl->exists());

    assertTimeseriesOptionsConsistency(targetColl->getCollectionPtr().get(),
                                       viewlessTimeseriesEnabled(opCtx));

    if (request.getRegisterExistingCollectionInGlobalCatalog()) {
        // No need to check for collection options when registering an existing collection
        return;
    }

    if (isUnsplittable(request)) {
        // Release Collection Acquisition and all associated locks
        targetColl.reset();

        // In case we are trying to create an unsplittable collection,
        // we can directly forward the request to the local catalog.
        // The operation is idempotent and throws in case the options specified in the request
        // are incompatible with the one from the existing collection.
        uassertStatusOK(createCollectionLocally(opCtx, targetNss, request));
        return;
    }

    uassert(ErrorCodes::InvalidOptions,
            "can't shard a capped collection",
            (isSharded(request) && !targetColl->getCollectionPtr()->isCapped()) ||
                isUnsplittable(request));


    if (isTimeseries(request)) {
        uassert(ErrorCodes::InvalidOptions,
                "Found existing collection with no `timeseries` options. The `timeseries` options "
                "provided must match the ones of the existing collection.",
                isTimeseries(targetColl));

        uassert(
            ErrorCodes::InvalidOptions,
            fmt::format(
                "The `timeseries` options provided must match the ones of the existing collection. "
                "Requested {} but found {}",
                request.getTimeseries()->toBSON().toString(),
                targetColl->getCollectionPtr()->getTimeseriesOptions()->toBSON().toString()),
            timeseries::optionsAreEqual(*request.getTimeseries(),
                                        *targetColl->getCollectionPtr()->getTimeseriesOptions()));
    }
}

void checkShardingCatalogCollectionOptions(OperationContext* opCtx,
                                           const NamespaceString& targetNss,
                                           const ShardsvrCreateCollectionRequest& request,
                                           const ChunkManager& cm) {
    if (request.getRegisterExistingCollectionInGlobalCatalog()) {
        // No need for checking the sharding catalog when tracking a collection for the first time
        return;
    }

    tassert(
        8119040, "Found empty routing info when checking collection options", cm.hasRoutingTable());

    uassert(ErrorCodes::AlreadyInitialized,
            fmt::format("Collection '{}' already exists with a different 'unique' option. "
                        "Requested '{}' but found '{}'",
                        targetNss.toStringForErrorMsg(),
                        request.getUnique().value_or(false),
                        cm.isUnique()),
            cm.isUnique() == request.getUnique().value_or(false));

    if (request.getDataShard()) {

        tassert(10644537,
                "Data shard can only be specified in createCollection not in shardCollection",
                isUnsplittable(request));

        tassert(
            8119031,
            fmt::format(
                "Collection '{}' is distributed across more than one shard even if is unsplittable",
                targetNss.toStringForErrorMsg()),
            cm.getNShardsOwningChunks() == 1 || cm.isSharded());

        uassert(ErrorCodes::AlreadyInitialized,
                fmt::format("Incompatible 'dataShard' option. Collection '{}' is already sharded "
                            "with data distributed on multiple shards",
                            targetNss.toStringForErrorMsg()),
                cm.getNShardsOwningChunks() == 1);

        const auto& originalDataShard = [&] {
            std::set<ShardId> allShards;
            cm.getAllShardIds(&allShards);
            return *allShards.begin();
        }();

        uassert(ErrorCodes::AlreadyInitialized,
                fmt::format("Incompatible 'dataShard' option. Collection '{}' already exists on a "
                            "different shard. Requested shard '{}' but found '{}'",
                            targetNss.toStringForErrorMsg(),
                            request.getDataShard()->toString(),
                            originalDataShard.toString()),
                originalDataShard == *request.getDataShard());
    }

    {
        // Check collator
        const auto requestedCollator =
            resolveCollationForUserQueries(opCtx,
                                           targetNss,
                                           request.getCollation(),
                                           request.getUnsplittable(),
                                           request.getRegisterExistingCollectionInGlobalCatalog());
        const auto defaultCollator =
            cm.getDefaultCollator() ? cm.getDefaultCollator()->getSpec().toBSON() : BSONObj();
        uassert(ErrorCodes::AlreadyInitialized,
                fmt::format("Collection '{}' already exists with a different 'collator' option. "
                            "Requested {} but found {}",
                            targetNss.toStringForErrorMsg(),
                            requestedCollator.toString(),
                            defaultCollator.toString()),
                SimpleBSONObjComparator::kInstance.evaluate(defaultCollator == requestedCollator));
    }

    {
        // Check timeseries options
        uassert(ErrorCodes::AlreadyInitialized,
                fmt::format("Collection '{}' already exists and is not timeseries",
                            targetNss.toStringForErrorMsg()),
                !request.getTimeseries() || cm.getTimeseriesFields());

        if (cm.getTimeseriesFields()) {
            if (request.getTimeseries()) {
                // Both the request and the existing collection have timeseries options, check if
                // they match
                const auto& existingTimeseriesOptions =
                    cm.getTimeseriesFields()->getTimeseriesOptions();
                uassert(ErrorCodes::AlreadyInitialized,
                        fmt::format("Collection '{}' already exists with a different timeseries "
                                    "options. Requested '{}' but found '{}'",
                                    targetNss.toStringForErrorMsg(),
                                    request.getTimeseries()->toBSON().toString(),
                                    existingTimeseriesOptions.toBSON().toString()),
                        timeseries::optionsAreEqual(*request.getTimeseries(),
                                                    existingTimeseriesOptions));
            } else {
                // The collection exists and is timeseries but it was requested to create a normal
                // collection
                uassert(ErrorCodes::AlreadyInitialized,
                        fmt::format("Timeseries collection '{}' already exists",
                                    targetNss.toStringForErrorMsg()),
                        !isUnsplittable(request));
            }
        }
    }

    if (isSharded(request) && cm.isSharded()) {
        // We only check shardKey match if we are trying to shard an already sharded collection.
        //
        // In all other cases we allow the shardKey to mismatch. For instance an implicit
        // create arriving with the default unsplittable shard key should not return an error if the
        // collection is already sharded with a different shard key.

        const auto& requestKeyPattern = [&] {
            if (!cm.getTimeseriesFields()) {
                return *request.getShardKey();
            }

            // The existing collection is timeseries, thus we need to double check that shardKey
            // match the timeseries constraints
            const auto& existingTimeseriesOptions =
                cm.getTimeseriesFields()->getTimeseriesOptions();

            return uassertStatusOK(timeseries::createBucketsShardKeySpecFromTimeseriesShardKeySpec(
                existingTimeseriesOptions, *request.getShardKey()));
        }();

        uassert(ErrorCodes::AlreadyInitialized,
                fmt::format("Collection '{}' already exists with a different shard key. Requested "
                            "{} but found {}",
                            targetNss.toStringForErrorMsg(),
                            requestKeyPattern.toString(),
                            cm.getShardKeyPattern().toBSON().toString()),
                SimpleBSONObjComparator::kInstance.evaluate(requestKeyPattern ==
                                                            cm.getShardKeyPattern().toBSON()));
    }
}

boost::optional<CreateCollectionResponse> checkIfCollectionExistsWithSameOptions(
    OperationContext* opCtx,
    const ShardsvrCreateCollectionRequest& request,
    const NamespaceString& originalNss,
    bool newSessionsCollectionPath) {

    boost::optional<NamespaceString> optTargetNss;
    boost::optional<UUID> optTargetCollUUID;
    // TODO (SERVER-100309): Remove once 9.0 becomes last LTS.
    bool missingSessionsCollectionLocally = false;

    {
        // 1. Check if the collection already exists in the local catalog with same options

        boost::optional<CollectionAcquisition> targetColl =
            boost::make_optional(acquireTargetCollection(opCtx, originalNss, request));

        if (request.getRegisterExistingCollectionInGlobalCatalog()) {
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Namespace " << targetColl->nss().toStringForErrorMsg()
                                  << " not found",
                    targetColl->exists());
        }

        if (!targetColl->exists()) {
            // TODO (SERVER-100309): Remove once 9.0 becomes last LTS.
            if (newSessionsCollectionPath &&
                originalNss == NamespaceString::kLogicalSessionsNamespace) {
                optTargetNss = originalNss;
                missingSessionsCollectionLocally = true;
            } else {
                return boost::none;
            }
        } else {
            optTargetNss = targetColl->nss();
            optTargetCollUUID = targetColl->uuid();

            // Since the coordinator is holding the DDL lock for the collection we have the
            // guarantee that the collection can't be dropped concurrently.
            checkLocalCatalogCollectionOptions(
                opCtx, *optTargetNss, request, std::move(targetColl));
        }
    }

    tassert(10644538, "Expected optTargetNss to be set", optTargetNss);
    const auto& targetNss = *optTargetNss;
    tassert(10644539,
            "Expected optTargetCollUUID to be set unless creating system.sessions",
            optTargetCollUUID || missingSessionsCollectionLocally);

    // 2. Make sure we're not trying to track a temporary collection upon moveCollection
    if (request.getRegisterExistingCollectionInGlobalCatalog()) {
        DBDirectClient client(opCtx);
        const auto isTemporaryCollection =
            client.count(NamespaceString::kAggTempCollections,
                         BSON("_id" << NamespaceStringUtil::serialize(
                                  *optTargetNss, SerializationContext::stateDefault())));
        if (isTemporaryCollection) {
            // Return UNSHARDED version for the coordinator to gracefully terminate without
            // registering the collection
            return CreateCollectionResponse{ShardVersion::UNSHARDED()};
        }
    }

    // 3. Check if the collection already registered in the sharding catalog with same options

    // Force a refresh of the placement info and fetch it.
    //
    // Considering that:
    //  - we are the database primary shard
    //  - the configTime known by the database primary shard is guaranteed to be inclusive of all
    //  previously executed DDL operation on the database.
    //  - the refresh is performed on the config server using `afterClusterTime: configTime`
    //
    // We have the guaranteed that this placement information is causal-consistent with all the
    // previously executed DDL operations.
    const auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx, targetNss));

    if (!cm.hasRoutingTable()) {
        // If the sessions collection does not already exist we need to make sure that there is an
        // available shard for us to make it on.
        if (targetNss == NamespaceString::kLogicalSessionsNamespace) {
            uassert(ErrorCodes::IllegalOperation,
                    "There are no suitable shards to create the sessions collection on",
                    Grid::get(opCtx)->shardRegistry()->getNumShards(opCtx) != 0);
        }

        // The collection is not tracked in the sharding catalog. We either need to register it or
        // to shard it. Proceed with the coordinator.
        return boost::none;
    }

    if (cm.isUnsplittable() && isSharded(request)) {
        // The collection already exists but is unsplittable and we need to shard it.
        // Proceed with the coordinator
        return boost::none;
    }

    checkShardingCatalogCollectionOptions(opCtx, targetNss, request, cm);

    const bool isRequestForATimeseriesView =
        isTimeseries(request) && !originalNss.isTimeseriesBucketsCollection();
    if (isRequestForATimeseriesView && isBucketWithoutTheView(opCtx, *optTargetNss)) {
        if (request.getDataShard()) {
            // For a timeseries request, the bucket collection already exists and it's tracked but
            // the view is missing locally. Setting the dataShard is therefore illegal as the view
            // can only be created on the primary.
            // Note that the dataShard is only used for testing and this error message is mainly
            // useful for debugging purpuses only.
            uasserted(ErrorCodes::AlreadyInitialized,
                      fmt::format("Cannot specify a data shard for an implicitly created view on "
                                  "an existing timeseries collection.",
                                  targetNss.toStringForErrorMsg(),
                                  request.getDataShard()->toString()));
        }
        // For a timeseries request, the bucket collection already exists and it's tracked but the
        // view is missing locally. We need to create it. Proceed with the coordinator.
        return boost::none;
    }

    // If the sessions collection exists in the sharding catalog but not locally, we want to
    // run the coordinator so that we can create the collection locally. This is also true if we
    // have a mismatched uuid locally - we want to run the coordinator and replace the local version
    // of the collection.
    //
    // TODO (SERVER-100309): Remove once 9.0 becomes last LTS.
    if (missingSessionsCollectionLocally ||
        (targetNss == NamespaceString::kLogicalSessionsNamespace &&
         !cm.uuidMatches(*optTargetCollUUID))) {
        return boost::none;
    }

    // The collection already exists and match the requested options
    CreateCollectionResponse response(ShardVersionFactory::make(cm));
    response.setCollectionUUID(cm.getUUID());
    return response;
}

void checkCommandArguments(OperationContext* opCtx,
                           const ShardsvrCreateCollectionRequest& request,
                           const NamespaceString& originalNss) {
    LOGV2_DEBUG(5277902, 2, "Create collection checkCommandArguments", logAttrs(originalNss));

    const ShardKeyPattern shardKeyPattern{*request.getShardKey()};

    uassert(ErrorCodes::IllegalOperation,
            "Special collection '" + originalNss.toStringForErrorMsg() +
                "' can't be registered in the sharding catalog",
            !originalNss.isNamespaceAlwaysUntracked());

    // Ensure that hashed and unique are not both set.
    uassert(ErrorCodes::InvalidOptions,
            "Hashed shard keys cannot be declared unique. It's possible to ensure uniqueness on "
            "the hashed field by declaring an additional (non-hashed) unique index on the field.",
            !shardKeyPattern.isHashedPattern() || !request.getUnique().value_or(false));

    if (request.getUnsplittable()) {
        // Create unsharded collection

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "presplitHashedZones can't be specified for unsharded collection",
                !request.getPresplitHashedZones().value_or(false));

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "Found non-trivial shard key while creating unsharded collection",
                shardKeyPattern.getKeyPattern().toBSON().woCompare((BSON("_id" << 1))) == 0);
    } else {
        // Create sharded collection

        uassert(ErrorCodes::InvalidOptions,
                "dataShard can only be specified for unsplittable collections",
                !request.getDataShard());

        uassert(5731591,
                "Sharding a buckets collection is not allowed",
                !originalNss.isTimeseriesBucketsCollection());

        sharding_ddl_util::assertNamespaceLengthLimit(originalNss, request.getUnsplittable());
    }

    // The moveCollection operation adds a special flag registerExistingCollectionInGlobalCatalog to
    // teh ShardsvrCreateCollectionRequest.
    // In this case, the createCollectionCoordinator only performs the registration and skips the
    // creation of collection.
    // This flag cannot be used together with dataShard, as it may cause the data to become
    // orphaned.
    uassert(ErrorCodes::InvalidOptions,
            "dataShard and registerExistingCollectionInGlobalCatalog cannot be specified in the "
            "same request",
            !(request.getDataShard() && request.getRegisterExistingCollectionInGlobalCatalog()));
}

/**
 * Helper function to audit and log the shard collection event.
 */
void logStartCreateCollection(OperationContext* opCtx,
                              const ShardsvrCreateCollectionRequest& request,
                              const NamespaceString& originalNss) {
    BSONObjBuilder collectionDetail;
    collectionDetail.append("shardKey", *request.getShardKey());
    collectionDetail.append(
        "collection",
        NamespaceStringUtil::serialize(originalNss, SerializationContext::stateDefault()));
    collectionDetail.append("primary", ShardingState::get(opCtx)->shardId().toString());
    const auto options =
        create_collection_util::makeCollectionOptions(opCtx, request).toBSON(false);
    collectionDetail.append("options", options);
    auto operationStr =
        isUnsplittable(request) ? "createCollection.start" : "shardCollection.start";
    ShardingLogging::get(opCtx)->logChange(
        opCtx, operationStr, originalNss, collectionDetail.obj());
}

void enterCriticalSectionsOnCoordinator(OperationContext* opCtx,
                                        const BSONObj& critSecReason,
                                        const NamespaceString& originalNss) {

    auto mainNss = originalNss.isTimeseriesBucketsCollection()
        ? originalNss.getTimeseriesViewNamespace()
        : originalNss;

    ShardingRecoveryService::get(opCtx)->acquireRecoverableCriticalSectionBlockWrites(
        opCtx, mainNss, critSecReason, defaultMajorityWriteConcernDoNotUse());

    // Preventively acquire the critical section protecting the buckets namespace that the
    // creation of a timeseries collection would require.
    ShardingRecoveryService::get(opCtx)->acquireRecoverableCriticalSectionBlockWrites(
        opCtx,
        mainNss.makeTimeseriesBucketsNamespace(),
        critSecReason,
        defaultMajorityWriteConcernDoNotUse());
}

void exitCriticalSectionsOnCoordinator(OperationContext* opCtx,
                                       bool throwIfReasonDiffers,
                                       const BSONObj& critSecReason,
                                       const NamespaceString& originalNss) {

    auto mainNss = originalNss.isTimeseriesBucketsCollection()
        ? originalNss.getTimeseriesViewNamespace()
        : originalNss;

    ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
        opCtx,
        mainNss.makeTimeseriesBucketsNamespace(),
        critSecReason,
        defaultMajorityWriteConcernDoNotUse(),
        ShardingRecoveryService::FilteringMetadataClearer(),
        throwIfReasonDiffers);

    ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
        opCtx,
        mainNss,
        critSecReason,
        defaultMajorityWriteConcernDoNotUse(),
        ShardingRecoveryService::FilteringMetadataClearer(),
        throwIfReasonDiffers);
}

/*
 * Check the requested shardKey is a timefield, then convert it to a shardKey compatible for the
 * bucket collection.
 */
BSONObj validateAndTranslateShardKey(OperationContext* opCtx,
                                     const TypeCollectionTimeseriesFields& timeseriesFields,
                                     const BSONObj& shardKey) {
    shardkeyutil::validateTimeseriesShardKey(
        timeseriesFields.getTimeField(), timeseriesFields.getMetaField(), shardKey);

    return uassertStatusOK(timeseries::createBucketsShardKeySpecFromTimeseriesShardKeySpec(
        timeseriesFields.getTimeseriesOptions(), shardKey));
}

/**
 * Helper function to log the end of the shard collection event.
 */
void logEndCreateCollection(
    OperationContext* opCtx,
    const NamespaceString& originalNss,
    const boost::optional<CreateCollectionResponse>& result,
    const boost::optional<bool>& collectionEmpty,
    const boost::optional<InitialSplitPolicy::ShardCollectionConfig>& initialChunks,
    bool isUnsplittable) {
    BSONObjBuilder collectionDetail;
    if (result) {
        result->getCollectionUUID()->appendToBuilder(&collectionDetail, "uuid");
        collectionDetail.append("placementVersion", result->getCollectionVersion().toString());
    }
    if (collectionEmpty)
        collectionDetail.append("empty", *collectionEmpty);
    if (initialChunks)
        collectionDetail.appendNumber("numChunks",
                                      static_cast<long long>(initialChunks->chunks.size()));
    auto operationStr = isUnsplittable ? "createCollection.end" : "shardCollection.end";
    ShardingLogging::get(opCtx)->logChange(
        opCtx, operationStr, originalNss, collectionDetail.obj());
}

/**
 * If the optimized path can be taken, ensure the collection is already created in all the
 * participant shards.
 */
void createCollectionOnShards(OperationContext* opCtx,
                              const OperationSessionInfo& osi,
                              const boost::optional<UUID>& collectionUUID,
                              const std::vector<ShardId>& shardIds,
                              const NamespaceString& nss,
                              const OptionsAndIndexes& optionsAndIndexes) {
    std::vector<AsyncRequestsSender::Request> requests;
    auto dbPrimaryShardId = ShardingState::get(opCtx)->shardId();

    const auto& [collOptions, indexes, idIndex] = optionsAndIndexes;

    for (const auto& shard : shardIds) {
        ShardsvrCreateCollectionParticipant createCollectionParticipantRequest(nss);
        createCollectionParticipantRequest.setCollectionUUID(*collectionUUID);

        createCollectionParticipantRequest.setOptions(collOptions);
        createCollectionParticipantRequest.setIdIndex(idIndex);
        createCollectionParticipantRequest.setIndexes(indexes);
        generic_argument_util::setOperationSessionInfo(createCollectionParticipantRequest, osi);
        generic_argument_util::setMajorityWriteConcern(createCollectionParticipantRequest);

        requests.emplace_back(shard, createCollectionParticipantRequest.toBSON());
    }

    if (!requests.empty()) {
        auto responses = gatherResponses(opCtx,
                                         nss.dbName(),
                                         nss,
                                         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                         Shard::RetryPolicy::kIdempotent,
                                         requests);

        // If any shards fail to create the collection, fail the entire shardCollection command
        // (potentially leaving incomplely created sharded collection)
        for (auto&& response : responses) {
            auto shardResponse = uassertStatusOKWithContext(
                std::move(response.swResponse),
                str::stream() << "Unable to create collection " << nss.toStringForErrorMsg()
                              << " on " << response.shardId);
            auto status = getStatusFromCommandResult(shardResponse.data);
            uassertStatusOK(status.withContext(str::stream() << "Unable to create collection "
                                                             << nss.toStringForErrorMsg() << " on "
                                                             << response.shardId));

            auto wcStatus = getWriteConcernStatusFromCommandResult(shardResponse.data);
            uassertStatusOK(wcStatus.withContext(str::stream() << "Unable to create collection "
                                                               << nss.toStringForErrorMsg()
                                                               << " on " << response.shardId));
        }
    }
}

void performNoopWrite(OperationContext* opCtx) {
    const BSONObj kMsgObj = BSON("msg" << "ShardCollectionCoordinator in execution");
    writeConflictRetry(opCtx, "writeNoop", NamespaceString::kRsOplogNamespace, [&] {
        AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);
        WriteUnitOfWork wunit(opCtx);
        opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(opCtx, kMsgObj);
        wunit.commit();
    });
}

/**
 * Given the appropiate split policy, create the initial chunks.
 */
boost::optional<InitialSplitPolicy::ShardCollectionConfig> createChunks(
    OperationContext* opCtx,
    const ShardKeyPattern& shardKeyPattern,
    const boost::optional<UUID>& collectionUUID,
    const std::unique_ptr<InitialSplitPolicy>& splitPolicy,
    const NamespaceString& nss) {
    LOGV2_DEBUG(5277904, 2, "Create collection createChunks", logAttrs(nss));
    boost::optional<InitialSplitPolicy::ShardCollectionConfig> initialChunks =
        splitPolicy->createFirstChunks(
            opCtx, shardKeyPattern, {*collectionUUID, ShardingState::get(opCtx)->shardId()});

    tassert(10649100,
            "The initial split policy is expected to produce at least one chunk",
            initialChunks && !initialChunks->chunks.empty());

    return initialChunks;
}

void enterCriticalSectionsOnCoordinatorToBlockReads(OperationContext* opCtx,
                                                    const BSONObj& critSecReason,
                                                    const NamespaceString& originalNss) {

    auto mainNss = originalNss.isTimeseriesBucketsCollection()
        ? originalNss.getTimeseriesViewNamespace()
        : originalNss;

    ShardingRecoveryService::get(opCtx)->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx, mainNss, critSecReason, defaultMajorityWriteConcernDoNotUse());

    ShardingRecoveryService::get(opCtx)->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx,
        mainNss.makeTimeseriesBucketsNamespace(),
        critSecReason,
        defaultMajorityWriteConcernDoNotUse());
}

/**
 * Ensures the collection is created locally and has the appropiate shard index.
 */
boost::optional<UUID> createCollectionAndIndexes(
    OperationContext* opCtx,
    const ShardKeyPattern& shardKeyPattern,
    const ShardsvrCreateCollectionRequest& request,
    const NamespaceString& originalNss,
    const NamespaceString& translatedNss,
    const TranslatedRequestParams& translatedRequestParams,
    const ShardId& dataShard) {
    LOGV2_DEBUG(
        5277903, 2, "Create collection createCollectionAndIndexes", logAttrs(translatedNss));

    // TODO (SERVER-77915): Remove once 8.0 becomes last LTS.
    boost::optional<OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE>
        allowCollectionCreation;
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    if (!fcvSnapshot.isVersionInitialized() ||
        feature_flags::g80CollectionCreationPath.isEnabled(fcvSnapshot)) {
        allowCollectionCreation.emplace(opCtx, originalNss);
    }

    auto translatedRequest = request;
    translatedRequest.setCollation(translatedRequestParams.getCollation());

    if (const auto& timeseriesFields = translatedRequestParams.getTimeseries()) {
        translatedRequest.setTimeseries(timeseriesFields->getTimeseriesOptions());
    }

    // The creation is on the original nss in order to support timeseries creation using the
    // directly the bucket nss. In that case, we do not want to create the view. This feature is
    // currently used by mongorestore and mongosync.
    auto createStatus = createCollectionLocally(opCtx, originalNss, translatedRequest);
    if (!createStatus.isOK() && createStatus.code() == ErrorCodes::NamespaceExists) {
        LOGV2_DEBUG(5909400, 3, "Collection namespace already exists", logAttrs(originalNss));
    } else {
        uassertStatusOK(createStatus);
    }

    shardkeyutil::validateShardKeyIsNotEncrypted(opCtx, translatedNss, shardKeyPattern);

    // The shard key index for unsplittable collection { _id : 1 } is already implicitly created
    // for unsplittable collections.
    if (isUnsplittable(request)) {
        return *sharding_ddl_util::getCollectionUUID(opCtx, translatedNss);
    }
    auto indexCreated = false;
    if (request.getImplicitlyCreateIndex().value_or(true)) {
        indexCreated = shardkeyutil::validateShardKeyIndexExistsOrCreateIfPossible(
            opCtx,
            translatedNss,
            shardKeyPattern,
            translatedRequestParams.getCollation(),
            request.getUnique().value_or(false),
            request.getEnforceUniquenessCheck().value_or(true),
            shardkeyutil::ValidationBehaviorsShardCollection(opCtx, dataShard),
            (translatedRequestParams.getTimeseries()
                 ? boost::make_optional(
                       translatedRequestParams.getTimeseries()->getTimeseriesOptions())
                 : boost::none));
    } else {
        uassert(6373200,
                "Must have an index compatible with the proposed shard key",
                validShardKeyIndexExists(
                    opCtx,
                    translatedNss,
                    shardKeyPattern,
                    translatedRequestParams.getCollation(),
                    request.getUnique().value_or(false) &&
                        request.getEnforceUniquenessCheck().value_or(true),
                    shardkeyutil::ValidationBehaviorsShardCollection(opCtx, dataShard)));
    }

    auto replClientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());

    if (!indexCreated) {
        replClientInfo.setLastOpToSystemLastOpTime(opCtx);
    }
    // Wait until the index is majority written, to prevent having the collection commited to the
    // config server, but the index creation rolled backed on stepdowns.
    WriteConcernResult ignoreResult;
    uassertStatusOK(waitForWriteConcern(
        opCtx, replClientInfo.getLastOp(), defaultMajorityWriteConcernDoNotUse(), &ignoreResult));

    return *sharding_ddl_util::getCollectionUUID(opCtx, translatedNss);
}

/**
 * Does the following writes:
 * 1. Replaces the config.chunks entries for the new collection;
 * 1. Inserts the config.collections entry for the new collection
      (or updates the existing one, if currently unsplittable);
 * 3. Inserts an entry into config.placementHistory with the sublist of shards that will host
 *    the chunks of the new collection.
 */
void commit(OperationContext* opCtx,
            const std::shared_ptr<executor::TaskExecutor>& executor,
            const ShardsvrCreateCollectionRequest& request,
            boost::optional<InitialSplitPolicy::ShardCollectionConfig>& initialChunks,
            const boost::optional<UUID>& collectionUUID,
            const NamespaceString& nss,
            const std::set<ShardId>& shardsHoldingData,
            const TranslatedRequestParams& translatedRequestParams,
            std::function<OperationSessionInfo(OperationContext*)> newSessionBuilder) {
    LOGV2_DEBUG(5277906, 2, "Create collection commit", logAttrs(nss));

    if (MONGO_unlikely(nss == NamespaceString::kLogicalSessionsNamespace)) {
        tassert(ErrorCodes::IllegalOperation,
                fmt::format("The '{}' collection must always be sharded",
                            NamespaceString::kLogicalSessionsNamespace.toStringForErrorMsg()),
                isSharded(request));
    }

    if (MONGO_unlikely(failAtCommitCreateCollectionCoordinator.shouldFail())) {
        LOGV2_DEBUG(6960301, 2, "About to hit failAtCommitCreateCollectionCoordinator fail point");
        uasserted(ErrorCodes::InterruptedAtShutdown,
                  "failAtCommitCreateCollectionCoordinator fail point");
    }

    auto coll = CollectionType(nss,
                               initialChunks->collPlacementVersion().epoch(),
                               initialChunks->collPlacementVersion().getTimestamp(),
                               Date_t::now(),
                               *collectionUUID,
                               translatedRequestParams.getKeyPattern());
    if (isUnsplittable(request))
        coll.setUnsplittable(isUnsplittable(request));

    const auto& placementVersion = initialChunks->chunks.back().getVersion();

    if (const auto& timeseriesFields = translatedRequestParams.getTimeseries()) {
        coll.setTimeseriesFields(timeseriesFields);
    }

    if (auto collationBSON = translatedRequestParams.getCollation(); !collationBSON.isEmpty()) {
        coll.setDefaultCollation(collationBSON);
    }

    if (request.getUnique()) {
        coll.setUnique(*request.getUnique());
    }

    auto ops = sharding_ddl_util::getOperationsToCreateOrShardCollectionOnShardingCatalog(
        coll, initialChunks->chunks, placementVersion, shardsHoldingData);

    sharding_ddl_util::runTransactionWithStmtIdsOnShardingCatalog(
        opCtx, executor, newSessionBuilder(opCtx), std::move(ops));
}

}  // namespace
void CreateCollectionCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    // If we have two shard collections on the same namespace, then the arguments must be the same.
    const auto otherDoc = CreateCollectionCoordinatorDocument::parse(
        doc, IDLParserContext("CreateCollectionCoordinatorDocument"));

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getShardsvrCreateCollectionRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another create collection with different arguments is already "
                             "running for the same namespace: "
                          << selfReq,
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

void CreateCollectionCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

CreateCollectionResponse CreateCollectionCoordinator::getResult(OperationContext* opCtx) {
    getCompletionFuture().get(opCtx);
    tassert(10644506, "Expected _result to be initialized", _result.is_initialized());
    return *_result;
}

const NamespaceString& CreateCollectionCoordinator::nss() const {
    // Rely on the resolved request parameters to retrieve the nss to be targeted by the
    // coordinator.
    stdx::lock_guard lk{_docMutex};
    tassert(10644507,
            "Expected translatedRequestParams to be set in the coordinator document",
            _doc.getTranslatedRequestParams());
    return _doc.getTranslatedRequestParams()->getNss();
}

void CreateCollectionCoordinator::_exitCriticalSectionOnShards(
    OperationContext* opCtx,
    bool throwIfReasonDiffers,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const std::vector<ShardId>& shardIds) {
    ShardsvrParticipantBlock unblockCRUDOperationsRequest(nss());
    unblockCRUDOperationsRequest.setBlockType(CriticalSectionBlockTypeEnum::kUnblock);
    unblockCRUDOperationsRequest.setReason(_critSecReason);
    unblockCRUDOperationsRequest.setClearFilteringMetadata(true);
    unblockCRUDOperationsRequest.setThrowIfReasonDiffers(throwIfReasonDiffers);
    generic_argument_util::setMajorityWriteConcern(unblockCRUDOperationsRequest);
    generic_argument_util::setOperationSessionInfo(unblockCRUDOperationsRequest,
                                                   getNewSession(opCtx));
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
        **executor, token, unblockCRUDOperationsRequest);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, shardIds);
}

bool CreateCollectionCoordinator::_mustAlwaysMakeProgress() {
    // Any non-retryable errors before committing to the sharding catalog should cause the operation
    // to be terminated and rollbacked, triggering the cleanup procedure. On the other hand, after
    // the collection has been created on all involved shards, the operation must always make
    // forward progress.
    return _doc.getPhase() >= Phase::kCommitOnShardingCatalog;
}

ExecutorFuture<void> CreateCollectionCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            if (_doc.getPhase() < Phase::kEnterWriteCriticalSectionOnCoordinator) {
                auto opCtxHolder = makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                _checkPreconditions(opCtx);
            }
        })
        .then(_buildPhaseHandler(
            Phase::kEnterWriteCriticalSectionOnCoordinator,
            [this, anchor = shared_from_this()](auto* opCtx) {  // NOLINT
                _enterWriteCriticalSectionOnCoordinator(opCtx);

                try {
                    // check again the precoditions
                    _checkPreconditions(opCtx);
                } catch (ExceptionFor<ErrorCodes::RequestAlreadyFulfilled>& ex) {
                    LOGV2(8119050,
                          "Found that collection already exists with matching option after taking "
                          "the collection critical section");
                    exitCriticalSectionsOnCoordinator(
                        opCtx, _firstExecution, _critSecReason, originalNss());
                    throw ex;
                }
            }))
        .then(_buildPhaseHandler(Phase::kTranslateRequestParameters,
                                 [this, anchor = shared_from_this()](auto* opCtx) {  // NOLINT
                                     _translateRequestParameters(opCtx);
                                 }))
        .then(_buildPhaseHandler(
            Phase::kEnterWriteCSOnDataShardAndCheckEmpty,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                // This phase enters the write critical section on the data
                // shard if the collection already exists to prevent the
                // collection from becoming empty/non-empty after this check.
                // We then check if the collection and persist this on the
                // coordinator document to be used while creating initial
                // chunks for the collection.
                _enterWriteCriticalSectionOnDataShardAndCheckCollectionEmpty(
                    opCtx, executor, token);
            }))
        .then(_buildPhaseHandler(
            Phase::kSyncIndexesOnCoordinator,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                // TODO (SERVER-87268) skip this phase if the collection does
                // not exist.

                // If the collection exists and is located on a
                // shard other than the coordinator, the indexes on the
                // coordinator may be inaccurate since index modification
                // commands are only executed on shards that own data for a
                // collection. In this phase, we sync the indexes on the
                // coordinator with those on the data shard so that we can
                // trust the list of indexes on the coordinator. This allows us
                // to create the shard key index locally and send the full list
                // of indexes to participant shards from the coordinator.
                _syncIndexesOnCoordinator(opCtx, executor, token);
            }))
        .then(_buildPhaseHandler(Phase::kCreateCollectionOnCoordinator,
                                 [this, token, executor = executor, anchor = shared_from_this()](
                                     auto* opCtx) {  // NOLINT
                                     _createCollectionOnCoordinator(opCtx, executor, token);
                                 }))
        .then(
            _buildPhaseHandler(Phase::kEnterCriticalSection,
                               [this, token, executor = executor, anchor = shared_from_this()](
                                   auto* opCtx) { _enterCriticalSection(opCtx, executor, token); }))
        .then(_buildPhaseHandler(
            Phase::kCreateCollectionOnParticipants,
            [this, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                _createCollectionOnParticipants(opCtx, executor);
            }))
        .then(_buildPhaseHandler(
            Phase::kCommitOnShardingCatalog,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                _commitOnShardingCatalog(opCtx, executor, token);
            }))
        .then(_buildPhaseHandler(Phase::kSetPostCommitMetadata,
                                 [this, executor = executor, anchor = shared_from_this()](
                                     auto* opCtx) { _setPostCommitMetadata(opCtx, executor); }))
        .then(
            _buildPhaseHandler(Phase::kExitCriticalSection,
                               [this, token, executor = executor, anchor = shared_from_this()](
                                   auto* opCtx) { _exitCriticalSection(opCtx, executor, token); }))
        .then([this, executor = executor, anchor = shared_from_this()] {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            auto involvedShards = *_doc.getShardIds();
            auto addIfNotPresent = [&](const ShardId& shard) {
                if (std::find(involvedShards.begin(), involvedShards.end(), shard) ==
                    involvedShards.end())
                    involvedShards.push_back(shard);
            };

            // The filtering information has been cleared on all participant shards. Here we issue a
            // best effort refresh on all shards involved in the operation to install the correct
            // filtering information.
            addIfNotPresent(ShardingState::get(opCtx)->shardId());
            addIfNotPresent(*_doc.getOriginalDataShard());
            sharding_util::triggerFireAndForgetShardRefreshes(opCtx, involvedShards, nss());

            if (_firstExecution) {
                const auto& placementVersion = _initialChunks->chunks.back().getVersion();
                CreateCollectionResponse response{ShardVersionFactory::make(placementVersion)};
                response.setCollectionUUID(_uuid);
                _result = std::move(response);
            } else {
                // Recover metadata from the config server if the coordinator was resumed after
                // releasing the critical section: a migration could potentially have committed
                // changing the placement version.
                auto cm = uassertStatusOK(
                    Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx,
                                                                                            nss()));
                CreateCollectionResponse response{ShardVersionFactory::make(cm)};
                response.setCollectionUUID(cm.getUUID());
                _result = std::move(response);
            }

            logEndCreateCollection(opCtx,
                                   originalNss(),
                                   _result,
                                   _doc.getCollectionIsEmpty(),
                                   _initialChunks,
                                   _request.getUnsplittable());
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (status == ErrorCodes::RequestAlreadyFulfilled) {
                return Status::OK();
            }

            if (_doc.getPhase() < Phase::kEnterWriteCriticalSectionOnCoordinator) {
                // Early exit to not trigger the clean up procedure because the coordinator has
                // not entered to any critical section.
                return status;
            }

            const auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            // If a shard has been removed, remove it from the list of involved shards.
            if (_doc.getShardIds() && status == ErrorCodes::ShardNotFound) {
                auto involvedShardIds = *_doc.getShardIds();
                auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

                std::erase_if(involvedShardIds, [&](auto&& shard) {
                    return std::find(allShardIds.begin(), allShardIds.end(), shard) ==
                        allShardIds.end();
                });

                // If the shard being removed is the requested data shard, throw an error.
                // TODO (SERVER-83880): If the data shard was selected by the coordinator rather
                // than the user, choose a new data shard rather than throwing.
                if (_request.getDataShard() &&
                    std::find(allShardIds.begin(), allShardIds.end(), *_doc.getDataShard()) ==
                        allShardIds.end()) {
                    triggerCleanup(opCtx, status);
                    MONGO_UNREACHABLE_TASSERT(10083520);
                }

                _doc.setShardIds(std::move(involvedShardIds));
                _updateStateDocument(opCtx, CreateCollectionCoordinatorDocument(_doc));
            }

            if (!_mustAlwaysMakeProgress() && !_isRetriableErrorForDDLCoordinator(status)) {
                triggerCleanup(opCtx, status);
            }

            return status;
        });
}

void CreateCollectionCoordinator::_checkPreconditions(OperationContext* opCtx) {
    checkCommandArguments(opCtx, _request, originalNss());

    // Perform a preliminary check on whether the request may resolve into a no-op before acquiring
    // any critical section.
    auto createCollectionResponseOpt = checkIfCollectionExistsWithSameOptions(
        opCtx,
        _request,
        originalNss(),
        _doc.getCreateSessionsCollectionRemotelyOnFirstShard().value_or(false));
    if (createCollectionResponseOpt) {
        _result = createCollectionResponseOpt;
        // Launch an exception to directly jump to the end of the continuation chain
        uasserted(ErrorCodes::RequestAlreadyFulfilled,
                  str::stream() << "The collection" << originalNss().toStringForErrorMsg()
                                << "is already tracked from a past request");
    }

    // When running on a configsvr, make sure that we are indeed a data-bearing shard (config
    // shard).
    // This is important in order to fix a race where create collection for 'config.system.session',
    // which is sent to a random shard, could otherwise execute on a config server that is no longer
    // a data-bearing shard.
    // TODO (SERVER-100309): Remove this once 9.0 becomes last LTS.
    if (!_doc.getCreateSessionsCollectionRemotelyOnFirstShard() &&
        ShardingState::get(opCtx)->pollClusterRole()->has(ClusterRole::ConfigServer)) {
        const auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
        bool amIAConfigShard = std::find(allShardIds.begin(),
                                         allShardIds.end(),
                                         ShardingState::get(opCtx)->shardId()) != allShardIds.end();
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "Cannot run CreateCollectionCoordinator on a non data bearing config server",
                amIAConfigShard);
    }

    if (_request.getDataShard() && _request.getDataShard() == ShardId::kConfigServerId) {
        const auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
        bool isConfigShardAvailable =
            std::find(allShardIds.begin(), allShardIds.end(), ShardId::kConfigServerId) !=
            allShardIds.end();
        uassert(
            ErrorCodes::ShardNotFound,
            str::stream()
                << ShardId::kConfigServerId.toString()
                << " server can't be chosen as data shard because it is currently configured to "
                << "only host cluster metadata. Please use transitionFromDedicatedConfigServer "
                << "command if you also want to store application data in the config server.",
            isConfigShardAvailable);
    }
}

void CreateCollectionCoordinator::_enterWriteCriticalSectionOnCoordinator(OperationContext* opCtx) {
    logStartCreateCollection(opCtx, _request, originalNss());
    enterCriticalSectionsOnCoordinator(opCtx, _critSecReason, originalNss());
}

void CreateCollectionCoordinator::_translateRequestParameters(OperationContext* opCtx) {
    // In case timeseries options are already in the local catalog, they must be used. After the
    // checkPrecoditions phase we have the guarantee for the request's timeseries options to be
    // either empty or identical to the local catalog's ones.
    NamespaceString targetNs;
    boost::optional<TypeCollectionTimeseriesFields> optExtendedTimeseriesFields;
    {
        auto coll = acquireTargetCollection(opCtx, originalNss(), _request);
        targetNs = coll.nss();
        if (coll.exists() && coll.getCollectionPtr()->isTimeseriesCollection()) {
            // Set core timeseries options
            tassert(
                10636500,
                fmt::format(
                    "Collection '{}' is a timeseries collection but is missing timeseries options",
                    targetNs.toStringForErrorMsg()),
                coll.getCollectionPtr()->getTimeseriesOptions());
            auto extendedTimeseriesFields = TypeCollectionTimeseriesFields();
            extendedTimeseriesFields.setTimeseriesOptions(
                *(coll.getCollectionPtr()->getTimeseriesOptions()));

            if (coll.getCollectionPtr()->isNewTimeseriesWithoutView()) {
                // For viewless timeseries we project all timeseries fields to the global catalog.

                // Set mixedSchema property
                const auto& mixedSchemaBucketsState =
                    coll.getCollectionPtr()->getTimeseriesMixedSchemaBucketsState();
                tassert(
                    10636501,
                    fmt::format("Found invalid 'mixedSchemaBuckets' property for collection '{}'",
                                targetNs.toStringForErrorMsg()),
                    mixedSchemaBucketsState.isValid());
                if (!mixedSchemaBucketsState.mustConsiderMixedSchemaBucketsInReads()) {
                    extendedTimeseriesFields.setTimeseriesBucketsMayHaveMixedSchemaData(false);
                }

                if (feature_flags::gTSBucketingParametersUnchanged.isEnabled(
                        VersionContext::getDecoration(opCtx),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                    // Set bucketingParametersChanged property
                    extendedTimeseriesFields.setTimeseriesBucketingParametersHaveChanged(
                        coll.getCollectionPtr()->timeseriesBucketingParametersHaveChanged());
                }
            }

            optExtendedTimeseriesFields = std::move(extendedTimeseriesFields);
        } else if (_request.getTimeseries()) {
            // The collection does not exists so we are creating a new timeseries collection.
            auto extendedTimeseriesFields = TypeCollectionTimeseriesFields();
            extendedTimeseriesFields.setTimeseriesOptions(*_request.getTimeseries());
            if (viewlessTimeseriesEnabled(opCtx)) {
                extendedTimeseriesFields.setTimeseriesBucketsMayHaveMixedSchemaData(false);
                if (feature_flags::gTSBucketingParametersUnchanged.isEnabled(
                        VersionContext::getDecoration(opCtx),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                    extendedTimeseriesFields.setTimeseriesBucketingParametersHaveChanged(false);
                }
            }

            optExtendedTimeseriesFields = std::move(extendedTimeseriesFields);
        }
    }

    sharding_ddl_util::assertNamespaceLengthLimit(targetNs, _request.getUnsplittable());

    const auto resolvedCollator =
        resolveCollationForUserQueries(opCtx,
                                       targetNs,
                                       _request.getCollation(),
                                       _request.getUnsplittable(),
                                       _request.getRegisterExistingCollectionInGlobalCatalog());

    // Assign the correct shard key: in case of timeseries, the shard key must be converted.
    KeyPattern keyPattern;
    if (optExtendedTimeseriesFields && isSharded(_request)) {
        keyPattern = validateAndTranslateShardKey(
            opCtx, *optExtendedTimeseriesFields, *_request.getShardKey());
    } else {
        keyPattern = *_request.getShardKey();
    }
    auto translatedRequestParams = TranslatedRequestParams(targetNs, keyPattern, resolvedCollator);
    translatedRequestParams.setTimeseries(optExtendedTimeseriesFields);
    _doc.setTranslatedRequestParams(std::move(translatedRequestParams));

    const auto originalDataShard = [&]() -> ShardId {
        auto cm = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx, nss()));
        if (!cm.hasRoutingTable()) {
            // It is still possible for the collection to exist but not be tracked.
            return ShardingState::get(opCtx)->shardId();
        }
        std::set<ShardId> allShards;
        cm.getAllShardIds(&allShards);
        return *(allShards.begin());
    }();
    _doc.setOriginalDataShard(originalDataShard);
}

void CreateCollectionCoordinator::_enterWriteCriticalSectionOnDataShardAndCheckCollectionEmpty(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    // If the collection does not exist then we know that it is empty and there is no need to take
    // the write critical section on the data shard because the critical section on the coordinator
    // will prevent new collection creations.
    if (!sharding_ddl_util::getCollectionUUID(opCtx, nss())) {
        _doc.setCollectionIsEmpty(true);
        return;
    }

    // TODO (SERVER-87265) Remove this call if possible.
    if (!_firstExecution) {
        _performNoopRetryableWriteOnAllShardsAndConfigsvr(opCtx, getNewSession(opCtx), **executor);
    }

    _enterCriticalSectionOnShards(opCtx,
                                  executor,
                                  token,
                                  nss(),
                                  {*_doc.getOriginalDataShard()},
                                  CriticalSectionBlockTypeEnum::kWrites);

    const auto targetIsConfigDb = nss().dbName() == DatabaseName::kConfig;
    const auto collectionIsEmpty = std::invoke([&, this]() {
        if (targetIsConfigDb) {
            return checkIfCollectionIsEmpty(opCtx, nss(), ShardId::kConfigServerId);
        }
        return checkIfCollectionIsEmpty(opCtx, nss(), {*_doc.getOriginalDataShard()});
    });

    if (targetIsConfigDb) {
        // If this is a collection on the config db, it must be empty to be sharded.
        uassert(ErrorCodes::IllegalOperation,
                "collections in the config db must be empty to be sharded",
                collectionIsEmpty);
    }

    _doc.setCollectionIsEmpty(collectionIsEmpty);
}

void CreateCollectionCoordinator::_syncIndexesOnCoordinator(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    // If the collection does not exist or the current data shard is the coordinator, then the
    // indexes on the coordinator will already be accurate.
    bool collectionExists = [&] {
        // During the transition from running the create coordintor for config.system.sessions on
        // the first shard to running it on the config server, the collection may be sharded but the
        // collection will not exist locally on the config server. This logic will ensure that we
        // create the collection locally on the config server the first time the coordinator is run
        // on the config server.
        //
        // TODO (SERVER-100309): Remove once 9.0 becomes last LTS
        if (_doc.getCreateSessionsCollectionRemotelyOnFirstShard() &&
            nss() == NamespaceString::kLogicalSessionsNamespace) {
            const auto cri = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss()));
            return cri.hasRoutingTable();
        } else {
            return sharding_ddl_util::getCollectionUUID(opCtx, nss()).is_initialized();
        }
    }();
    if (!collectionExists || *_doc.getOriginalDataShard() == ShardingState::get(opCtx)->shardId()) {
        return;
    }

    // TODO (SERVER-87265) Remove this call if possible.
    if (!_firstExecution) {
        _performNoopRetryableWriteOnAllShardsAndConfigsvr(opCtx, getNewSession(opCtx), **executor);
    }

    auto optUuid = sharding_ddl_util::getCollectionUUID(opCtx, nss());
    // TODO (SERVER-100309): Remove sessions collection handling once 9.0 becomes last LTS.
    if (!optUuid) {
        tassert(10644508,
                "Expected the namespace to be system.sessions",
                nss() == NamespaceString::kLogicalSessionsNamespace);
        tassert(10644509,
                "Expected createSessionsCollectionRemotelyOnFirstShard to be set on the "
                "coordinator document",
                _doc.getCreateSessionsCollectionRemotelyOnFirstShard());
        // If we are in the state described above, we cannot get the uuid locally and so we need to
        // take the existing one from config.collections.
        const auto& cri = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss()));
        _uuid = cri.getChunkManager().getUUID();
    } else {
        _uuid = *optUuid;
    }

    // Get indexes from the dataShard and copy them to the coordinator.
    const auto session = getNewSession(opCtx);
    createCollectionOnShards(opCtx,
                             session,
                             _uuid,
                             {ShardingState::get(opCtx)->shardId()},
                             nss(),
                             _getCollectionOptionsAndIndexes(opCtx, *_doc.getOriginalDataShard()));
}

void CreateCollectionCoordinator::_createCollectionOnCoordinator(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    tassert(8728400,
            "Expecting translated request params to not be empty.",
            _doc.getTranslatedRequestParams());

    ShardKeyPattern shardKeyPattern(_doc.getTranslatedRequestParams()->getKeyPattern());

    if (_request.getRegisterExistingCollectionInGlobalCatalog()) {
        // If the flag is set, we're guaranteed that at this point the collection already exists
        _uuid = *sharding_ddl_util::getCollectionUUID(opCtx, nss());
    } else {
        _uuid = createCollectionAndIndexes(opCtx,
                                           shardKeyPattern,
                                           _request,
                                           originalNss(),
                                           nss(),
                                           *_doc.getTranslatedRequestParams(),
                                           *_doc.getOriginalDataShard());
    }

    if (isSharded(_request)) {
        audit::logShardCollection(opCtx->getClient(),
                                  nss(),
                                  *_request.getShardKey(),
                                  _request.getUnique().value_or(false));
    }

    auto dataShardForPolicy =
        _request.getDataShard() ? _request.getDataShard() : _doc.getOriginalDataShard();
    if (_doc.getCreateSessionsCollectionRemotelyOnFirstShard() &&
        nss() == NamespaceString::kLogicalSessionsNamespace &&
        dataShardForPolicy == ShardingState::get(opCtx)->shardId()) {
        auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
        std::sort(allShardIds.begin(), allShardIds.end());
        dataShardForPolicy = allShardIds[0];
    }
    const auto splitPolicy = create_collection_util::createPolicy(
        opCtx,
        shardKeyPattern,
        _request.getPresplitHashedZones().value_or(false),
        getTagsAndValidate(opCtx, nss(), shardKeyPattern.toBSON(), _request.getUnsplittable()),
        getNumShards(opCtx),
        *_doc.getCollectionIsEmpty(),
        _request.getUnsplittable(),
        dataShardForPolicy);

    // The chunks created by the policy will include a version that will later propagate to the
    // 'collection version' (in config.collections) and the 'DDL commit time' (in
    // config.placementHistory and placementChanged event).
    // To honor the commit protocol expected by change stream reader V2, the pre-commit notification
    // must be strictly less than the timestamp associated to such a version.
    _notifyChangeStreamReadersOnUpcomingCommit(opCtx, executor, token);
    performNoopWrite(opCtx);
    _initialChunks = createChunks(opCtx, shardKeyPattern, _uuid, splitPolicy, nss());

    // Save on doc the set of shards involved in the chunk distribution
    auto participantShards = [&]() {
        std::set<ShardId> involvedShards;
        for (const auto& chunk : _initialChunks->chunks) {
            involvedShards.emplace(chunk.getShard());
        }
        return std::vector<ShardId>(std::make_move_iterator(involvedShards.begin()),
                                    std::make_move_iterator(involvedShards.end()));
    }();
    _doc.setShardIds(std::move(participantShards));
}

void CreateCollectionCoordinator::_enterCriticalSectionOnShards(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token,
    const NamespaceString& nss,
    const std::vector<ShardId>& shardIds,
    mongo::CriticalSectionBlockTypeEnum blockType) {
    ShardsvrParticipantBlock blockCRUDOperationsRequest(nss);
    blockCRUDOperationsRequest.setBlockType(blockType);
    blockCRUDOperationsRequest.setReason(_critSecReason);

    generic_argument_util::setMajorityWriteConcern(blockCRUDOperationsRequest);
    generic_argument_util::setOperationSessionInfo(blockCRUDOperationsRequest,
                                                   getNewSession(opCtx));
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
        **executor, token, blockCRUDOperationsRequest);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, shardIds);
}

void CreateCollectionCoordinator::_enterCriticalSection(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    if (!_firstExecution) {
        _performNoopRetryableWriteOnAllShardsAndConfigsvr(opCtx, getNewSession(opCtx), **executor);
    }

    // Block reads and writes on all shards other than the dbPrimary.
    auto participants = *_doc.getShardIds();
    // Ensure the critical section is promoted to block reads on the data shard if the data shard is
    // not the coordinator and is not a participant.
    if (*_doc.getOriginalDataShard() != ShardingState::get(opCtx)->shardId() &&
        std::find(participants.begin(), participants.end(), *_doc.getOriginalDataShard()) ==
            participants.end()) {
        participants.push_back(*_doc.getOriginalDataShard());
    }
    // The critical section on the coordinator will be promoted separately below since both the
    // original and bucket namespace critical sections must be promoted.
    std::erase(participants, ShardingState::get(opCtx)->shardId());
    _enterCriticalSectionOnShards(
        opCtx, executor, token, nss(), participants, CriticalSectionBlockTypeEnum::kReadsAndWrites);

    // Promote critical sections for both original and bucket namespace on coordinator.
    enterCriticalSectionsOnCoordinatorToBlockReads(opCtx, _critSecReason, originalNss());
}

OptionsAndIndexes CreateCollectionCoordinator::_getCollectionOptionsAndIndexes(
    OperationContext* opCtx, const ShardId& fromShard) {
    BSONObj idIndex;
    BSONObjBuilder optionsBob;

    const auto destinationShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, fromShard));

    ListCollections listCollections;
    listCollections.setDbName(nss().dbName());
    listCollections.setFilter(BSON("info.uuid" << *_uuid));
    if (viewlessTimeseriesEnabled(opCtx)) {
        listCollections.setRawData(true);
    }

    auto collectionResponse =
        uassertStatusOK(destinationShard->runExhaustiveCursorCommand(
                            opCtx,
                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                            nss().dbName(),
                            listCollections.toBSON(),
                            Milliseconds(-1)))
            .docs;

    tassert(10644510,
            "Expected listCollections to return a non-empty response",
            !collectionResponse.empty());
    auto& entry = collectionResponse.front();

    if (entry["options"].isABSONObj()) {
        optionsBob.appendElements(entry["options"].Obj());
    }
    optionsBob.append(entry["info"]["uuid"]);
    if (entry["idIndex"]) {
        idIndex = entry["idIndex"].Obj().getOwned();
    }

    ListIndexes listIndexes(NamespaceStringOrUUID(nss().dbName(), *_uuid));
    if (viewlessTimeseriesEnabled(opCtx)) {
        listIndexes.setRawData(true);
    }

    std::vector<BSONObj> indexes;
    auto swIndexResponse = destinationShard->runExhaustiveCursorCommand(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        nss().dbName(),
        listIndexes.toBSON(),
        Milliseconds(-1));

    indexes = std::move(uassertStatusOK(swIndexResponse).docs);

    return {optionsBob.obj(), std::move(indexes), idIndex};
}

void CreateCollectionCoordinator::_createCollectionOnParticipants(
    OperationContext* opCtx, const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (!_firstExecution) {
        _performNoopRetryableWriteOnAllShardsAndConfigsvr(opCtx, getNewSession(opCtx), **executor);

        _uuid = sharding_ddl_util::getCollectionUUID(opCtx, nss());
    }

    // Get the list of indexes and options from the coordinator to ensure that the shard key index
    // is included in the list of indexes.
    auto session = getNewSession(opCtx);
    auto participants = *_doc.getShardIds();
    std::erase(participants, ShardingState::get(opCtx)->shardId());
    createCollectionOnShards(
        opCtx,
        session,
        _uuid,
        participants,
        nss(),
        _getCollectionOptionsAndIndexes(opCtx, ShardingState::get(opCtx)->shardId()));
}

void CreateCollectionCoordinator::_notifyChangeStreamReadersOnUpcomingCommit(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    if (_request.getUnsplittable()) {
        // Do not generate any event; this request concerns the creation of an unsplittable
        // collection.
        return;
    }

    // Adapt the original user request to the expected format, then generate the event.
    auto patchedRequest = _request;
    patchedRequest.setShardKey(_doc.getTranslatedRequestParams()->getKeyPattern().toBSON());
    // TODO SERVER-83006: remove deprecated numInitialChunks parameter. numInitialChunks should not
    // be logged by the change stream (the field has been deprecated, but it is still kept in the
    // request until it can be safely removed).
    patchedRequest.setNumInitialChunks(boost::none);
    tassert(10649101, "Uuid is expected to be already initialized", _uuid.has_value());
    CollectionSharded notification(nss(), *_uuid, patchedRequest.toBSON());

    const auto primaryShardId = ShardingState::get(opCtx)->shardId();
    const auto supportsPreciseTargeting =
        feature_flags::gFeatureFlagChangeStreamPreciseShardTargeting.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot());

    if (primaryShardId == _doc.getOriginalDataShard() || !supportsPreciseTargeting) {
        // Perform a local call to dispatch the notification through the coordinator.
        notifyChangeStreamsOnShardCollection(opCtx, notification);
        return;
    }

    // This request is targeting a pre-existing unsplittable collection that is located outside the
    // primary shard. Send a remote command to dispatch the notification.
    ShardsvrNotifyShardingEventRequest request(notify_sharding_event::kCollectionSharded,
                                               notification.toBSON());
    request.setDbName(DatabaseName::kAdmin);

    generic_argument_util::setMajorityWriteConcern(request);
    const auto osi = getNewSession(opCtx);
    generic_argument_util::setOperationSessionInfo(request, osi);

    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrNotifyShardingEventRequest>>(
        **executor, token, std::move(request));
    sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx, opts, {_doc.getOriginalDataShard().value()});
}

void CreateCollectionCoordinator::_notifyChangeStreamReadersOnPlacementChanged(
    OperationContext* opCtx,
    const Timestamp& commitTime,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    NamespacePlacementChanged notification(nss(), commitTime);
    const auto& changeStreamsNotifierShardId = _doc.getOriginalDataShard().value();
    auto buildNewSessionFn = [this](OperationContext* opCtx) {
        return getNewSession(opCtx);
    };

    sharding_ddl_util::generatePlacementChangeNotificationOnShard(
        opCtx, notification, changeStreamsNotifierShardId, buildNewSessionFn, executor, token);
}


void CreateCollectionCoordinator::_commitOnShardingCatalog(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    tassert(8728401,
            "Expecting translated request params to not be empty.",
            _doc.getTranslatedRequestParams());

    if (MONGO_unlikely(hangBeforeCommitOnShardingCatalog.shouldFail())) {
        LOGV2(8363100, "Hanging due to hangBeforeCommitOnShardingCatalog fail point");
        hangBeforeCommitOnShardingCatalog.pauseWhileSet();
    }

    if (!_firstExecution) {
        _performNoopRetryableWriteOnAllShardsAndConfigsvr(opCtx, getNewSession(opCtx), **executor);

        // Check if a previous request already created and committed the collection.
        const auto shardKeyPattern =
            ShardKeyPattern(_doc.getTranslatedRequestParams()->getKeyPattern());
        if (const auto committedSpecs =
                sharding_ddl_util::checkIfCollectionAlreadyTrackedWithOptions(
                    opCtx,
                    nss(),
                    shardKeyPattern.toBSON(),
                    _doc.getTranslatedRequestParams()->getCollation(),
                    _request.getUnique().value_or(false),
                    _request.getUnsplittable().value_or(false));
            committedSpecs.has_value()) {

            const auto commitTime =
                committedSpecs->getCollectionVersion().placementVersion().getTimestamp();
            // Ensure that the post-commit notification to change stream readers is emitted at least
            // once.
            _notifyChangeStreamReadersOnPlacementChanged(opCtx, commitTime, executor, token);

            // Checkpoint configTime in order to preserve causality of operations in case of a
            // stepdown.
            VectorClockMutable::get(opCtx)->waitForDurable().get(opCtx);
            return;
        }

        _uuid = sharding_ddl_util::getCollectionUUID(opCtx, nss());

        // Re-calculate initial chunk distribution given the set of shards with the critical section
        // taken.
        const auto& dataShardForPolicy =
            _request.getDataShard() ? _request.getDataShard() : _doc.getOriginalDataShard();
        try {
            const auto splitPolicy = create_collection_util::createPolicy(
                opCtx,
                shardKeyPattern,
                _request.getPresplitHashedZones().value_or(false),
                getTagsAndValidate(
                    opCtx, nss(), shardKeyPattern.toBSON(), _request.getUnsplittable()),
                getNumShards(opCtx),
                *_doc.getCollectionIsEmpty(),
                _request.getUnsplittable(),
                dataShardForPolicy,
                _doc.getShardIds());

            // The chunks created by the policy will include a version that will later propagate to
            // the 'collection version' (in config.collections) and the 'DDL commit time' (in
            // config.placementHistory and placementChanged event).
            // To honor the commit protocol expected by change stream reader V2, the pre-commit
            // notification must be strictly less than the timestamp associated to such a version.
            _notifyChangeStreamReadersOnUpcomingCommit(opCtx, executor, token);
            performNoopWrite(opCtx);
            _initialChunks = createChunks(opCtx, shardKeyPattern, _uuid, splitPolicy, nss());
        } catch (const DBException& ex) {
            const auto& status = ex.toStatus();
            if (!_isRetriableErrorForDDLCoordinator(status)) {
                // The error was raised by the logic that re-calculates the initial chunk
                // distribution, presumably due to the concurrent execution of a addZone and/or
                // addShard command that added new constraints which are incompatible with the
                // currently available set of shards or shard key pattern requested. Under such a
                // scenario, there is no way to correctly complete the operation; since we are
                // already within a phase where forward progress is expected, explicit clean up has
                // to be invoked.
                triggerCleanup(opCtx, ex.toStatus());
                MONGO_UNREACHABLE_TASSERT(10083521);
            }

            throw;
        }
    }

    std::set<ShardId> involvedShards;
    for (const auto& chunk : _initialChunks->chunks) {
        involvedShards.emplace(chunk.getShard());
    }

    commit(opCtx,
           **executor,
           _request,
           _initialChunks,
           _uuid,
           nss(),
           involvedShards,
           *_doc.getTranslatedRequestParams(),
           [this](OperationContext* opCtx) { return getNewSession(opCtx); });
    const auto& commitTime = _initialChunks->chunks.back().getVersion().getTimestamp();
    _notifyChangeStreamReadersOnPlacementChanged(opCtx, commitTime, executor, token);

    // Checkpoint configTime in order to preserve causality of operations in
    // case of a stepdown.
    VectorClockMutable::get(opCtx)->waitForDurable().get(opCtx);
}

void CreateCollectionCoordinator::_setPostCommitMetadata(
    OperationContext* opCtx, const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (!_firstExecution) {
        _performNoopRetryableWriteOnAllShardsAndConfigsvr(opCtx, getNewSession(opCtx), **executor);

        _uuid = sharding_ddl_util::getCollectionUUID(opCtx, nss());

        // Get the shards committed to the sharding catalog.
        std::set<ShardId> involvedShardIds;
        const auto& cm = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx, nss()));
        cm.getAllShardIds(&involvedShardIds);

        // If there are less shards involved in the operation than the ones persisted in the
        // document, implies that the collection has been created in some shards that are not owning
        // chunks.
        auto allShardIds = *_doc.getShardIds();
        std::vector<ShardId> nonInvolvedShardIds;
        std::set_difference(allShardIds.cbegin(),
                            allShardIds.cend(),
                            involvedShardIds.cbegin(),
                            involvedShardIds.cend(),
                            std::back_inserter(nonInvolvedShardIds));

        // Remove the primary shard from the list. It must always have the collection regardless if
        // it owns chunks.
        const auto primaryShardId = ShardingState::get(opCtx)->shardId();
        std::erase(nonInvolvedShardIds, primaryShardId);

        const auto session = getNewSession(opCtx);
        if (!nonInvolvedShardIds.empty()) {
            sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
                opCtx,
                nss(),
                nonInvolvedShardIds,
                **executor,
                session,
                true /* fromMigrate */,
                false /* dropSystemCollections */,
                _uuid);
        }
    }

    // If the collection being sharded was located on a data shard and then sharded with zones that
    // prevent the data shard from owning any data, we need to drop the collection metadata on the
    // data shard.
    const auto session = getNewSession(opCtx);
    auto allShardIds = *_doc.getShardIds();
    if (*_doc.getOriginalDataShard() != ShardingState::get(opCtx)->shardId() &&
        std::find(allShardIds.begin(), allShardIds.end(), *_doc.getOriginalDataShard()) ==
            allShardIds.end()) {
        tassert(10644511,
                "Expected collectionIsEmpty to be set on the coordinator document",
                *_doc.getCollectionIsEmpty());
        sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
            opCtx, nss(), {*_doc.getOriginalDataShard()}, **executor, session, true, false, _uuid);
    }
}

void CreateCollectionCoordinator::_exitCriticalSection(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    if (!_firstExecution) {
        _performNoopRetryableWriteOnAllShardsAndConfigsvr(opCtx, getNewSession(opCtx), **executor);
    }

    // Exit critical section on all shards other than the coordinator.
    auto participants = *_doc.getShardIds();
    // Ensure the critical section is released on the data shard if the data shard is not the
    // coordinator and is not a participant.
    if (*_doc.getOriginalDataShard() != ShardingState::get(opCtx)->shardId() &&
        std::find(participants.begin(), participants.end(), *_doc.getOriginalDataShard()) ==
            participants.end()) {
        participants.push_back(*_doc.getOriginalDataShard());
    }
    // The critical section on the coordinator will be released below since both the original and
    // bucket namespace critical sections need released.
    std::erase(participants, ShardingState::get(opCtx)->shardId());

    _exitCriticalSectionOnShards(
        opCtx, false /* throwIfReasonDiffers */, executor, token, participants);

    // If the coordinator successfully committed the collection during a previous execution, the
    // critical section may have already been released. In such case, it is safe to skip the release
    // if the reason does not match because a migration may have already re-acquired it.
    exitCriticalSectionsOnCoordinator(
        opCtx, _firstExecution /* throwIfReasonDiffers */, _critSecReason, originalNss());
}

ExecutorFuture<void> CreateCollectionCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, token, executor = executor, status, anchor = shared_from_this()] {
            const auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                opCtx, getNewSession(opCtx), **executor);

            if (_doc.getPhase() >= Phase::kCreateCollectionOnParticipants) {
                _uuid = sharding_ddl_util::getCollectionUUID(opCtx, nss());
                // TODO SERVER-83774: Remove the following tassert and skip the broadcast if the
                // _uuid does not exist.
                tassert(10644512, "Expected _uuid to be set", _uuid);
                const auto session = getNewSession(opCtx);
                broadcastDropCollection(opCtx,
                                        nss(),
                                        *_doc.getOriginalDataShard() /* excludedDataShard */,
                                        **executor,
                                        session,
                                        _uuid);
            }


            std::vector<ShardId> participants;
            if (_doc.getPhase() >= Phase::kEnterCriticalSection) {
                participants = *_doc.getShardIds();
                std::erase(participants, ShardingState::get(opCtx)->shardId());
            }
            if (_doc.getPhase() >= Phase::kEnterWriteCSOnDataShardAndCheckEmpty) {
                if (*_doc.getOriginalDataShard() != ShardingState::get(opCtx)->shardId() &&
                    std::find(participants.begin(),
                              participants.end(),
                              *_doc.getOriginalDataShard()) == participants.end()) {
                    participants.push_back(*_doc.getOriginalDataShard());
                }
            }

            // Exit critical section on any participant shards, including the data shard.
            if (!participants.empty()) {
                _exitCriticalSectionOnShards(
                    opCtx, true /* throwIfReasonDiffers */, executor, token, participants);
            }


            // Exit both critical sections on the coordinator
            exitCriticalSectionsOnCoordinator(
                opCtx, true /* throwIfReasonDiffers */, _critSecReason, originalNss());
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            const auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            // If a shard has been removed, remove it from the list of involved shards.
            if (_doc.getShardIds() && status == ErrorCodes::ShardNotFound) {
                auto involvedShardIds = *_doc.getShardIds();
                auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

                std::erase_if(involvedShardIds, [&](auto&& shard) {
                    return std::find(allShardIds.begin(), allShardIds.end(), shard) ==
                        allShardIds.end();
                });

                _doc.setShardIds(std::move(involvedShardIds));
                _updateStateDocument(opCtx, CreateCollectionCoordinatorDocument(_doc));
            }

            return status;
        });
}

}  // namespace mongo
