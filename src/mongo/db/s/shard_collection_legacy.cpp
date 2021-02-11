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

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/s/active_shard_collection_registry.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/shard_collection_legacy.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/shard_key_util.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/clone_collection_options_from_primary_shard_gen.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/sharded_collections_ddl_parameters_gen.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(pauseShardCollectionBeforeCriticalSection);
MONGO_FAIL_POINT_DEFINE(pauseShardCollectionReadOnlyCriticalSection);
MONGO_FAIL_POINT_DEFINE(pauseShardCollectionCommitPhase);
MONGO_FAIL_POINT_DEFINE(pauseShardCollectionAfterCriticalSection);

struct ShardCollectionTargetState {
    UUID uuid;
    ShardKeyPattern shardKeyPattern;
    std::vector<TagsType> tags;
    bool collectionIsEmpty;
};

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});

/**
 * If the specified status is not OK logs a warning and throws a DBException corresponding to the
 * specified status.
 */
void uassertStatusOKWithWarning(const Status& status) {
    if (!status.isOK()) {
        LOGV2_WARNING(22103,
                      "shardsvrShardCollection failed {error}",
                      "shardsvrShardCollection failed",
                      "error"_attr = redact(status));
        uassertStatusOK(status);
    }
}

boost::optional<CreateCollectionResponse> checkIfCollectionAlreadyShardedWithSameOptions(
    OperationContext* opCtx, const ShardsvrShardCollectionRequest& request) {
    const auto& nss = *request.get_shardsvrShardCollection();
    return mongo::sharding_ddl_util::checkIfCollectionAlreadySharded(
        opCtx, nss, request.getKey(), *request.getCollation(), request.getUnique());
}

void checkForExistingChunks(OperationContext* opCtx, const NamespaceString& nss) {
    BSONObjBuilder countBuilder;
    countBuilder.append("count", ChunkType::ConfigNS.coll());
    countBuilder.append("query", BSON(ChunkType::ns(nss.ns())));

    // OK to use limit=1, since if any chunks exist, we will fail.
    countBuilder.append("limit", 1);

    auto readConcern =
        Grid::get(opCtx)->readConcernWithConfigTime(repl::ReadConcernLevel::kMajorityReadConcern);
    readConcern.appendInfo(&countBuilder);

    auto cmdResponse = uassertStatusOK(
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            kConfigReadSelector,
            ChunkType::ConfigNS.db().toString(),
            countBuilder.done(),
            Shard::kDefaultConfigCommandTimeout,
            Shard::RetryPolicy::kIdempotent));
    uassertStatusOK(cmdResponse.commandStatus);

    long long numChunks;
    uassertStatusOK(bsonExtractIntegerField(cmdResponse.response, "n", &numChunks));
    uassert(ErrorCodes::ManualInterventionRequired,
            str::stream() << "A previous attempt to shard collection " << nss.ns()
                          << " failed after writing some initial chunks to config.chunks. Please "
                             "manually delete the partially written chunks for collection "
                          << nss.ns() << " from config.chunks",
            numChunks == 0);
}

void checkCollation(OperationContext* opCtx, const ShardsvrShardCollectionRequest& request) {
    // Ensure the collation is valid. Currently we only allow the simple collation.
    std::unique_ptr<CollatorInterface> requestedCollator;

    const auto& collation = *request.getCollation();
    if (!collation.isEmpty())
        requestedCollator = uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));

    AutoGetCollection autoColl(opCtx,
                               *request.get_shardsvrShardCollection(),
                               MODE_IS,
                               AutoGetCollectionViewMode::kViewsForbidden);

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
        return;

    // TODO (SERVER-48639): If this check fails, this means the collation changed between the time
    // '_configsvrShardCollection' was called and the request got to the shard. Report the message
    // as if it failed on the config server in the first place.
    uassert(ErrorCodes::BadValue,
            str::stream()
                << "Collection has default collation: "
                << (actualCollator ? actualCollator : requestedCollator.get())->getSpec().toBSON()
                << ". Must specify collation {locale: 'simple'}.",
            CollatorInterface::collatorsMatch(requestedCollator.get(), actualCollator));
}

/**
 * Compares the proposed shard key with the shard key of the collection's existing zones
 * to ensure they are a legal combination.
 */
void validateShardKeyAgainstExistingZones(OperationContext* opCtx,
                                          const BSONObj& proposedKey,
                                          const ShardKeyPattern& shardKeyPattern,
                                          const std::vector<TagsType>& tags) {
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
        }
    }
}

std::vector<TagsType> getTagsAndValidate(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const BSONObj& proposedKey,
                                         const ShardKeyPattern& shardKeyPattern) {
    // Read zone info
    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto tags = uassertStatusOK(catalogClient->getTagsForCollection(opCtx, nss));

    if (!tags.empty()) {
        validateShardKeyAgainstExistingZones(opCtx, proposedKey, shardKeyPattern, tags);
    }

    return tags;
}

boost::optional<UUID> getUUIDFromPrimaryShard(OperationContext* opCtx, const NamespaceString& nss) {
    // Obtain the collection's UUID from the primary shard's listCollections response.
    DBDirectClient localClient(opCtx);
    BSONObj res;
    {
        std::list<BSONObj> all =
            localClient.getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));
        if (!all.empty()) {
            res = all.front().getOwned();
        }
    }

    uassert(ErrorCodes::InternalError,
            str::stream() << "expected to have an entry for " << nss.toString()
                          << " in listCollections response, but did not",
            !res.isEmpty());

    BSONObj collectionInfo;
    if (res["info"].type() == BSONType::Object) {
        collectionInfo = res["info"].Obj();
    }

    uassert(ErrorCodes::InternalError,
            str::stream() << "expected to return 'info' field as part of "
                             "listCollections for "
                          << nss.ns()
                          << " because the cluster is in featureCompatibilityVersion=3.6, but got "
                          << res,
            !collectionInfo.isEmpty());

    uassert(ErrorCodes::InternalError,
            str::stream() << "expected to return a UUID for collection " << nss.ns()
                          << " as part of 'info' field but got " << res,
            collectionInfo.hasField("uuid"));

    return uassertStatusOK(UUID::parse(collectionInfo["uuid"]));
}

UUID getOrGenerateUUID(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const ShardsvrShardCollectionRequest& request) {
    if (request.getGetUUIDfromPrimaryShard()) {
        return *getUUIDFromPrimaryShard(opCtx, nss);
    }

    return UUID::gen();
}

bool checkIfCollectionIsEmpty(OperationContext* opCtx, const NamespaceString& nss) {
    // Use find with predicate instead of count in order to ensure that the count
    // command doesn't just consult the cached metadata, which may not always be
    // correct
    DBDirectClient localClient(opCtx);
    return localClient.findOne(nss.ns(), Query()).isEmpty();
}

int getNumShards(OperationContext* opCtx) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    shardRegistry->reload(opCtx);
    return shardRegistry->getAllShardIds(opCtx).size();
}

ShardCollectionTargetState calculateTargetState(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const ShardsvrShardCollectionRequest& request) {
    auto proposedKey(request.getKey().getOwned());
    ShardKeyPattern shardKeyPattern(proposedKey);

    shardkeyutil::validateShardKeyIndexExistsOrCreateIfPossible(
        opCtx,
        nss,
        proposedKey,
        shardKeyPattern,
        *request.getCollation(),
        request.getUnique(),
        shardkeyutil::ValidationBehaviorsShardCollection(opCtx));

    auto tags = getTagsAndValidate(opCtx, nss, proposedKey, shardKeyPattern);
    auto uuid = getOrGenerateUUID(opCtx, nss, request);

    const bool isEmpty = checkIfCollectionIsEmpty(opCtx, nss);
    return {uuid, std::move(shardKeyPattern), tags, isEmpty};
}

void logStartShardCollection(OperationContext* opCtx,
                             const BSONObj& cmdObj,
                             const NamespaceString& nss,
                             const ShardsvrShardCollectionRequest& request,
                             const ShardCollectionTargetState& prerequisites,
                             const ShardId& dbPrimaryShardId) {
    LOGV2(
        22100, "CMD: shardcollection: {command}", "CMD: shardcollection", "command"_attr = cmdObj);

    audit::logShardCollection(
        opCtx->getClient(), nss.ns(), prerequisites.shardKeyPattern.toBSON(), request.getUnique());

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto primaryShard = uassertStatusOK(shardRegistry->getShard(opCtx, dbPrimaryShardId));

    // Record start in changelog
    {
        BSONObjBuilder collectionDetail;
        collectionDetail.append("shardKey", prerequisites.shardKeyPattern.toBSON());
        collectionDetail.append("collection", nss.ns());
        prerequisites.uuid.appendToBuilder(&collectionDetail, "uuid");
        collectionDetail.append("empty", prerequisites.collectionIsEmpty);
        collectionDetail.append("primary", primaryShard->toString());
        uassertStatusOK(ShardingLogging::get(opCtx)->logChangeChecked(
            opCtx,
            "shardCollection.start",
            nss.ns(),
            collectionDetail.obj(),
            ShardingCatalogClient::kMajorityWriteConcern));
    }
}

void createCollectionOnShardsReceivingChunks(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const std::vector<ChunkType>& initialChunks,
                                             const ShardId& dbPrimaryShardId) {

    std::vector<AsyncRequestsSender::Request> requests;
    std::set<ShardId> initializedShards;
    for (const auto& chunk : initialChunks) {
        const auto& chunkShardId = chunk.getShard();
        if (chunkShardId == dbPrimaryShardId ||
            initializedShards.find(chunkShardId) != initializedShards.end()) {
            continue;
        }


        CloneCollectionOptionsFromPrimaryShard cloneCollectionOptionsFromPrimaryShardRequest(nss);
        cloneCollectionOptionsFromPrimaryShardRequest.setPrimaryShard(dbPrimaryShardId.toString());
        cloneCollectionOptionsFromPrimaryShardRequest.setDbName(nss.db());

        requests.emplace_back(
            chunkShardId,
            cloneCollectionOptionsFromPrimaryShardRequest.toBSON(
                BSON("writeConcern" << ShardingCatalogClient::kMajorityWriteConcern.toBSON())));

        initializedShards.emplace(chunkShardId);
    }

    if (!requests.empty()) {
        auto responses = gatherResponses(opCtx,
                                         nss.db(),
                                         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                         Shard::RetryPolicy::kIdempotent,
                                         requests);

        // If any shards fail to create the collection, fail the entire shardCollection command
        // (potentially leaving incomplely created sharded collection)
        for (const auto& response : responses) {
            auto shardResponse =
                uassertStatusOKWithContext(std::move(response.swResponse),
                                           str::stream() << "Unable to create collection "
                                                         << nss.ns() << " on " << response.shardId);
            auto status = getStatusFromCommandResult(shardResponse.data);
            uassertStatusOK(status.withContext(str::stream()
                                               << "Unable to create collection " << nss.ns()
                                               << " on " << response.shardId));

            auto wcStatus = getWriteConcernStatusFromCommandResult(shardResponse.data);
            uassertStatusOK(wcStatus.withContext(str::stream()
                                                 << "Unable to create collection " << nss.ns()
                                                 << " on " << response.shardId));
        }
    }
}

void writeFirstChunksToConfig(OperationContext* opCtx,
                              const InitialSplitPolicy::ShardCollectionConfig& initialChunks) {

    std::vector<BSONObj> chunkObjs;
    chunkObjs.reserve(initialChunks.chunks.size());
    for (const auto& chunk : initialChunks.chunks) {
        chunkObjs.push_back(chunk.toConfigBSON());
    }

    Grid::get(opCtx)->catalogClient()->insertConfigDocumentsAsRetryableWrite(
        opCtx,
        ChunkType::ConfigNS,
        std::move(chunkObjs),
        ShardingCatalogClient::kMajorityWriteConcern);
}

void updateShardingCatalogEntryForCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardCollectionTargetState& prerequisites,
    const InitialSplitPolicy::ShardCollectionConfig& initialChunks,
    const BSONObj& defaultCollation,
    const bool unique) {
    // Construct the collection default collator.
    std::unique_ptr<CollatorInterface> defaultCollator;
    if (!defaultCollation.isEmpty()) {
        defaultCollator = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                              ->makeFromBSON(defaultCollation));
    }

    boost::optional<Timestamp> creationTime;
    if (feature_flags::gShardingFullDDLSupportTimestampedVersion.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        creationTime = initialChunks.creationTime;
    }

    CollectionType coll(
        nss, initialChunks.collVersion().epoch(), creationTime, Date_t::now(), prerequisites.uuid);
    coll.setKeyPattern(prerequisites.shardKeyPattern.toBSON());
    if (defaultCollator) {
        coll.setDefaultCollation(defaultCollator->getSpec().toBSON());
    }
    coll.setUnique(unique);

    uassertStatusOK(ShardingCatalogClientImpl::updateShardingCatalogEntryForCollection(
        opCtx, nss, coll, true /*upsert*/));
}

void refreshAllShards(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const ShardId& dbPrimaryShardId,
                      const std::vector<ChunkType>& initialChunks) {
    // If the refresh fails, then the shard will end with a shardVersion UNSHARDED.
    try {
        forceShardFilteringMetadataRefresh(opCtx, nss);
    } catch (const DBException&) {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        CollectionShardingRuntime::get(opCtx, nss)->clearFilteringMetadata(opCtx);
        throw;
    }

    auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    std::set<ShardId> shardsRefreshed;
    for (const auto& chunk : initialChunks) {
        const auto& chunkShardId = chunk.getShard();
        if (chunkShardId == dbPrimaryShardId ||
            shardsRefreshed.find(chunkShardId) != shardsRefreshed.end()) {
            continue;
        }

        auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, chunkShardId));
        auto refreshCmdResponse = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            BSON("_flushRoutingTableCacheUpdates" << nss.ns()),
            Seconds{30},
            Shard::RetryPolicy::kIdempotent));

        uassertStatusOK(refreshCmdResponse.commandStatus);
        shardsRefreshed.emplace(chunkShardId);
    }
}

CreateCollectionResponse shardCollection(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const BSONObj& cmdObj,
                                         const ShardsvrShardCollectionRequest& request,
                                         const ShardId& dbPrimaryShardId,
                                         bool mustTakeDistLock) {
    CreateCollectionResponse shardCollectionResponse;
    // Fast check for whether the collection is already sharded without taking any locks
    if (auto createCollectionResponseOpt =
            checkIfCollectionAlreadyShardedWithSameOptions(opCtx, request)) {
        return *createCollectionResponseOpt;
    }

    auto writeChunkDocumentsAndRefreshShards =
        [&](const ShardCollectionTargetState& targetState,
            const InitialSplitPolicy::ShardCollectionConfig& initialChunks) {
            // Insert chunk documents to config.chunks on the config server.
            writeFirstChunksToConfig(opCtx, initialChunks);
            // If an error happens when contacting the config server, we don't know if the update
            // succeded or not, which might cause the local shard version to differ from the config
            // server, so we clear the metadata to allow another operation to refresh it.
            try {
                updateShardingCatalogEntryForCollection(opCtx,
                                                        nss,
                                                        targetState,
                                                        initialChunks,
                                                        *request.getCollation(),
                                                        request.getUnique());

            } catch (const DBException&) {
                UninterruptibleLockGuard noInterrupt(opCtx->lockState());
                AutoGetCollection autoColl(opCtx, nss, MODE_IX);
                CollectionShardingRuntime::get(opCtx, nss)->clearFilteringMetadata(opCtx);
                throw;
            }

            refreshAllShards(opCtx, nss, dbPrimaryShardId, initialChunks.chunks);
        };

    boost::optional<ShardCollectionTargetState> targetState;
    std::unique_ptr<InitialSplitPolicy> splitPolicy;
    InitialSplitPolicy::ShardCollectionConfig initialChunks;

    bool shouldUseUUIDForChunkIndexing;
    {
        invariant(!opCtx->lockState()->isLocked());
        Lock::SharedLock fcvLock(opCtx->lockState(), FeatureCompatibilityVersion::fcvLock);
        shouldUseUUIDForChunkIndexing =
            feature_flags::gShardingFullDDLSupportTimestampedVersion.isEnabled(
                serverGlobalParams.featureCompatibility);
    }

    {
        pauseShardCollectionBeforeCriticalSection.pauseWhileSet();

        // Make the distlocks boost::optional so that they can be emplaced only on the non legacy
        // path.
        boost::optional<DistLockManager::ScopedDistLock> dbDistLock;
        boost::optional<DistLockManager::ScopedDistLock> collDistLock;
        if (mustTakeDistLock) {
            dbDistLock.emplace(uassertStatusOK(DistLockManager::get(opCtx)->lock(
                opCtx, nss.db(), "shardCollection", DistLockManager::kDefaultLockTimeout)));
            collDistLock.emplace(uassertStatusOK(DistLockManager::get(opCtx)->lock(
                opCtx, nss.ns(), "shardCollection", DistLockManager::kDefaultLockTimeout)));
        }

        // From this point onward the collection can only be read, not written to, so it is safe to
        // construct the prerequisites and generate the target state.
        ScopedShardVersionCriticalSection critSec(opCtx, nss);

        pauseShardCollectionReadOnlyCriticalSection.pauseWhileSet();

        if (auto createCollectionResponseOpt =
                checkIfCollectionAlreadyShardedWithSameOptions(opCtx, request)) {
            return *createCollectionResponseOpt;
        }

        // If DistLock must not be taken, then the request came from the config server, there is no
        // need to check this here.
        if (mustTakeDistLock) {
            if (nss.db() == NamespaceString::kConfigDb) {
                auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

                auto findReponse = uassertStatusOK(configShard->exhaustiveFindOnConfig(
                    opCtx,
                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                    repl::ReadConcernLevel::kMajorityReadConcern,
                    nss,
                    BSONObj(),
                    BSONObj(),
                    1));

                auto numDocs = findReponse.docs.size();

                // If this is a collection on the config db, it must be empty to be sharded.
                uassert(ErrorCodes::IllegalOperation,
                        "collections in the config db must be empty to be sharded",
                        numDocs == 0);
            } else {
                const auto dbInfo = uassertStatusOK(
                    Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, nss.db()));
                auto shardId = ShardingState::get(opCtx)->shardId();

                uassert(ErrorCodes::IllegalOperation,
                        str::stream()
                            << "this is not the primary shard for db " << nss.db()
                            << " expected: " << dbInfo.primaryId() << " shardId: " << shardId,
                        dbInfo.primaryId() == shardId);
            }
        }

        // Fail if there are partially written chunks from a previous failed shardCollection.
        checkForExistingChunks(opCtx, nss);

        checkCollation(opCtx, request);

        targetState = calculateTargetState(opCtx, nss, request);

        shardCollectionResponse.setCollectionUUID(targetState->uuid);

        splitPolicy =
            InitialSplitPolicy::calculateOptimizationStrategy(opCtx,
                                                              targetState->shardKeyPattern,
                                                              request.getNumInitialChunks(),
                                                              request.getPresplitHashedZones(),
                                                              request.getInitialSplitPoints(),
                                                              targetState->tags,
                                                              getNumShards(opCtx),
                                                              targetState->collectionIsEmpty);
        boost::optional<CollectionUUID> optCollectionUUID;
        if (shouldUseUUIDForChunkIndexing) {
            optCollectionUUID = targetState->uuid;
        }

        initialChunks = splitPolicy->createFirstChunks(
            opCtx, targetState->shardKeyPattern, {nss, optCollectionUUID, dbPrimaryShardId});

        // There must be at least one chunk.
        invariant(initialChunks.chunks.size());

        shardCollectionResponse.setCollectionVersion(
            initialChunks.chunks[initialChunks.chunks.size() - 1].getVersion());

        logStartShardCollection(opCtx, cmdObj, nss, request, *targetState, dbPrimaryShardId);

        // From this point onward, the collection can not be written to or read from.
        critSec.enterCommitPhase();
        pauseShardCollectionCommitPhase.pauseWhileSet();

        if (splitPolicy->isOptimized()) {
            createCollectionOnShardsReceivingChunks(
                opCtx, nss, initialChunks.chunks, dbPrimaryShardId);

            writeChunkDocumentsAndRefreshShards(*targetState, initialChunks);
        }
    }

    // We have now left the critical section.
    pauseShardCollectionAfterCriticalSection.pauseWhileSet();

    if (!splitPolicy->isOptimized()) {
        writeChunkDocumentsAndRefreshShards(*targetState, initialChunks);
    }

    LOGV2(22101,
          "Created {numInitialChunks} chunk(s) for: {namespace}, producing collection version "
          "{initialCollectionVersion}",
          "Created initial chunk(s)",
          "numInitialChunks"_attr = initialChunks.chunks.size(),
          "namespace"_attr = nss,
          "initialCollectionVersion"_attr = initialChunks.collVersion());


    ShardingLogging::get(opCtx)->logChange(
        opCtx,
        "shardCollection.end",
        nss.ns(),
        BSON("version" << initialChunks.collVersion().toString() << "numChunks"
                       << static_cast<int>(initialChunks.chunks.size())),
        ShardingCatalogClient::kMajorityWriteConcern);

    return shardCollectionResponse;
}

}  // namespace

CreateCollectionResponse shardCollectionLegacy(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const BSONObj& cmdObj,
                                               bool requestFromCSRS) {
    auto request = ShardsvrShardCollectionRequest::parse(
        IDLParserErrorContext("_shardsvrShardCollection"), cmdObj);
    if (!request.getCollation())
        request.setCollation(BSONObj());
    if (!request.getCollation()->isEmpty()) {
        auto requestedCollator =
            uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                ->makeFromBSON(*request.getCollation()));
        if (!requestedCollator)
            request.setCollation(BSONObj());
    }

    auto scopedShardCollection =
        uassertStatusOK(ActiveShardCollectionRegistry::get(opCtx).registerShardCollection(request));

    boost::optional<CreateCollectionResponse> response;

    // Check if this collection is currently being sharded and if so, join it
    if (!scopedShardCollection.mustExecute()) {
        response = scopedShardCollection.getResponse().get();
    } else {
        try {
            response = shardCollection(opCtx,
                                       nss,
                                       cmdObj,
                                       request,
                                       ShardingState::get(opCtx)->shardId(),
                                       !requestFromCSRS);
        } catch (const DBException& e) {
            scopedShardCollection.emplaceResponse(e.toStatus());
            throw;
        } catch (const std::exception& e) {
            scopedShardCollection.emplaceResponse(
                {ErrorCodes::InternalError,
                 str::stream() << "Severe error occurred while running shardCollection command: "
                               << e.what()});
            throw;
        }

        uassert(ErrorCodes::InvalidUUID,
                str::stream() << "Collection " << nss << " is sharded without UUID",
                response);

        scopedShardCollection.emplaceResponse(response);
    }

    return response.value();
}
}  // namespace mongo