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


#include <algorithm>
#include <boost/cstdint.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/create_collection_coordinator.h"
#include "mongo/db/s/create_collection_coordinator_document_gen.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/participant_block_gen.h"
#include "mongo/db/s/remove_chunks_gen.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/shard_key_util.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"
#include "mongo/db/s/sharding_ddl_coordinator_gen.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/grid.h"
#include "mongo/s/index_version.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/sharding_state.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"


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
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "dataShard can only be specified in unsplittable collections",
            !dataShard || (dataShard && isUnsplittable));
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "When dataShard or unsplittable is specified, the collection must be "
                             "empty and no other option must be specified",
            !isUnsplittable ||
                (collectionIsEmpty && !presplitHashedZones && tags.empty() &&
                 shardKeyPattern.getKeyPattern().toBSON().woCompare((BSON("_id" << 1))) == 0));

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
            return std::make_unique<SplitPointsBasedSplitPolicy>(
                shardKeyPattern, numShards, std::move(availableShardIds));
        }
        if (!tags.empty()) {
            // Enforce zone constraints.
            return std::make_unique<SingleChunkPerTagSplitPolicy>(
                opCtx, std::move(tags), std::move(availableShardIds));
        }
    }

    // Generic case.
    return std::make_unique<SingleChunkOnPrimarySplitPolicy>();
}
}  // namespace create_collection_util

namespace {

struct OptionsAndIndexes {
    BSONObj options;
    std::vector<BSONObj> indexSpecs;
    BSONObj idIndexSpec;
};

OptionsAndIndexes getCollectionOptionsAndIndexes(OperationContext* opCtx,
                                                 const NamespaceStringOrUUID& nssOrUUID) {
    DBDirectClient localClient(opCtx);
    BSONObj idIndex;
    BSONObjBuilder optionsBob;

    auto all =
        localClient.getCollectionInfos(nssOrUUID.dbName(), BSON("info.uuid" << nssOrUUID.uuid()));

    // There must be a collection at this time.
    invariant(!all.empty());
    auto& entry = all.front();

    if (entry["options"].isABSONObj()) {
        optionsBob.appendElements(entry["options"].Obj());
    }
    optionsBob.append(entry["info"]["uuid"]);
    if (entry["idIndex"]) {
        idIndex = entry["idIndex"].Obj().getOwned();
    }

    auto indexSpecsList = localClient.getIndexSpecs(nssOrUUID, false, 0);

    return {optionsBob.obj(),
            std::vector<BSONObj>(std::begin(indexSpecsList), std::end(indexSpecsList)),
            idIndex};
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
BSONObj resolveCollationForUserQueries(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const boost::optional<BSONObj>& collationInRequest) {
    // Ensure the collation is valid. Currently we only allow the simple collation.
    std::unique_ptr<CollatorInterface> requestedCollator = nullptr;
    if (collationInRequest) {
        const auto& collationBson = collationInRequest.value();
        requestedCollator = uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collationBson));
        uassert(ErrorCodes::BadValue,
                str::stream() << "The collation for shardCollection must be {locale: 'simple'}, "
                              << "but found: " << collationBson,
                !requestedCollator);
    }

    AutoGetCollection autoColl(opCtx, nss, MODE_IS);

    const auto actualCollator = [&]() -> const CollatorInterface* {
        const auto& coll = autoColl.getCollection();
        if (coll) {
            uassert(
                ErrorCodes::InvalidOptions, "can't shard a capped collection", !coll->isCapped());
            return coll->getDefaultCollator();
        }

        return nullptr;
    }();

    if (!requestedCollator && !actualCollator)
        return BSONObj();

    auto actualCollation = actualCollator->getSpec();
    auto actualCollatorBSON = actualCollation.toBSON();

    if (!collationInRequest) {
        auto actualCollatorFilter =
            uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                ->makeFromBSON(actualCollatorBSON));
        uassert(ErrorCodes::BadValue,
                str::stream() << "If no collation was specified, the collection collation must be "
                                 "{locale: 'simple'}, "
                              << "but found: " << actualCollatorBSON,
                !actualCollatorFilter);
    }

    return actualCollatorBSON;
}

/**
 * Constructs the BSON specification document for the create collections command using the given
 * namespace, collation, and timeseries options.
 */
BSONObj makeCreateCommand(const NamespaceString& nss,
                          const boost::optional<Collation>& collation,
                          const TimeseriesOptions& tsOpts) {
    CreateCommand create(nss);
    CreateCollectionRequest baseRequest;
    baseRequest.setTimeseries(tsOpts);
    if (collation) {
        baseRequest.setCollation(*collation);
    }
    BSONObj commandPassthroughFields;
    create.setCreateCollectionRequest(baseRequest);
    return create.toBSON(commandPassthroughFields);
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
                    timeseries::kControlMinFieldNamePrefix.toString() +
                    coll->getTimeseriesOptions()->getTimeField();
                if (tagMinKeyElement.fieldNameStringData() == controlTimeField) {
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream() << "time field cannot be specified in the zone range for "
                                             "time-series collections",
                            tagMinKeyElement.type() == MinKey);
                }
                if (tagMaxKeyElement.fieldNameStringData() == controlTimeField) {
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream() << "time field cannot be specified in the zone range for "
                                             "time-series collections",
                            tagMaxKeyElement.type() == MinKey);
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

bool checkIfCollectionIsEmpty(OperationContext* opCtx, const NamespaceString& nss) {
    // Use find with predicate instead of count in order to ensure that the count
    // command doesn't just consult the cached metadata, which may not always be
    // correct
    DBDirectClient localClient(opCtx);
    return localClient.findOne(nss, BSONObj{}).isEmpty();
}

int getNumShards(OperationContext* opCtx) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    shardRegistry->reload(opCtx);

    return shardRegistry->getNumShards(opCtx);
}

void cleanupPartialChunksFromPreviousAttempt(OperationContext* opCtx,
                                             const UUID& uuid,
                                             const OperationSessionInfo& osi) {
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Remove the chunks matching uuid
    ConfigsvrRemoveChunks configsvrRemoveChunksCmd(uuid);
    configsvrRemoveChunksCmd.setDbName(DatabaseName::kAdmin);

    const auto swRemoveChunksResult = configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        DatabaseName::kAdmin,
        CommandHelpers::appendMajorityWriteConcern(configsvrRemoveChunksCmd.toBSON(osi.toBSON())),
        Shard::RetryPolicy::kIdempotent);

    uassertStatusOKWithContext(Shard::CommandResponse::getEffectiveStatus(swRemoveChunksResult),
                               str::stream() << "Error removing chunks matching uuid " << uuid);
}

void updateCollectionMetadataInTransaction(OperationContext* opCtx,
                                           const std::shared_ptr<executor::TaskExecutor>& executor,
                                           const std::vector<ChunkType>& chunks,
                                           const CollectionType& coll,
                                           const ChunkVersion& placementVersion,
                                           const std::set<ShardId>& shardIds,
                                           const OperationSessionInfo& osi) {
    const auto transactionChain = [&](const txn_api::TransactionClient& txnClient,
                                      ExecutorPtr txnExec) {
        auto ops = sharding_ddl_util::getOperationsToCreateOrShardCollectionOnShardingCatalog(
            coll, chunks, placementVersion, shardIds);

        StmtId statementsCounter = 0;
        for (auto&& op : ops) {
            const auto numOps = op.sizeWriteOps();
            std::vector<StmtId> statementIds(numOps);
            std::iota(statementIds.begin(), statementIds.end(), statementsCounter);
            statementsCounter += numOps;
            const auto response = txnClient.runCRUDOpSync(op, std::move(statementIds));
            uassertStatusOK(response.toStatus());
        }

        return SemiFuture<void>::makeReady();
    };

    // Ensure that this function will only return once the transaction gets majority committed
    auto wc = WriteConcernOptions{WriteConcernOptions::kMajority,
                                  WriteConcernOptions::SyncMode::UNSET,
                                  WriteConcernOptions::kNoTimeout};

    // This always runs in the shard role so should use a cluster transaction to guarantee targeting
    // the config server.
    bool useClusterTransaction = true;
    sharding_ddl_util::runTransactionOnShardingCatalog(
        opCtx, std::move(transactionChain), wc, osi, useClusterTransaction, executor);
}

void broadcastDropCollection(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const std::shared_ptr<executor::TaskExecutor>& executor,
                             const OperationSessionInfo& osi) {
    const auto primaryShardId = ShardingState::get(opCtx)->shardId();

    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    // Remove primary shard from participants
    participants.erase(std::remove(participants.begin(), participants.end(), primaryShardId),
                       participants.end());

    sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
        opCtx,
        nss,
        participants,
        executor,
        osi,
        true /* fromMigrate */,
        false /* dropSystemCollections */);
}

boost::optional<CreateCollectionResponse> checkIfCollectionAlreadyTrackedWithSameOptions(
    OperationContext* opCtx,
    const ShardsvrCreateCollectionRequest& request,
    const NamespaceString& originalNss) {
    // If the request is part of a C2C synchronisation, the check on the received UUID must be
    // performed first to honor the contract with mongosync (see SERVER-67885 for details).
    if (request.getCollectionUUID()) {
        if (AutoGetCollection stdColl{opCtx, originalNss, MODE_IS}; stdColl) {
            checkCollectionUUIDMismatch(opCtx, originalNss, *stdColl, request.getCollectionUUID());
        } else {
            // No standard collection is present on the local catalog, but the request is not yet
            // translated; a timeseries version of the requested namespace may still match the
            // requested UUID.
            auto bucketsNamespace = originalNss.makeTimeseriesBucketsNamespace();
            AutoGetCollection timeseriesColl{opCtx, bucketsNamespace, MODE_IS};
            checkCollectionUUIDMismatch(
                opCtx, originalNss, *timeseriesColl, request.getCollectionUUID());
        }
    }

    // Check if there is a standard collection that matches the original request parameters
    auto cri = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, originalNss));
    auto& cm = cri.cm;
    auto& sii = cri.sii;

    if (cm.hasRoutingTable()) {
        auto requestToShardAnUnsplittableCollection =
            !request.getUnsplittable().value_or(false) && cm.isUnsplittable();

        if (requestToShardAnUnsplittableCollection) {
            return boost::none;
        }

        auto requestMatchesExistingCollection = [&] {
            // No timeseries fields in request
            if (request.getTimeseries()) {
                return false;
            }

            if (request.getUnique().value_or(false) != cm.isUnique()) {
                return false;
            }

            if (request.getUnsplittable().value_or(false) != cm.isUnsplittable()) {
                return false;
            }

            if (SimpleBSONObjComparator::kInstance.evaluate(*request.getShardKey() !=
                                                            cm.getShardKeyPattern().toBSON())) {
                return false;
            }

            auto defaultCollator =
                cm.getDefaultCollator() ? cm.getDefaultCollator()->getSpec().toBSON() : BSONObj();
            if (SimpleBSONObjComparator::kInstance.evaluate(
                    defaultCollator !=
                    resolveCollationForUserQueries(opCtx, originalNss, request.getCollation()))) {
                return false;
            }

            return true;
        }();

        uassert(ErrorCodes::AlreadyInitialized,
                str::stream() << "collection already tracked with different options for collection "
                              << originalNss.toStringForErrorMsg(),
                requestMatchesExistingCollection);

        CreateCollectionResponse response(cri.getCollectionVersion());
        response.setCollectionUUID(cm.getUUID());
        return response;
    }

    // If the request is still unresolved, check if there is an existing TS buckets namespace that
    // may be matched by the request.
    auto bucketsNss = originalNss.makeTimeseriesBucketsNamespace();
    cri = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, bucketsNss));
    cm = cri.cm;
    sii = cri.sii;
    if (!cm.hasRoutingTable()) {
        return boost::none;
    }

    auto requestToShardAnUnsplittableCollection =
        !request.getUnsplittable().value_or(false) && cm.isUnsplittable();

    if (requestToShardAnUnsplittableCollection) {
        return boost::none;
    }

    auto requestMatchesExistingCollection = [&] {
        if (cm.isUnique() != request.getUnique().value_or(false)) {
            return false;
        }

        if (cm.isUnsplittable() != request.getUnsplittable().value_or(false)) {
            return false;
        }

        // Timeseries options match
        const auto& timeseriesOptionsOnDisk = (*cm.getTimeseriesFields()).getTimeseriesOptions();
        if (request.getTimeseries() &&
            !timeseries::optionsAreEqual(*request.getTimeseries(), timeseriesOptionsOnDisk)) {
            return false;
        }

        auto defaultCollator =
            cm.getDefaultCollator() ? cm.getDefaultCollator()->getSpec().toBSON() : BSONObj();
        if (SimpleBSONObjComparator::kInstance.evaluate(
                defaultCollator !=
                resolveCollationForUserQueries(opCtx, bucketsNss, request.getCollation()))) {
            return false;
        }

        // Same Key Pattern
        const auto& timeseriesOptions =
            request.getTimeseries() ? *request.getTimeseries() : timeseriesOptionsOnDisk;
        auto requestKeyPattern =
            uassertStatusOK(timeseries::createBucketsShardKeySpecFromTimeseriesShardKeySpec(
                timeseriesOptions, *request.getShardKey()));
        if (SimpleBSONObjComparator::kInstance.evaluate(cm.getShardKeyPattern().toBSON() !=
                                                        requestKeyPattern)) {
            return false;
        }
        return true;
    }();

    uassert(ErrorCodes::AlreadyInitialized,
            str::stream() << "collection already tracked with different options for collection "
                          << bucketsNss.toStringForErrorMsg(),
            requestMatchesExistingCollection);

    CreateCollectionResponse response(cri.getCollectionVersion());
    response.setCollectionUUID(cm.getUUID());
    return response;
}

void checkCommandArguments(OperationContext* opCtx,
                           const ShardsvrCreateCollectionRequest& request,
                           const NamespaceString& originalNss) {
    LOGV2_DEBUG(5277902, 2, "Create collection checkCommandArguments", logAttrs(originalNss));

    uassert(ErrorCodes::IllegalOperation,
            "Special collection '" + originalNss.toStringForErrorMsg() + "' cannot be sharded",
            !originalNss.isNamespaceAlwaysUntracked());

    // Ensure that hashed and unique are not both set.
    uassert(ErrorCodes::InvalidOptions,
            "Hashed shard keys cannot be declared unique. It's possible to ensure uniqueness on "
            "the hashed field by declaring an additional (non-hashed) unique index on the field.",
            !ShardKeyPattern(*request.getShardKey()).isHashedPattern() ||
                !request.getUnique().value_or(false));

    if (originalNss.dbName() == DatabaseName::kConfig) {
        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

        auto findReponse = uassertStatusOK(
            configShard->exhaustiveFindOnConfig(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                repl::ReadConcernLevel::kMajorityReadConcern,
                                                originalNss,
                                                BSONObj(),
                                                BSONObj(),
                                                1));

        auto numDocs = findReponse.docs.size();

        // If this is a collection on the config db, it must be empty to be sharded.
        uassert(ErrorCodes::IllegalOperation,
                "collections in the config db must be empty to be sharded",
                numDocs == 0);
    }
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
    ShardingLogging::get(opCtx)->logChange(
        opCtx, "shardCollection.start", originalNss, collectionDetail.obj());
}

void enterCriticalSectionsOnCoordinator(OperationContext* opCtx,
                                        const BSONObj& critSecReason,
                                        const NamespaceString& originalNss) {
    ShardingRecoveryService::get(opCtx)->acquireRecoverableCriticalSectionBlockWrites(
        opCtx, originalNss, critSecReason, ShardingCatalogClient::kMajorityWriteConcern);

    // Preventively acquire the critical section protecting the buckets namespace that the
    // creation of a timeseries collection would require.
    ShardingRecoveryService::get(opCtx)->acquireRecoverableCriticalSectionBlockWrites(
        opCtx,
        originalNss.makeTimeseriesBucketsNamespace(),
        critSecReason,
        ShardingCatalogClient::kMajorityWriteConcern);
}

void exitCriticalSectionsOnCoordinator(OperationContext* opCtx,
                                       bool throwIfReasonDiffers,
                                       const BSONObj& critSecReason,
                                       const NamespaceString& originalNss) {
    ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
        opCtx,
        originalNss,
        critSecReason,
        ShardingCatalogClient::kMajorityWriteConcern,
        throwIfReasonDiffers);

    ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
        opCtx,
        originalNss.makeTimeseriesBucketsNamespace(),
        critSecReason,
        ShardingCatalogClient::kMajorityWriteConcern,
        throwIfReasonDiffers);
}

/**
 * Updates the parameters contained in request based on the content of the local catalog and returns
 * an equivalent descriptor that may be persisted with the recovery document.
 */
TranslatedRequestParams translateRequestParameters(OperationContext* opCtx,
                                                   ShardsvrCreateCollectionRequest& request,
                                                   const NamespaceString& originalNss) {
    auto performCheckOnCollectionUUID = [opCtx, request](const NamespaceString& resolvedNss) {
        AutoGetCollection coll{
            opCtx,
            resolvedNss,
            MODE_IS,
            AutoGetCollection::Options{}.expectedUUID(request.getCollectionUUID())};
    };

    auto bucketsNs = originalNss.makeTimeseriesBucketsNamespace();
    // Hold reference to the catalog for collection lookup without locks to be safe.
    auto catalog = CollectionCatalog::get(opCtx);
    auto existingBucketsColl = catalog->lookupCollectionByNamespace(opCtx, bucketsNs);

    auto targetingStandardCollection = !request.getTimeseries() && !existingBucketsColl;

    if (targetingStandardCollection) {
        const auto& resolvedNamespace = originalNss;
        performCheckOnCollectionUUID(resolvedNamespace);
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Namespace too long. Namespace: "
                              << resolvedNamespace.toStringForErrorMsg()
                              << " Max: " << NamespaceString::MaxNsShardedCollectionLen,
                resolvedNamespace.size() <= NamespaceString::MaxNsShardedCollectionLen);
        return TranslatedRequestParams(
            resolvedNamespace,
            *request.getShardKey(),
            resolveCollationForUserQueries(opCtx, resolvedNamespace, request.getCollation()));
    }

    // The request is targeting a new or existing Timeseries collection and the request has not been
    // patched yet.
    const auto& resolvedNamespace = bucketsNs;
    performCheckOnCollectionUUID(resolvedNamespace);

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Namespace too long. Namespace: "
                          << resolvedNamespace.toStringForErrorMsg()
                          << " Max: " << NamespaceString::MaxNsShardedCollectionLen,
            resolvedNamespace.size() <= NamespaceString::MaxNsShardedCollectionLen);

    // Consolidate the related request parameters...
    auto existingTimeseriesOptions = [&bucketsNs, &existingBucketsColl] {
        if (!existingBucketsColl) {
            return boost::optional<TimeseriesOptions>();
        }

        uassert(6159000,
                str::stream() << "the collection '" << bucketsNs.toStringForErrorMsg()
                              << "' does not have 'timeseries' options",
                existingBucketsColl->getTimeseriesOptions());
        return existingBucketsColl->getTimeseriesOptions();
    }();

    if (request.getTimeseries() && existingTimeseriesOptions) {
        uassert(5731500,
                str::stream() << "the 'timeseries' spec provided must match that of exists '"
                              << originalNss.toStringForErrorMsg() << "' collection",
                timeseries::optionsAreEqual(*request.getTimeseries(), *existingTimeseriesOptions));
    } else if (!request.getTimeseries()) {
        request.setTimeseries(existingTimeseriesOptions);
    }

    if (request.getUnsplittable()) {
        return TranslatedRequestParams(
            resolvedNamespace,
            request.getShardKey().value(),
            resolveCollationForUserQueries(opCtx, resolvedNamespace, request.getCollation()));
    }

    // check that they are consistent with the requested shard key before creating the key pattern
    // object.
    auto timeFieldName = request.getTimeseries()->getTimeField();
    auto metaFieldName = request.getTimeseries()->getMetaField();
    BSONObjIterator shardKeyElems{*request.getShardKey()};
    while (auto elem = shardKeyElems.next()) {
        if (elem.fieldNameStringData() == timeFieldName) {
            uassert(5914000,
                    str::stream() << "the time field '" << timeFieldName
                                  << "' can be only at the end of the shard key pattern",
                    !shardKeyElems.more());
        } else {
            uassert(5914001,
                    str::stream() << "only the time field or meta field can be "
                                     "part of shard key pattern",
                    metaFieldName &&
                        (elem.fieldNameStringData() == *metaFieldName ||
                         elem.fieldNameStringData().startsWith(*metaFieldName + ".")));
        }
    }
    KeyPattern keyPattern(
        uassertStatusOK(timeseries::createBucketsShardKeySpecFromTimeseriesShardKeySpec(
            *request.getTimeseries(), *request.getShardKey())));
    return TranslatedRequestParams(
        resolvedNamespace,
        keyPattern,
        resolveCollationForUserQueries(opCtx, resolvedNamespace, request.getCollation()));
}

/**
 * Helper function to log the end of the shard collection event.
 */
void logEndCreateCollection(
    OperationContext* opCtx,
    const NamespaceString& originalNss,
    const boost::optional<CreateCollectionResponse>& result,
    const boost::optional<bool>& collectionEmpty,
    const boost::optional<InitialSplitPolicy::ShardCollectionConfig>& initialChunks) {
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
    ShardingLogging::get(opCtx)->logChange(
        opCtx, "shardCollection.end", originalNss, collectionDetail.obj());
}

/**
 * If the optimized path can be taken, ensure the collection is already created in all the
 * participant shards.
 */
void createCollectionOnParticipants(OperationContext* opCtx,
                                    const OperationSessionInfo& osi,
                                    const boost::optional<UUID>& collectionUUID,
                                    const std::vector<ShardId>& shardIds,
                                    const NamespaceString& nss) {
    std::vector<AsyncRequestsSender::Request> requests;
    auto dbPrimaryShardId = ShardingState::get(opCtx)->shardId();

    NamespaceStringOrUUID nssOrUUID{nss.dbName(), *collectionUUID};
    auto [collOptions, indexes, idIndex] = getCollectionOptionsAndIndexes(opCtx, nssOrUUID);

    for (const auto& shard : shardIds) {
        if (shard == dbPrimaryShardId) {
            continue;
        }

        ShardsvrCreateCollectionParticipant createCollectionParticipantRequest(nss);
        createCollectionParticipantRequest.setCollectionUUID(*collectionUUID);

        createCollectionParticipantRequest.setOptions(collOptions);
        createCollectionParticipantRequest.setIdIndex(idIndex);
        createCollectionParticipantRequest.setIndexes(indexes);

        requests.emplace_back(shard,
                              CommandHelpers::appendMajorityWriteConcern(
                                  createCollectionParticipantRequest.toBSON(osi.toBSON())));
    }

    if (!requests.empty()) {
        auto responses = gatherResponses(opCtx,
                                         nss.dbName(),
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

    // There must be at least one chunk.
    invariant(initialChunks);
    invariant(!initialChunks->chunks.empty());

    return initialChunks;
}

void enterCriticalSectionsOnCoordinatorToBlockReads(OperationContext* opCtx,
                                                    const BSONObj& critSecReason,
                                                    const NamespaceString& originalNss) {
    ShardingRecoveryService::get(opCtx)->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx, originalNss, critSecReason, ShardingCatalogClient::kMajorityWriteConcern);

    ShardingRecoveryService::get(opCtx)->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx,
        originalNss.makeTimeseriesBucketsNamespace(),
        critSecReason,
        ShardingCatalogClient::kMajorityWriteConcern);
}

/**
 * Ensures the collection is created locally and has the appropiate shard index.
 */
boost::optional<UUID> createCollectionAndIndexes(
    OperationContext* opCtx,
    const ShardKeyPattern& shardKeyPattern,
    const ShardsvrCreateCollectionRequest& request,
    const boost::optional<bool> collectionEmpty,
    const NamespaceString& nss,
    const boost::optional<mongo::TranslatedRequestParams>& translatedRequestParams) {
    LOGV2_DEBUG(5277903, 2, "Create collection createCollectionAndIndexes", logAttrs(nss));

    const auto& collationBSON = translatedRequestParams->getCollation();
    boost::optional<Collation> collation;
    if (!collationBSON.isEmpty()) {
        collation.emplace(
            Collation::parse(IDLParserContext("CreateCollectionCoordinator"), collationBSON));
    }

    // We need to implicitly create a timeseries view and underlying bucket collection.
    if (collectionEmpty && request.getTimeseries()) {
        const auto viewName = nss.getTimeseriesViewNamespace();
        auto createCmd = makeCreateCommand(viewName, collation, *request.getTimeseries());

        BSONObj createRes;
        DBDirectClient localClient(opCtx);
        localClient.runCommand(nss.dbName(), createCmd, createRes);
        auto createStatus = getStatusFromCommandResult(createRes);

        if (!createStatus.isOK() && createStatus.code() == ErrorCodes::NamespaceExists) {
            LOGV2_DEBUG(5909400, 3, "Timeseries namespace already exists", logAttrs(viewName));
        } else {
            uassertStatusOK(createStatus);
        }
    }

    // TODO (SERVER-77915): Remove once 8.0 becomes last LTS.
    boost::optional<OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE>
        allowCollectionCreation;
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    if (!fcvSnapshot.isVersionInitialized() ||
        feature_flags::gTrackUnshardedCollectionsOnShardingCatalog.isEnabled(fcvSnapshot)) {
        allowCollectionCreation.emplace(opCtx);
    }

    shardkeyutil::validateShardKeyIsNotEncrypted(opCtx, nss, shardKeyPattern);

    auto indexCreated = false;
    if (request.getImplicitlyCreateIndex().value_or(true)) {
        indexCreated = shardkeyutil::validateShardKeyIndexExistsOrCreateIfPossible(
            opCtx,
            nss,
            shardKeyPattern,
            collationBSON,
            request.getUnique().value_or(false),
            request.getEnforceUniquenessCheck().value_or(true),
            shardkeyutil::ValidationBehaviorsShardCollection(opCtx));
    } else {
        uassert(6373200,
                "Must have an index compatible with the proposed shard key",
                validShardKeyIndexExists(opCtx,
                                         nss,
                                         shardKeyPattern,
                                         collationBSON,
                                         request.getUnique().value_or(false) &&
                                             request.getEnforceUniquenessCheck().value_or(true),
                                         shardkeyutil::ValidationBehaviorsShardCollection(opCtx)));
    }

    auto replClientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());

    if (!indexCreated) {
        replClientInfo.setLastOpToSystemLastOpTime(opCtx);
    }
    // Wait until the index is majority written, to prevent having the collection commited to
    // the config server, but the index creation rolled backed on stepdowns.
    WriteConcernResult ignoreResult;
    uassertStatusOK(waitForWriteConcern(opCtx,
                                        replClientInfo.getLastOp(),
                                        ShardingCatalogClient::kMajorityWriteConcern,
                                        &ignoreResult));

    return *sharding_ddl_util::getCollectionUUID(opCtx, nss);
}

void generateCommitEventForChangeStreams(
    OperationContext* opCtx,
    const NamespaceString& translatedNss,
    const UUID& collUUID,
    const ShardsvrCreateCollectionRequest& originalRequest,
    mongo::TranslatedRequestParams& translatedRequestParams,
    CommitPhase commitPhase,
    const boost::optional<std::set<ShardId>>& shardsHostingCollection = boost::none) {
    if (originalRequest.getUnsplittable()) {
        // Do not generate any event; unsplittable collections cannot appear as sharded ones to
        // change stream users.
        return;
    }

    // Adapt the original user request to the expected format, then generate the event.
    auto patchedRequest = originalRequest;
    patchedRequest.setShardKey(translatedRequestParams.getKeyPattern().toBSON());
    // TODO SERVER-83006: remove deprecated numInitialChunks parameter.
    // numInitialChunks should not be logged by the change stream (the field has been deprecated,
    // but it is still kept in the request until it can be safely removed.
    patchedRequest.setNumInitialChunks(boost::none);

    notifyChangeStreamsOnShardCollection(opCtx,
                                         translatedNss,
                                         collUUID,
                                         patchedRequest.toBSON(),
                                         commitPhase,
                                         shardsHostingCollection);
}

/**
 * Does the following writes:
 * 1. Replaces the config.chunks entries for the new collection.
 * 1. Updates the config.collections entry for the new collection
 * 3. Inserts an entry into config.placementHistory with the sublist of shards that will host
 * one or more chunks of the new collection
 */
void commit(OperationContext* opCtx,
            const std::shared_ptr<executor::TaskExecutor>& executor,
            const ShardsvrCreateCollectionRequest& request,
            boost::optional<InitialSplitPolicy::ShardCollectionConfig>& initialChunks,
            const boost::optional<UUID>& collectionUUID,
            const NamespaceString& nss,
            const std::set<ShardId>& shardsHoldingData,
            const boost::optional<mongo::TranslatedRequestParams>& translatedRequestParams,
            std::function<OperationSessionInfo(OperationContext*)> newSessionBuilder) {
    LOGV2_DEBUG(5277906, 2, "Create collection commit", logAttrs(nss));

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
                               translatedRequestParams->getKeyPattern());
    if (request.getUnsplittable())
        coll.setUnsplittable(request.getUnsplittable());

    const auto& placementVersion = initialChunks->chunks.back().getVersion();

    if (request.getTimeseries()) {
        TypeCollectionTimeseriesFields timeseriesFields;
        timeseriesFields.setTimeseriesOptions(*request.getTimeseries());
        coll.setTimeseriesFields(std::move(timeseriesFields));
    }

    if (auto collationBSON = translatedRequestParams->getCollation(); !collationBSON.isEmpty()) {
        coll.setDefaultCollation(collationBSON);
    }

    if (request.getUnique()) {
        coll.setUnique(*request.getUnique());
    }

    updateCollectionMetadataInTransaction(opCtx,
                                          executor,
                                          initialChunks->chunks,
                                          coll,
                                          placementVersion,
                                          shardsHoldingData,
                                          newSessionBuilder(opCtx));
}

}  // namespace

void CreateCollectionCoordinatorLegacy::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

CreateCollectionResponse CreateCollectionCoordinatorLegacy::getResult(OperationContext* opCtx) {
    getCompletionFuture().get(opCtx);
    invariant(_result.is_initialized());
    return *_result;
}

const NamespaceString& CreateCollectionCoordinatorLegacy::nss() const {
    // Rely on the resolved request parameters to retrieve the nss to be targeted by the
    // coordinator.
    stdx::lock_guard lk{_docMutex};
    invariant(_doc.getTranslatedRequestParams());
    return _doc.getTranslatedRequestParams()->getNss();
}

void CreateCollectionCoordinatorLegacy::checkIfOptionsConflict(const BSONObj& doc) const {
    // If we have two shard collections on the same namespace, then the arguments must be the same.
    const auto otherDoc = CreateCollectionCoordinatorDocumentLegacy::parse(
        IDLParserContext("CreateCollectionCoordinatorDocumentLegacy"), doc);

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getShardsvrCreateCollectionRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another create collection with different arguments is already "
                             "running for the same namespace: "
                          << selfReq,
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

ExecutorFuture<void> CreateCollectionCoordinatorLegacy::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            if (_doc.getPhase() < Phase::kCommit) {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                checkCommandArguments(opCtx, _request, originalNss());
                // Perform a preliminary check on whether the request may resolve into a no-op
                // before acquiring any critical section.
                auto createCollectionResponseOpt =
                    checkIfCollectionAlreadyTrackedWithSameOptions(opCtx, _request, originalNss());
                if (createCollectionResponseOpt) {
                    _result = createCollectionResponseOpt;
                    // Launch an exception to directly jump to the end of the continuation chain
                    uasserted(ErrorCodes::RequestAlreadyFulfilled,
                              str::stream()
                                  << "The collection" << originalNss().toStringForErrorMsg()
                                  << "is already tracked from a past request");
                }
            }
        })
        .then(_buildPhaseHandler(
            Phase::kCommit,
            [this, executor = executor, token, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                if (!_firstExecution) {
                    // Perform a noop write on the participants in order to advance the txnNumber
                    // for this coordinator's lsid so that requests with older txnNumbers can no
                    // longer execute.
                    //
                    // Additionally we want to perform a majority write on the CSRS to ensure that
                    // all the subsequent reads will see all the writes performed from a previous
                    // execution of this coordinator.
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getNewSession(opCtx), **executor);

                    if (_doc.getTranslatedRequestParams()) {

                        const auto shardKeyPattern =
                            ShardKeyPattern(_doc.getTranslatedRequestParams()->getKeyPattern());
                        const auto& collation = _doc.getTranslatedRequestParams()->getCollation();

                        // Check if the collection was already sharded by a past request
                        if (auto createCollectionResponseOpt =
                                sharding_ddl_util::checkIfCollectionAlreadyTrackedWithOptions(
                                    opCtx,
                                    nss(),
                                    shardKeyPattern.toBSON(),
                                    collation,
                                    _request.getUnique().value_or(false),
                                    _request.getUnsplittable().value_or(false))) {

                            // A previous request already created and committed the collection
                            // but there was a stepdown after the commit.

                            // Ensure that the change stream event gets emitted at least once.
                            generateCommitEventForChangeStreams(
                                opCtx,
                                nss(),
                                *createCollectionResponseOpt->getCollectionUUID(),
                                _request,
                                *_doc.getTranslatedRequestParams(),
                                CommitPhase::kSuccessful);

                            // The critical section might have been taken by a migration, we force
                            // to skip the invariant check and we do nothing in case it was taken.
                            exitCriticalSectionsOnCoordinator(opCtx,
                                                              false /* throwIfReasonDiffers */,
                                                              _critSecReason,
                                                              originalNss());

                            _result = createCollectionResponseOpt;
                            return;
                        }

                        // Legacy cleanup from when we were not committing the chunks and collection
                        // entry transactionally.
                        auto uuid = sharding_ddl_util::getCollectionUUID(opCtx, nss());
                        auto cri = uassertStatusOK(
                            Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(
                                opCtx, nss()));
                        // If the collection can be found locally but is not yet tracked on the
                        // config server, then we clean up the config.chunks collection.
                        if (uuid && !cri.cm.hasRoutingTable()) {
                            LOGV2_DEBUG(5458704,
                                        1,
                                        "Removing partial changes from previous run",
                                        logAttrs(nss()));

                            cleanupPartialChunksFromPreviousAttempt(
                                opCtx, *uuid, getNewSession(opCtx));

                            broadcastDropCollection(opCtx, nss(), **executor, getNewSession(opCtx));
                        }
                    }
                }

                logStartCreateCollection(opCtx, _request, originalNss());
                enterCriticalSectionsOnCoordinator(opCtx, _critSecReason, originalNss());

                // Translate request parameters and persist them in the coordiantor document
                _doc.setTranslatedRequestParams(
                    translateRequestParameters(opCtx, _request, originalNss()));
                _updateStateDocument(opCtx, CreateCollectionCoordinatorDocumentLegacy(_doc));

                ShardKeyPattern shardKeyPattern(_doc.getTranslatedRequestParams()->getKeyPattern());
                _collectionEmpty = checkIfCollectionIsEmpty(opCtx, nss());
                _splitPolicy = create_collection_util::createPolicy(
                    opCtx,
                    shardKeyPattern,
                    _request.getPresplitHashedZones().value_or(false),
                    getTagsAndValidate(
                        opCtx, nss(), shardKeyPattern.toBSON(), false /* isUnsplittable */),
                    getNumShards(opCtx),
                    *_collectionEmpty,
                    _request.getUnsplittable(),
                    _request.getDataShard());


                _collectionUUID = createCollectionAndIndexes(opCtx,
                                                             shardKeyPattern,
                                                             _request,
                                                             _collectionEmpty,
                                                             nss(),
                                                             _doc.getTranslatedRequestParams());

                audit::logShardCollection(opCtx->getClient(),
                                          nss(),
                                          *_request.getShardKey(),
                                          _request.getUnique().value_or(false));

                _initialChunks =
                    createChunks(opCtx, shardKeyPattern, _collectionUUID, _splitPolicy, nss());

                std::set<ShardId> involvedShards;
                for (const auto& chunk : _initialChunks->chunks) {
                    involvedShards.emplace(chunk.getShard());
                }

                // Block reads/writes from here on if we need to create the collection on other
                // shards, this way we prevent reads/writes that should be redirected to another
                // shard
                enterCriticalSectionsOnCoordinatorToBlockReads(
                    opCtx, _critSecReason, originalNss());

                createCollectionOnParticipants(
                    opCtx,
                    getNewSession(opCtx),
                    _collectionUUID,
                    std::vector<ShardId>{std::make_move_iterator(involvedShards.begin()),
                                         std::make_move_iterator(involvedShards.end())},
                    nss());

                try {
                    generateCommitEventForChangeStreams(opCtx,
                                                        nss(),
                                                        *_collectionUUID,
                                                        _request,
                                                        *_doc.getTranslatedRequestParams(),
                                                        CommitPhase::kPrepare,
                                                        involvedShards);

                    commit(opCtx,
                           **executor,
                           _request,
                           _initialChunks,
                           _collectionUUID,
                           nss(),
                           involvedShards,
                           _doc.getTranslatedRequestParams(),
                           [this](OperationContext* opCtx) { return getNewSession(opCtx); });

                    generateCommitEventForChangeStreams(opCtx,
                                                        nss(),
                                                        *_collectionUUID,
                                                        _request,
                                                        *_doc.getTranslatedRequestParams(),
                                                        CommitPhase::kSuccessful);

                    LOGV2_DEBUG(5277907, 2, "Collection successfully committed", logAttrs(nss()));

                    forceShardFilteringMetadataRefresh(opCtx, nss());
                } catch (const DBException& ex) {
                    LOGV2(
                        5277908,
                        "Failed to obtain collection's placement version, so it will be recovered",
                        logAttrs(nss()),
                        "error"_attr = redact(ex));

                    // If the refresh fails, then set the placement version to UNKNOWN and let a
                    // future operation to refresh the metadata.

                    // TODO (SERVER-71444): Fix to be interruptible or document exception.
                    {
                        UninterruptibleLockGuard noInterrupt(  // NOLINT.
                            shard_role_details::getLocker(opCtx));
                        AutoGetCollection autoColl(opCtx, nss(), MODE_IX);
                        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx,
                                                                                             nss())
                            ->clearFilteringMetadata(opCtx);
                    }

                    generateCommitEventForChangeStreams(opCtx,
                                                        nss(),
                                                        *_collectionUUID,
                                                        _request,
                                                        *_doc.getTranslatedRequestParams(),
                                                        CommitPhase::kAborted);

                    throw;
                }

                // Best effort refresh to warm up cache of all involved shards so we can have a
                // cluster ready to receive operations.
                auto shardRegistry = Grid::get(opCtx)->shardRegistry();
                auto dbPrimaryShardId = ShardingState::get(opCtx)->shardId();

                for (const auto& shardid : involvedShards) {
                    if (shardid == dbPrimaryShardId) {
                        continue;
                    }

                    auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardid));
                    shard->runFireAndForgetCommand(
                        opCtx,
                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                        DatabaseName::kAdmin,
                        BSON("_flushRoutingTableCacheUpdates" << NamespaceStringUtil::serialize(
                                 nss(), SerializationContext::stateDefault())));
                }

                LOGV2(5277901,
                      "Created initial chunk(s)",
                      logAttrs(nss()),
                      "numInitialChunks"_attr = _initialChunks->chunks.size(),
                      "initialCollectionPlacementVersion"_attr =
                          _initialChunks->collPlacementVersion());

                _result = CreateCollectionResponse(
                    ShardVersionFactory::make(_initialChunks->chunks.back().getVersion(),
                                              boost::optional<CollectionIndexes>(boost::none)));
                _result->setCollectionUUID(_collectionUUID);

                LOGV2(5458701,
                      "Collection created",
                      logAttrs(nss()),
                      "UUID"_attr = _result->getCollectionUUID(),
                      "placementVersion"_attr = _result->getCollectionVersion());

                // End of the critical section, from now on, read and writes are permitted.
                exitCriticalSectionsOnCoordinator(opCtx, true, _critSecReason, originalNss());
            }))
        .then([this] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);
            logEndCreateCollection(opCtx, originalNss(), _result, _collectionEmpty, _initialChunks);
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (status == ErrorCodes::RequestAlreadyFulfilled) {
                return Status::OK();
            }

            if (_doc.getPhase() < Phase::kCommit) {
                // Early exit to not trigger the clean up procedure because the coordinator has not
                // entered to any critical section.
                return status;
            }

            if (!_isRetriableErrorForDDLCoordinator(status)) {
                const auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                triggerCleanup(opCtx, status);
            }

            return status;
        });
}

ExecutorFuture<void> CreateCollectionCoordinatorLegacy::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, token, executor = executor, status, anchor = shared_from_this()] {
            const auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                opCtx, getNewSession(opCtx), **executor);

            if (_doc.getTranslatedRequestParams()) {
                // The new behaviour of committing transactionally is not feature flag protected. It
                // could happen that the coordinator is run the first time on a primary with an
                // older binary and, after persisting the chunk entries, a new primary with a newer
                // binary takes over. Therefore, if the collection can be found locally but is not
                // yet tracked on the config server, then we clean up the config.chunks collection.

                auto uuid = sharding_ddl_util::getCollectionUUID(opCtx, nss());
                auto cri = uassertStatusOK(
                    Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx,
                                                                                          nss()));
                if (uuid && !cri.cm.hasRoutingTable()) {
                    cleanupPartialChunksFromPreviousAttempt(opCtx, *uuid, getNewSession(opCtx));
                }
            }

            // The critical section might have been taken by a migration, we force to skip the
            // invariant check and we do nothing in case it was taken.
            exitCriticalSectionsOnCoordinator(
                opCtx, false /* throwIfReasonDiffers */, _critSecReason, originalNss());
        });
}

void CreateCollectionCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    // If we have two shard collections on the same namespace, then the arguments must be the same.
    const auto otherDoc = CreateCollectionCoordinatorDocumentLegacy::parse(
        IDLParserContext("CreateCollectionCoordinatorDocument"), doc);

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
    invariant(_result.is_initialized());
    return *_result;
}

const NamespaceString& CreateCollectionCoordinator::nss() const {
    // Rely on the resolved request parameters to retrieve the nss to be targeted by the
    // coordinator.
    stdx::lock_guard lk{_docMutex};
    invariant(_doc.getTranslatedRequestParams());
    return _doc.getTranslatedRequestParams()->getNss();
}

void CreateCollectionCoordinator::_exitCriticalSectionsOnParticipants(
    OperationContext* opCtx,
    bool throwIfReasonDiffers,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    ShardsvrParticipantBlock unblockCRUDOperationsRequest(nss());
    unblockCRUDOperationsRequest.setBlockType(CriticalSectionBlockTypeEnum::kUnblock);
    unblockCRUDOperationsRequest.setReason(_critSecReason);
    unblockCRUDOperationsRequest.setClearFilteringMetadata(true);
    unblockCRUDOperationsRequest.setThrowIfReasonDiffers(throwIfReasonDiffers);

    const auto primaryShardId = ShardingState::get(opCtx)->shardId();
    auto participants = *_doc.getShardIds();
    participants.erase(std::remove(participants.begin(), participants.end(), primaryShardId),
                       participants.end());

    async_rpc::GenericArgs args;
    async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
    async_rpc::AsyncRPCCommandHelpers::appendOSI(args, getNewSession(opCtx));
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
        **executor, token, unblockCRUDOperationsRequest, args);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, participants);
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
                _checkPreconditions();
            }
        })
        .then(_buildPhaseHandler(Phase::kEnterWriteCriticalSectionOnCoordinator,
                                 [this, anchor = shared_from_this()] {  // NOLINT
                                     _enterWriteCriticalSectionOnCoordinator();
                                 }))
        .then(_buildPhaseHandler(Phase::kTranslateRequestParameters,
                                 [this, anchor = shared_from_this()] {  // NOLINT
                                     _translateRequestParameters();
                                 }))
        .then(_buildPhaseHandler(Phase::kCreateCollectionOnCoordinator,
                                 [this, anchor = shared_from_this()] {  // NOLINT
                                     _createCollectionOnCoordinator();
                                 }))
        .then(_buildPhaseHandler(Phase::kEnterCriticalSection,
                                 [this, token, executor = executor, anchor = shared_from_this()] {
                                     _enterCriticalSection(executor, token);
                                 }))
        .then(_buildPhaseHandler(Phase::kCreateCollectionOnParticipants,
                                 [this, executor = executor, anchor = shared_from_this()] {
                                     _createCollectionOnParticipants(executor);
                                 }))
        .then(_buildPhaseHandler(Phase::kCommitOnShardingCatalog,
                                 [this, executor = executor, anchor = shared_from_this()] {
                                     _commitOnShardingCatalog(executor);
                                 }))
        .then(_buildPhaseHandler(Phase::kSetPostCommitMetadata,
                                 [this, executor = executor, anchor = shared_from_this()] {
                                     _setPostCommitMetadata(executor);
                                 }))
        .then(_buildPhaseHandler(Phase::kExitCriticalSection,
                                 [this, token, executor = executor, anchor = shared_from_this()] {
                                     _exitCriticalSection(executor, token);
                                 }))
        .then([this, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            if (_firstExecution) {
                const auto& placementVersion = _initialChunks->chunks.back().getVersion();
                CreateCollectionResponse response{ShardVersionFactory::make(
                    placementVersion, boost::optional<CollectionIndexes>(boost::none))};
                response.setCollectionUUID(_uuid);
                _result = std::move(response);
            } else {
                // Recover metadata from the config server if the coordinator was resumed after
                // releasing the critical section: a migration could potentially have committed
                // changing the placement version.
                auto cri = uassertStatusOK(
                    Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx,
                                                                                          nss()));
                CreateCollectionResponse response{cri.getCollectionVersion()};
                response.setCollectionUUID(cri.cm.getUUID());
                _result = std::move(response);
            }

            logEndCreateCollection(opCtx, originalNss(), _result, _collectionEmpty, _initialChunks);
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

            const auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

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

            if (!_mustAlwaysMakeProgress() && !_isRetriableErrorForDDLCoordinator(status)) {
                triggerCleanup(opCtx, status);
            }

            return status;
        });
}

void CreateCollectionCoordinator::_checkPreconditions() {
    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    checkCommandArguments(opCtx, _request, originalNss());

    // Perform a preliminary check on whether the request may resolve into a no-op before acquiring
    // any critical section.
    auto createCollectionResponseOpt =
        checkIfCollectionAlreadyTrackedWithSameOptions(opCtx, _request, originalNss());
    if (createCollectionResponseOpt) {
        _result = createCollectionResponseOpt;
        // Launch an exception to directly jump to the end of the continuation chain
        uasserted(ErrorCodes::RequestAlreadyFulfilled,
                  str::stream() << "The collection" << originalNss().toStringForErrorMsg()
                                << "is already tracked from a past request");
    }
}

void CreateCollectionCoordinator::_enterWriteCriticalSectionOnCoordinator() {
    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    logStartCreateCollection(opCtx, _request, originalNss());
    enterCriticalSectionsOnCoordinator(opCtx, _critSecReason, originalNss());
}

void CreateCollectionCoordinator::_translateRequestParameters() {
    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    _doc.setTranslatedRequestParams(translateRequestParameters(opCtx, _request, originalNss()));
}

void CreateCollectionCoordinator::_createCollectionOnCoordinator() {
    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    ShardKeyPattern shardKeyPattern(_doc.getTranslatedRequestParams()->getKeyPattern());
    _collectionEmpty = checkIfCollectionIsEmpty(opCtx, nss());
    const auto splitPolicy = create_collection_util::createPolicy(
        opCtx,
        shardKeyPattern,
        _request.getPresplitHashedZones().value_or(false),
        getTagsAndValidate(opCtx, nss(), shardKeyPattern.toBSON(), _request.getUnsplittable()),
        getNumShards(opCtx),
        *_collectionEmpty,
        _request.getUnsplittable(),
        _request.getDataShard());

    _uuid = createCollectionAndIndexes(opCtx,
                                       shardKeyPattern,
                                       _request,
                                       _collectionEmpty,
                                       nss(),
                                       _doc.getTranslatedRequestParams());

    audit::logShardCollection(
        opCtx->getClient(), nss(), *_request.getShardKey(), _request.getUnique().value_or(false));

    _initialChunks = createChunks(opCtx, shardKeyPattern, _uuid, splitPolicy, nss());

    // Save on doc the set of shards involved in the chunk distribution
    std::set<ShardId> involvedShards;
    for (const auto& chunk : _initialChunks->chunks) {
        involvedShards.emplace(chunk.getShard());
    }
    std::vector<ShardId> shardIds(std::make_move_iterator(involvedShards.begin()),
                                  std::make_move_iterator(involvedShards.end()));
    _doc.setShardIds(std::move(shardIds));
}

void CreateCollectionCoordinator::_enterCriticalSection(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const CancellationToken& token) {
    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    if (!_firstExecution) {
        _performNoopRetryableWriteOnAllShardsAndConfigsvr(opCtx, getNewSession(opCtx), **executor);
    }

    ShardsvrParticipantBlock blockCRUDOperationsRequest(nss());
    blockCRUDOperationsRequest.setBlockType(mongo::CriticalSectionBlockTypeEnum::kReadsAndWrites);
    blockCRUDOperationsRequest.setReason(_critSecReason);

    const auto primaryShardId = ShardingState::get(opCtx)->shardId();
    auto participants = *_doc.getShardIds();
    participants.erase(std::remove(participants.begin(), participants.end(), primaryShardId),
                       participants.end());

    async_rpc::GenericArgs args;
    async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
    async_rpc::AsyncRPCCommandHelpers::appendOSI(args, getNewSession(opCtx));
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
        **executor, token, blockCRUDOperationsRequest, args);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, participants);

    // Block reads/writes from here on if we need to create the collection on other shards, this way
    // we prevent reads/writes that should be redirected to another shard.
    enterCriticalSectionsOnCoordinatorToBlockReads(opCtx, _critSecReason, originalNss());
}

void CreateCollectionCoordinator::_createCollectionOnParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    if (!_firstExecution) {
        _performNoopRetryableWriteOnAllShardsAndConfigsvr(opCtx, getNewSession(opCtx), **executor);

        broadcastDropCollection(opCtx, nss(), **executor, getNewSession(opCtx));

        _uuid = sharding_ddl_util::getCollectionUUID(opCtx, nss());
    }

    createCollectionOnParticipants(opCtx, getNewSession(opCtx), _uuid, *_doc.getShardIds(), nss());
}

void CreateCollectionCoordinator::_commitOnShardingCatalog(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    if (MONGO_unlikely(hangBeforeCommitOnShardingCatalog.shouldFail())) {
        LOGV2(8363100, "Hanging due to hangBeforeCommitOnShardingCatalog fail point");
        hangBeforeCommitOnShardingCatalog.pauseWhileSet();
    }

    if (!_firstExecution) {
        _performNoopRetryableWriteOnAllShardsAndConfigsvr(opCtx, getNewSession(opCtx), **executor);

        // Check if a previous request already created and committed the collection.
        const auto shardKeyPattern =
            ShardKeyPattern(_doc.getTranslatedRequestParams()->getKeyPattern());
        if (sharding_ddl_util::checkIfCollectionAlreadyTrackedWithOptions(
                opCtx,
                nss(),
                shardKeyPattern.toBSON(),
                _doc.getTranslatedRequestParams()->getCollation(),
                _request.getUnique().value_or(false),
                _request.getUnsplittable().value_or(false))) {
            return;
        }

        _uuid = sharding_ddl_util::getCollectionUUID(opCtx, nss());

        _collectionEmpty = checkIfCollectionIsEmpty(opCtx, nss());

        // Re-calculate initial chunk distribution given the set of shards with the critical section
        // taken.
        try {
            const auto splitPolicy = create_collection_util::createPolicy(
                opCtx,
                shardKeyPattern,
                _request.getPresplitHashedZones().value_or(false),
                getTagsAndValidate(
                    opCtx, nss(), shardKeyPattern.toBSON(), _request.getUnsplittable()),
                getNumShards(opCtx),
                *_collectionEmpty,
                _request.getUnsplittable(),
                _request.getDataShard(),
                _doc.getShardIds());
            _initialChunks = createChunks(opCtx, shardKeyPattern, _uuid, splitPolicy, nss());
        } catch (const DBException& ex) {
            // If there is any error when re-calculating the initial chunk distribution, rollback
            // the create collection coordinator. If an error happens during this pre-stage,
            // although we are on a phase that we must always make progress, there is no way to
            // commit with a corrupted chunk distribution. This situation is triggered by executing
            // addZone and/or addShard violating the actual set of involved shards or the shard key
            // selected.
            triggerCleanup(opCtx, ex.toStatus());
            MONGO_UNREACHABLE;
        }
    }

    std::set<ShardId> involvedShards;
    for (const auto& chunk : _initialChunks->chunks) {
        involvedShards.emplace(chunk.getShard());
    }

    generateCommitEventForChangeStreams(opCtx,
                                        nss(),
                                        *_uuid,
                                        _request,
                                        *_doc.getTranslatedRequestParams(),
                                        CommitPhase::kPrepare,
                                        involvedShards);

    commit(opCtx,
           **executor,
           _request,
           _initialChunks,
           _uuid,
           nss(),
           involvedShards,
           _doc.getTranslatedRequestParams(),
           [this](OperationContext* opCtx) { return getNewSession(opCtx); });
}

void CreateCollectionCoordinator::_setPostCommitMetadata(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    if (!_firstExecution) {
        _performNoopRetryableWriteOnAllShardsAndConfigsvr(opCtx, getNewSession(opCtx), **executor);

        _uuid = sharding_ddl_util::getCollectionUUID(opCtx, nss());
    }

    // Ensure that the change stream event gets emitted at least once.
    generateCommitEventForChangeStreams(opCtx,
                                        nss(),
                                        *_uuid,
                                        _request,
                                        *_doc.getTranslatedRequestParams(),
                                        CommitPhase::kSuccessful);

    // Install new filtering metadata or clear it.
    try {
        forceShardFilteringMetadataRefresh(opCtx, nss());
    } catch (const DBException&) {
        AutoGetCollection autoColl(opCtx, nss(), MODE_IX);
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss())
            ->clearFilteringMetadata(opCtx);
    }
}

void CreateCollectionCoordinator::_exitCriticalSection(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const CancellationToken& token) {
    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    if (!_firstExecution) {
        _performNoopRetryableWriteOnAllShardsAndConfigsvr(opCtx, getNewSession(opCtx), **executor);
    }

    _exitCriticalSectionsOnParticipants(opCtx, false /* throwIfReasonDiffers */, executor, token);

    // If the coordinator successfully committed the collection during a previous execution, the
    // critical section may have already been released. In such case, it is safe to skip the release
    // if the reason does not match because a migration may have already re-acquired it.
    exitCriticalSectionsOnCoordinator(opCtx, _firstExecution, _critSecReason, originalNss());
}

ExecutorFuture<void> CreateCollectionCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, token, executor = executor, status, anchor = shared_from_this()] {
            const auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                opCtx, getNewSession(opCtx), **executor);

            if (_doc.getPhase() >= Phase::kCommitOnShardingCatalog) {
                _uuid = sharding_ddl_util::getCollectionUUID(opCtx, nss());

                // Notify change streams to abort the shard collection.
                generateCommitEventForChangeStreams(opCtx,
                                                    nss(),
                                                    *_uuid,
                                                    _request,
                                                    *_doc.getTranslatedRequestParams(),
                                                    CommitPhase::kAborted);
            }

            if (_doc.getPhase() >= Phase::kEnterCriticalSection) {
                _exitCriticalSectionsOnParticipants(
                    opCtx, true /* throwIfReasonDiffers */, executor, token);
            }

            exitCriticalSectionsOnCoordinator(
                opCtx, true /* throwIfReasonDiffers */, _critSecReason, originalNss());
        });
}

}  // namespace mongo
