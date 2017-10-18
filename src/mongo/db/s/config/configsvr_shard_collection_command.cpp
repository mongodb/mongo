/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/sessions_collection.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_client.h"
#include "mongo/s/grid.h"
#include "mongo/s/migration_secondary_throttle_options.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using std::string;

const long long kMaxSizeMBDefault = 0;

/**
 * Constructs the BSON specification document for the given namespace, index key and options.
 */
BSONObj makeCreateIndexesCmd(const NamespaceString& nss,
                             const BSONObj& keys,
                             const BSONObj& collation,
                             bool unique) {
    BSONObjBuilder index;

    // Required fields for an index.

    index.append("key", keys);

    StringBuilder indexName;
    bool isFirstKey = true;
    for (BSONObjIterator keyIter(keys); keyIter.more();) {
        BSONElement currentKey = keyIter.next();

        if (isFirstKey) {
            isFirstKey = false;
        } else {
            indexName << "_";
        }

        indexName << currentKey.fieldName() << "_";
        if (currentKey.isNumber()) {
            indexName << currentKey.numberInt();
        } else {
            indexName << currentKey.str();  // this should match up with shell command
        }
    }
    index.append("name", indexName.str());

    // Index options.

    if (!collation.isEmpty()) {
        // Creating an index with the "collation" option requires a v=2 index.
        index.append("v", static_cast<int>(IndexDescriptor::IndexVersion::kV2));
        index.append("collation", collation);
    }

    if (unique && !IndexDescriptor::isIdIndexPattern(keys)) {
        index.appendBool("unique", unique);
    }

    // The outer createIndexes command.

    BSONObjBuilder createIndexes;
    createIndexes.append("createIndexes", nss.coll());
    createIndexes.append("indexes", BSON_ARRAY(index.obj()));
    createIndexes.append("writeConcern", WriteConcernOptions::Majority);
    return createIndexes.obj();
}

/**
 * Validates the options specified in the request.
 *
 * WARNING: After validating the request's collation, replaces it with the collection default
 * collation.
 */
void validateAndDeduceFullRequestOptions(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const ShardKeyPattern& shardKeyPattern,
                                         int numShards,
                                         ScopedDbConnection& conn,
                                         ConfigsvrShardCollectionRequest* request) {
    uassert(
        ErrorCodes::InvalidOptions, "cannot have empty shard key", !request->getKey().isEmpty());

    // Ensure the proposed shard key is valid.
    uassert(ErrorCodes::InvalidOptions,
            str::stream()
                << "Unsupported shard key pattern "
                << shardKeyPattern.toString()
                << ". Pattern must either be a single hashed field, or a list of ascending fields",
            shardKeyPattern.isValid());

    // Ensure that hashed and unique are not both set.
    uassert(ErrorCodes::InvalidOptions,
            "Hashed shard keys cannot be declared unique. It's possible to ensure uniqueness on "
            "the hashed field by declaring an additional (non-hashed) unique index on the field.",
            !shardKeyPattern.isHashedPattern() || !request->getUnique());

    // Ensure the namespace is valid.
    uassert(ErrorCodes::IllegalOperation,
            "can't shard system namespaces",
            !nss.isSystem() || nss.ns() == SessionsCollection::kSessionsFullNS);

    // Ensure the collation is valid. Currently we only allow the simple collation.
    bool simpleCollationSpecified = false;
    if (request->getCollation()) {
        auto& collation = *request->getCollation();
        auto collator = uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
        uassert(ErrorCodes::BadValue,
                str::stream() << "The collation for shardCollection must be {locale: 'simple'}, "
                              << "but found: "
                              << collation,
                !collator);
        simpleCollationSpecified = true;
    }

    // Ensure numInitialChunks is within valid bounds.
    // Cannot have more than 8192 initial chunks per shard. Setting a maximum of 1,000,000
    // chunks in total to limit the amount of memory this command consumes so there is less
    // danger of an OOM error.
    const int maxNumInitialChunksForShards = numShards * 8192;
    const int maxNumInitialChunksTotal = 1000 * 1000;  // Arbitrary limit to memory consumption
    int numChunks = request->getNumInitialChunks();
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "numInitialChunks cannot be more than either: "
                          << maxNumInitialChunksForShards
                          << ", 8192 * number of shards; or "
                          << maxNumInitialChunksTotal,
            numChunks <= maxNumInitialChunksForShards && numChunks <= maxNumInitialChunksTotal);

    // Retrieve the collection metadata in order to verify that it is legal to shard this
    // collection.
    BSONObj res;
    {
        std::list<BSONObj> all =
            conn->getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));
        if (!all.empty()) {
            res = all.front().getOwned();
        }
    }

    BSONObj defaultCollation;

    if (!res.isEmpty()) {
        // Check that namespace is not a view.
        {
            std::string namespaceType;
            uassertStatusOK(bsonExtractStringField(res, "type", &namespaceType));
            uassert(ErrorCodes::CommandNotSupportedOnView,
                    "Views cannot be sharded.",
                    namespaceType != "view");
        }

        BSONObj collectionOptions;
        if (res["options"].type() == BSONType::Object) {
            collectionOptions = res["options"].Obj();
        }

        // Check that collection is not capped.
        uassert(ErrorCodes::InvalidOptions,
                "can't shard a capped collection",
                !collectionOptions["capped"].trueValue());

        // Get collection default collation.
        BSONElement collationElement;
        auto status = bsonExtractTypedField(
            collectionOptions, "collation", BSONType::Object, &collationElement);
        if (status.isOK()) {
            defaultCollation = collationElement.Obj().getOwned();
            uassert(ErrorCodes::BadValue,
                    "Default collation in collection metadata cannot be empty.",
                    !defaultCollation.isEmpty());
        } else if (status != ErrorCodes::NoSuchKey) {
            uassertStatusOK(status);
        }

        // If the collection has a non-simple default collation but the user did not specify the
        // simple collation explicitly, return an error.
        uassert(ErrorCodes::BadValue,
                str::stream() << "Collection has default collation: "
                              << collectionOptions["collation"]
                              << ". Must specify collation {locale: 'simple'}",
                defaultCollation.isEmpty() || simpleCollationSpecified);
    }

    // Once the request's collation has been validated as simple or unset, replace it with the
    // deduced collection default collation.
    request->setCollation(defaultCollation.getOwned());
}

/**
 * Throws an exception if the collection is already sharded with different options.
 *
 * If the collection is already sharded with the same options, returns the existing collection's
 * full spec, else returns boost::none.
 */
boost::optional<CollectionType> checkIfAlreadyShardedWithSameOptions(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ConfigsvrShardCollectionRequest& request) {
    auto existingColls =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
                            opCtx,
                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                            repl::ReadConcernLevel::kLocalReadConcern,
                            NamespaceString(CollectionType::ConfigNS),
                            BSON("_id" << nss.ns() << "dropped" << false),
                            BSONObj(),
                            1))
            .docs;

    if (!existingColls.empty()) {
        auto existingOptions = uassertStatusOK(CollectionType::fromBSON(existingColls.front()));

        CollectionType requestedOptions;
        requestedOptions.setNs(nss);
        requestedOptions.setKeyPattern(KeyPattern(request.getKey()));
        requestedOptions.setDefaultCollation(*request.getCollation());
        requestedOptions.setUnique(request.getUnique());

        // If the collection is already sharded, fail if the deduced options in this request do not
        // match the options the collection was originally sharded with.
        uassert(ErrorCodes::AlreadyInitialized,
                str::stream() << "sharding already enabled for collection " << nss.ns()
                              << " with options "
                              << existingOptions.toString(),
                requestedOptions.hasSameOptions(existingOptions));

        // We did a local read of the collection entry above and found that this shardCollection
        // request was already satisfied. However, the data may not be majority committed (a
        // previous shardCollection attempt may have failed with a writeConcern error).
        // Since the current Client doesn't know the opTime of the last write to the collection
        // entry, make it wait for the last opTime in the system when we wait for writeConcern.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return existingOptions;
    }

    // Not currently sharded.
    return boost::none;
}

/**
 * Compares the proposed shard key with the collection's existing indexes on the primary shard to
 * ensure they are a legal combination.
 *
 * If the collection is empty and no index on the shard key exists, creates the required index.
 */
void validateShardKeyAgainstExistingIndexes(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const BSONObj& proposedKey,
                                            const ShardKeyPattern& shardKeyPattern,
                                            const std::shared_ptr<Shard> primaryShard,
                                            ScopedDbConnection& conn,
                                            const ConfigsvrShardCollectionRequest& request) {
    // The proposed shard key must be validated against the set of existing indexes.
    // In particular, we must ensure the following constraints
    //
    // 1. All existing unique indexes, except those which start with the _id index,
    //    must contain the proposed key as a prefix (uniqueness of the _id index is
    //    ensured by the _id generation process or guaranteed by the user).
    //
    // 2. If the collection is not empty, there must exist at least one index that
    //    is "useful" for the proposed key.  A "useful" index is defined as follows
    //    Useful Index:
    //         i. contains proposedKey as a prefix
    //         ii. is not a sparse index, partial index, or index with a non-simple collation
    //         iii. contains no null values
    //         iv. is not multikey (maybe lift this restriction later)
    //         v. if a hashed index, has default seed (lift this restriction later)
    //
    // 3. If the proposed shard key is specified as unique, there must exist a useful,
    //    unique index exactly equal to the proposedKey (not just a prefix).
    //
    // After validating these constraint:
    //
    // 4. If there is no useful index, and the collection is non-empty, we
    //    must fail.
    //
    // 5. If the collection is empty, and it's still possible to create an index
    //    on the proposed key, we go ahead and do so.

    std::list<BSONObj> indexes = conn->getIndexSpecs(nss.ns());

    // 1.  Verify consistency with existing unique indexes
    for (const auto& idx : indexes) {
        BSONObj currentKey = idx["key"].embeddedObject();
        bool isUnique = idx["unique"].trueValue();
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "can't shard collection '" << nss.ns() << "' with unique index on "
                              << currentKey
                              << " and proposed shard key "
                              << proposedKey
                              << ". Uniqueness can't be maintained unless shard key is a prefix",
                !isUnique || shardKeyPattern.isUniqueIndexCompatible(currentKey));
    }

    // 2. Check for a useful index
    bool hasUsefulIndexForKey = false;
    for (const auto& idx : indexes) {
        BSONObj currentKey = idx["key"].embeddedObject();
        // Check 2.i. and 2.ii.
        if (!idx["sparse"].trueValue() && idx["filter"].eoo() && idx["collation"].eoo() &&
            proposedKey.isPrefixOf(currentKey, SimpleBSONElementComparator::kInstance)) {
            // We can't currently use hashed indexes with a non-default hash seed
            // Check v.
            // Note that this means that, for sharding, we only support one hashed index
            // per field per collection.
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "can't shard collection " << nss.ns()
                                  << " with hashed shard key "
                                  << proposedKey
                                  << " because the hashed index uses a non-default seed of "
                                  << idx["seed"].numberInt(),
                    !shardKeyPattern.isHashedPattern() || idx["seed"].eoo() ||
                        idx["seed"].numberInt() == BSONElementHasher::DEFAULT_HASH_SEED);
            hasUsefulIndexForKey = true;
        }
    }

    // 3. If proposed key is required to be unique, additionally check for exact match.

    if (hasUsefulIndexForKey && request.getUnique()) {
        BSONObj eqQuery = BSON("ns" << nss.ns() << "key" << proposedKey);
        BSONObj eqQueryResult;

        for (const auto& idx : indexes) {
            if (SimpleBSONObjComparator::kInstance.evaluate(idx["key"].embeddedObject() ==
                                                            proposedKey)) {
                eqQueryResult = idx;
                break;
            }
        }

        if (eqQueryResult.isEmpty()) {
            // If no exact match, index not useful, but still possible to create one later
            hasUsefulIndexForKey = false;
        } else {
            bool isExplicitlyUnique = eqQueryResult["unique"].trueValue();
            BSONObj currKey = eqQueryResult["key"].embeddedObject();
            bool isCurrentID = str::equals(currKey.firstElementFieldName(), "_id");
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "can't shard collection " << nss.ns() << ", " << proposedKey
                                  << " index not unique, and unique index explicitly specified",
                    isExplicitlyUnique || isCurrentID);
        }
    }

    if (hasUsefulIndexForKey) {
        // Check 2.iii and 2.iv. Make sure no null entries in the sharding index
        // and that there is a useful, non-multikey index available
        BSONObjBuilder checkShardingIndexCmd;
        checkShardingIndexCmd.append("checkShardingIndex", nss.ns());
        checkShardingIndexCmd.append("keyPattern", proposedKey);
        BSONObj res;
        auto success = conn.get()->runCommand("admin", checkShardingIndexCmd.obj(), res);
        uassert(ErrorCodes::OperationFailed, res["errmsg"].str(), success);
    } else if (conn->count(nss.ns()) != 0) {
        // 4. if no useful index, and collection is non-empty, fail
        uasserted(ErrorCodes::InvalidOptions,
                  "Please create an index that starts with the proposed shard key before "
                  "sharding the collection");
    } else {
        // 5. If no useful index exists, and collection empty, create one on proposedKey.
        //    Only need to call ensureIndex on primary shard, since indexes get copied to
        //    receiving shard whenever a migrate occurs.
        //    If the collection has a default collation, explicitly send the simple
        //    collation as part of the createIndex request.
        BSONObj collation =
            !request.getCollation()->isEmpty() ? CollationSpec::kSimpleSpec : BSONObj();
        auto createIndexesCmd =
            makeCreateIndexesCmd(nss, proposedKey, collation, request.getUnique());

        const auto swResponse = primaryShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            nss.db().toString(),
            createIndexesCmd,
            Shard::RetryPolicy::kNoRetry);
        auto createIndexesStatus = swResponse.getStatus();
        if (createIndexesStatus.isOK()) {
            const auto response = swResponse.getValue();
            createIndexesStatus = (!response.commandStatus.isOK()) ? response.commandStatus
                                                                   : response.writeConcernStatus;
        }
        uassertStatusOK(createIndexesStatus);
    }
}

/**
 * For new collections which use hashed shard keys, we can can pre-split the range of possible
 * hashes into a large number of chunks, and distribute them evenly at creation time. Until we
 * design a better initialization scheme, the safest way to pre-split is to make one big chunk for
 * each shard and migrate them one at a time.
 *
 * Populates 'initSplits' with the split points to use on the primary shard to produce the initial
 * "big chunks."
 * Also populates 'allSplits' with the additional split points to use on the "big chunks" after the
 * "big chunks" have been spread evenly across shards through migrations.
 */
void determinePresplittingPoints(OperationContext* opCtx,
                                 int numShards,
                                 bool isEmpty,
                                 const BSONObj& proposedKey,
                                 const ShardKeyPattern& shardKeyPattern,
                                 const ConfigsvrShardCollectionRequest& request,
                                 std::vector<BSONObj>* initSplits,
                                 std::vector<BSONObj>* allSplits) {
    auto numChunks = request.getNumInitialChunks();

    if (request.getInitialSplitPoints()) {
        *initSplits = std::move(*request.getInitialSplitPoints());
        return;
    }

    if (shardKeyPattern.isHashedPattern() && isEmpty) {
        // If initial split points are not specified, only pre-split when using a hashed shard
        // key and the collection is empty
        if (numChunks <= 0) {
            // default number of initial chunks
            numChunks = 2 * numShards;
        }

        // hashes are signed, 64-bit ints. So we divide the range (-MIN long, +MAX long)
        // into intervals of size (2^64/numChunks) and create split points at the
        // boundaries.  The logic below ensures that initial chunks are all
        // symmetric around 0.
        long long intervalSize = (std::numeric_limits<long long>::max() / numChunks) * 2;
        long long current = 0;

        if (numChunks % 2 == 0) {
            allSplits->push_back(BSON(proposedKey.firstElementFieldName() << current));
            current += intervalSize;
        } else {
            current += intervalSize / 2;
        }

        for (int i = 0; i < (numChunks - 1) / 2; i++) {
            allSplits->push_back(BSON(proposedKey.firstElementFieldName() << current));
            allSplits->push_back(BSON(proposedKey.firstElementFieldName() << -current));
            current += intervalSize;
        }

        sort(allSplits->begin(),
             allSplits->end(),
             SimpleBSONObjComparator::kInstance.makeLessThan());

        // The initial splits define the "big chunks" that we will subdivide later.
        int lastIndex = -1;
        for (int i = 1; i < numShards; i++) {
            if (lastIndex < (i * numChunks) / numShards - 1) {
                lastIndex = (i * numChunks) / numShards - 1;
                initSplits->push_back(allSplits->at(lastIndex));
            }
        }
    } else if (numChunks > 0) {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << (!shardKeyPattern.isHashedPattern()
                                        ? "numInitialChunks is not supported "
                                          "when the shard key is not hashed."
                                        : "numInitialChunks is not supported "
                                          "when the collection is not empty."));
    }
}

/**
 * Migrates the initial "big chunks" from the primary shard to spread them evenly across the shards.
 *
 * If 'allSplits' is not empty, additionally splits each "big chunk" into smaller chunks using the
 * points in 'allSplits.'
 */
void migrateAndFurtherSplitInitialChunks(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         int numShards,
                                         const std::vector<ShardId>& shardIds,
                                         bool isEmpty,
                                         const ShardKeyPattern& shardKeyPattern,
                                         const std::vector<BSONObj>& allSplits) {
    auto catalogCache = Grid::get(opCtx)->catalogCache();

    if (!shardKeyPattern.isHashedPattern()) {
        // Only initially move chunks when using a hashed shard key.
        return;
    }

    if (!isEmpty) {
        // If the collection is not empty, rely on the balancer to migrate the chunks.
        return;
    }

    auto routingInfo = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection was successfully written as sharded but got dropped before it "
            "could be evenly distributed",
            routingInfo.cm());

    auto chunkManager = routingInfo.cm();

    // Move and commit each "big chunk" to a different shard.
    int i = 0;
    for (auto chunk : chunkManager->chunks()) {
        const ShardId& shardId = shardIds[i++ % numShards];
        const auto toStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
        if (!toStatus.isOK()) {
            continue;
        }
        const auto to = toStatus.getValue();

        // Can't move chunk to shard it's already on
        if (to->getId() == chunk->getShardId()) {
            continue;
        }

        ChunkType chunkType;
        chunkType.setNS(nss.ns());
        chunkType.setMin(chunk->getMin());
        chunkType.setMax(chunk->getMax());
        chunkType.setShard(chunk->getShardId());
        chunkType.setVersion(chunkManager->getVersion());

        Status moveStatus = configsvr_client::moveChunk(
            opCtx,
            chunkType,
            to->getId(),
            Grid::get(opCtx)->getBalancerConfiguration()->getMaxChunkSizeBytes(),
            MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
            true);
        if (!moveStatus.isOK()) {
            warning() << "couldn't move chunk " << redact(chunk->toString()) << " to shard " << *to
                      << " while sharding collection " << nss.ns() << causedBy(redact(moveStatus));
        }
    }

    if (allSplits.empty()) {
        return;
    }

    // Reload the config info, after all the migrations
    routingInfo = uassertStatusOK(catalogCache->getCollectionRoutingInfoWithRefresh(opCtx, nss));
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection was successfully written as sharded but got dropped before it "
            "could be evenly distributed",
            routingInfo.cm());
    chunkManager = routingInfo.cm();

    // Subdivide the big chunks by splitting at each of the points in "allSplits"
    // that we haven't already split by.
    auto currentChunk = chunkManager->findIntersectingChunkWithSimpleCollation(allSplits[0]);

    std::vector<BSONObj> subSplits;
    for (unsigned i = 0; i <= allSplits.size(); i++) {
        if (i == allSplits.size() || !currentChunk->containsKey(allSplits[i])) {
            if (!subSplits.empty()) {
                auto splitStatus = shardutil::splitChunkAtMultiplePoints(
                    opCtx,
                    currentChunk->getShardId(),
                    nss,
                    chunkManager->getShardKeyPattern(),
                    chunkManager->getVersion(),
                    ChunkRange(currentChunk->getMin(), currentChunk->getMax()),
                    subSplits);
                if (!splitStatus.isOK()) {
                    warning() << "couldn't split chunk " << redact(currentChunk->toString())
                              << " while sharding collection " << nss.ns()
                              << causedBy(redact(splitStatus.getStatus()));
                }

                subSplits.clear();
            }

            if (i < allSplits.size()) {
                currentChunk = chunkManager->findIntersectingChunkWithSimpleCollation(allSplits[i]);
            }
        } else {
            BSONObj splitPoint(allSplits[i]);

            // Do not split on the boundaries
            if (currentChunk->getMin().woCompare(splitPoint) == 0) {
                continue;
            }

            subSplits.push_back(splitPoint);
        }
    }
}

boost::optional<UUID> getUUIDFromPrimaryShard(const NamespaceString& nss,
                                              ScopedDbConnection& conn) {
    // UUIDs were introduced in featureCompatibilityVersion 3.6.
    if (!serverGlobalParams.featureCompatibility.isSchemaVersion36()) {
        return boost::none;
    }

    // Obtain the collection's UUID from the primary shard's listCollections response.
    BSONObj res;
    {
        std::list<BSONObj> all =
            conn->getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));
        if (!all.empty()) {
            res = all.front().getOwned();
        }
    }

    uassert(ErrorCodes::InternalError,
            str::stream() << "expected the primary shard host " << conn.getHost()
                          << " for database "
                          << nss.db()
                          << " to return an entry for "
                          << nss.ns()
                          << " in its listCollections response, but it did not",
            !res.isEmpty());

    BSONObj collectionInfo;
    if (res["info"].type() == BSONType::Object) {
        collectionInfo = res["info"].Obj();
    }

    uassert(ErrorCodes::InternalError,
            str::stream() << "expected primary shard to return 'info' field as part of "
                             "listCollections for "
                          << nss.ns()
                          << " because the cluster is in featureCompatibilityVersion=3.6, but got "
                          << res,
            !collectionInfo.isEmpty());

    uassert(ErrorCodes::InternalError,
            str::stream() << "expected primary shard to return a UUID for collection " << nss.ns()
                          << " as part of 'info' field but got "
                          << res,
            collectionInfo.hasField("uuid"));

    return uassertStatusOK(UUID::parse(collectionInfo["uuid"]));
}

/**
 * Internal sharding command run on config servers to add a shard to the cluster.
 */
class ConfigSvrShardCollectionCommand : public BasicCommand {
public:
    ConfigSvrShardCollectionCommand() : BasicCommand("_configsvrShardCollection") {}

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool slaveOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    void help(std::stringstream& help) const override {
        help << "Internal command, which is exported by the sharding config server. Do not call "
             << "directly. Shards a collection. Requires key. Optional unique. Sharding must "
                "already be enabled for the database";
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {

        uassert(ErrorCodes::IllegalOperation,
                "_configsvrShardCollection can only be run on config servers",
                serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "shardCollection must be called with majority writeConcern, got "
                              << cmdObj,
                opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

        // Do not allow sharding collections while a featureCompatibilityVersion upgrade or
        // downgrade is in progress (see SERVER-31231 for details).
        Lock::ExclusiveLock lk(opCtx->lockState(), FeatureCompatibilityVersion::fcvLock);

        const NamespaceString nss(parseNs(dbname, cmdObj));
        auto request = ConfigsvrShardCollectionRequest::parse(
            IDLParserErrorContext("ConfigsvrShardCollectionRequest"), cmdObj);

        auto const catalogManager = ShardingCatalogManager::get(opCtx);
        auto const catalogCache = Grid::get(opCtx)->catalogCache();
        auto const catalogClient = Grid::get(opCtx)->catalogClient();

        // Make the distlocks boost::optional so that they can be released by being reset below.
        // Remove the backwards compatible lock after 3.6 ships.
        boost::optional<DistLockManager::ScopedDistLock> backwardsCompatibleDbDistLock(
            uassertStatusOK(
                catalogClient->getDistLockManager()->lock(opCtx,
                                                          nss.db() + "-movePrimary",
                                                          "shardCollection",
                                                          DistLockManager::kDefaultLockTimeout)));
        boost::optional<DistLockManager::ScopedDistLock> dbDistLock(
            uassertStatusOK(catalogClient->getDistLockManager()->lock(
                opCtx, nss.db(), "shardCollection", DistLockManager::kDefaultLockTimeout)));
        boost::optional<DistLockManager::ScopedDistLock> collDistLock(
            uassertStatusOK(catalogClient->getDistLockManager()->lock(
                opCtx, nss.ns(), "shardCollection", DistLockManager::kDefaultLockTimeout)));

        // Ensure sharding is allowed on the database.
        // Until all metadata commands are on the config server, the CatalogCache on the config
        // server may be stale. Read the database entry directly rather than purging and reloading
        // the database into the CatalogCache, which is very expensive.
        auto dbType =
            uassertStatusOK(
                Grid::get(opCtx)->catalogClient()->getDatabase(
                    opCtx, nss.db().toString(), repl::ReadConcernLevel::kLocalReadConcern))
                .value;
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "sharding not enabled for db " << nss.db(),
                dbType.getSharded());

        // Get variables required throughout this command.

        auto proposedKey(request.getKey().getOwned());
        ShardKeyPattern shardKeyPattern(proposedKey);

        std::vector<ShardId> shardIds;
        Grid::get(opCtx)->shardRegistry()->getAllShardIds(&shardIds);
        const int numShards = shardIds.size();

        uassert(ErrorCodes::IllegalOperation,
                "cannot shard collections before there are shards",
                numShards > 0);

        // Handle collections in the config db separately.
        if (nss.db() == NamespaceString::kConfigDb) {
            // Only whitelisted collections in config may be sharded
            // (unless we are in test mode)
            uassert(ErrorCodes::IllegalOperation,
                    "only special collections in the config db may be sharded",
                    nss.ns() == SessionsCollection::kSessionsFullNS ||
                        Command::testCommandsEnabled);

            auto configShard = uassertStatusOK(
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, dbType.getPrimary()));
            ScopedDbConnection configConn(configShard->getConnString());
            ON_BLOCK_EXIT([&configConn] { configConn.done(); });

            // If this is a collection on the config db, it must be empty to be sharded,
            // otherwise we might end up with chunks on the config servers.
            uassert(ErrorCodes::IllegalOperation,
                    "collections in the config db must be empty to be sharded",
                    configConn->count(nss.ns()) == 0);
        }

        // For the config db, pick a new host shard for this collection, otherwise
        // make a connection to the real primary shard for this database.
        auto primaryShardId = [&]() {
            if (nss.db() == NamespaceString::kConfigDb) {
                return shardIds[0];
            } else {
                return dbType.getPrimary();
            }
        }();

        auto primaryShard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, primaryShardId));
        ScopedDbConnection conn(primaryShard->getConnString());
        ON_BLOCK_EXIT([&conn] { conn.done(); });

        // Step 1.
        validateAndDeduceFullRequestOptions(opCtx, nss, shardKeyPattern, numShards, conn, &request);

        // The collation option should have been set to the collection default collation after being
        // validated.
        invariant(request.getCollation());

        // Step 2.
        if (auto existingColl = checkIfAlreadyShardedWithSameOptions(opCtx, nss, request)) {
            result << "collectionsharded" << nss.ns();
            if (existingColl->getUUID()) {
                result << "collectionUUID" << *existingColl->getUUID();
            }
            return true;
        }

        // Step 3.
        validateShardKeyAgainstExistingIndexes(
            opCtx, nss, proposedKey, shardKeyPattern, primaryShard, conn, request);

        // Step 4.
        auto uuid = getUUIDFromPrimaryShard(nss, conn);

        // isEmpty is used by multiple steps below.
        bool isEmpty = (conn->count(nss.ns()) == 0);

        // Step 5.
        std::vector<BSONObj> initSplits;  // there will be at most numShards-1 of these
        std::vector<BSONObj> allSplits;   // all of the initial desired split points
        determinePresplittingPoints(opCtx,
                                    numShards,
                                    isEmpty,
                                    proposedKey,
                                    shardKeyPattern,
                                    request,
                                    &initSplits,
                                    &allSplits);

        LOG(0) << "CMD: shardcollection: " << cmdObj;

        audit::logShardCollection(Client::getCurrent(), nss.ns(), proposedKey, request.getUnique());

        // The initial chunks are distributed evenly across shards only if the initial split points
        // were specified in the request, i.e., by mapReduce. Otherwise, all the initial chunks are
        // placed on the primary shard, and may be distributed across shards through migrations
        // (below) if using a hashed shard key.
        const bool distributeInitialChunks = request.getInitialSplitPoints().is_initialized();

        // Step 6. Actually shard the collection.
        catalogManager->shardCollection(opCtx,
                                        nss.ns(),
                                        uuid,
                                        shardKeyPattern,
                                        *request.getCollation(),
                                        request.getUnique(),
                                        initSplits,
                                        distributeInitialChunks,
                                        primaryShardId);
        result << "collectionsharded" << nss.ns();
        if (uuid) {
            result << "collectionUUID" << *uuid;
        }

        // Make sure the cached metadata for the collection knows that we are now sharded
        catalogCache->invalidateShardedCollection(nss);

        // Free the distlocks to allow the splits and migrations below to proceed.
        collDistLock.reset();
        dbDistLock.reset();
        backwardsCompatibleDbDistLock.reset();

        // Step 7. Migrate initial chunks to distribute them across shards.
        migrateAndFurtherSplitInitialChunks(
            opCtx, nss, numShards, shardIds, isEmpty, shardKeyPattern, allSplits);

        return true;
    }

} configsvrShardCollectionCmd;

}  // namespace
}  // namespace mongo
