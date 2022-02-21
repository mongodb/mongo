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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/config_server_client.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

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
    return appendAllowImplicitCreate(createIndexes.obj(), true);
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
                                         const std::shared_ptr<Shard>& primaryShard,
                                         ConfigsvrShardCollectionRequest* request) {
    uassert(
        ErrorCodes::InvalidOptions, "cannot have empty shard key", !request->getKey().isEmpty());

    // Ensure that hashed and unique are not both set.
    uassert(ErrorCodes::InvalidOptions,
            "Hashed shard keys cannot be declared unique. It's possible to ensure uniqueness on "
            "the hashed field by declaring an additional (non-hashed) unique index on the field.",
            !shardKeyPattern.isHashedPattern() || !request->getUnique());

    // Ensure the namespace is valid.
    uassert(ErrorCodes::IllegalOperation,
            "can't shard system namespaces",
            !nss.isSystem() || nss == NamespaceString::kLogicalSessionsNamespace);

    // Ensure the collation is valid. Currently we only allow the simple collation.
    bool simpleCollationSpecified = false;
    if (request->getCollation()) {
        auto& collation = *request->getCollation();
        auto collator = uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
        uassert(ErrorCodes::BadValue,
                str::stream() << "The collation for shardCollection must be {locale: 'simple'}, "
                              << "but found: " << collation,
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
                          << maxNumInitialChunksForShards << ", 8192 * number of shards; or "
                          << maxNumInitialChunksTotal,
            numChunks >= 0 && numChunks <= maxNumInitialChunksForShards &&
                numChunks <= maxNumInitialChunksTotal);

    // Retrieve the collection metadata in order to verify that it is legal to shard this
    // collection.
    BSONObj res;
    {
        auto listCollectionsCmd =
            BSON("listCollections" << 1 << "filter" << BSON("name" << nss.coll()));
        auto allRes = uassertStatusOK(primaryShard->runExhaustiveCursorCommand(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            nss.db().toString(),
            listCollectionsCmd,
            Milliseconds(-1)));
        const auto& all = allRes.docs;
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
 * Compares the proposed shard key with the collection's existing indexes on the primary shard to
 * ensure they are a legal combination.
 *
 * If the collection is empty and no index on the shard key exists, creates the required index.
 */
void validateShardKeyAgainstExistingIndexes(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const BSONObj& proposedKey,
                                            const ShardKeyPattern& shardKeyPattern,
                                            const std::shared_ptr<Shard>& primaryShard,
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

    auto listIndexesCmd = BSON("listIndexes" << nss.coll());
    auto indexesRes =
        primaryShard->runExhaustiveCursorCommand(opCtx,
                                                 ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                                 nss.db().toString(),
                                                 listIndexesCmd,
                                                 Milliseconds(-1));
    std::vector<BSONObj> indexes;
    if (indexesRes.getStatus().code() != ErrorCodes::NamespaceNotFound) {
        indexes = uassertStatusOK(indexesRes).docs;
    }

    // 1.  Verify consistency with existing unique indexes
    for (const auto& idx : indexes) {
        BSONObj currentKey = idx["key"].embeddedObject();
        bool isUnique = idx["unique"].trueValue();
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "can't shard collection '" << nss.ns() << "' with unique index on "
                              << currentKey << " and proposed shard key " << proposedKey
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
                                  << " with hashed shard key " << proposedKey
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
            bool isCurrentID = (currKey.firstElementFieldNameStringData() == "_id");
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "can't shard collection " << nss.ns() << ", " << proposedKey
                                  << " index not unique, and unique index explicitly specified",
                    isExplicitlyUnique || isCurrentID);
        }
    }

    auto countCmd = BSON("count" << nss.coll());
    auto countRes =
        uassertStatusOK(primaryShard->runCommand(opCtx,
                                                 ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                                 nss.db().toString(),
                                                 countCmd,
                                                 Shard::RetryPolicy::kIdempotent));
    const bool isEmpty = (countRes.response["n"].Int() == 0);

    if (hasUsefulIndexForKey) {
        // Check 2.iii and 2.iv. Make sure no null entries in the sharding index
        // and that there is a useful, non-multikey index available
        BSONObjBuilder checkShardingIndexCmd;
        checkShardingIndexCmd.append("checkShardingIndex", nss.ns());
        checkShardingIndexCmd.append("keyPattern", proposedKey);
        auto checkShardingIndexRes = uassertStatusOK(
            primaryShard->runCommand(opCtx,
                                     ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                     "admin",
                                     checkShardingIndexCmd.obj(),
                                     Shard::RetryPolicy::kIdempotent));
        uassert(ErrorCodes::OperationFailed,
                checkShardingIndexRes.response["errmsg"].str(),
                checkShardingIndexRes.commandStatus == Status::OK());
    } else if (!isEmpty) {
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
 * Migrates the initial "big chunks" from the primary shard to spread them evenly across the shards.
 *
 * If 'finalSplitPoints' is not empty, additionally splits each "big chunk" into smaller chunks
 * using the points in 'finalSplitPoints.'
 */
void migrateAndFurtherSplitInitialChunks(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const std::vector<ShardId>& shardIds,
                                         const std::vector<BSONObj>& finalSplitPoints) {
    const auto catalogCache = Grid::get(opCtx)->catalogCache();

    auto routingInfo = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection was successfully written as sharded but got dropped before it "
            "could be evenly distributed",
            routingInfo.cm());

    auto chunkManager = routingInfo.cm();

    // Move and commit each "big chunk" to a different shard.
    auto nextShardId = [&, indx = 0]() mutable { return shardIds[indx++ % shardIds.size()]; };

    for (auto chunk : chunkManager->chunks()) {
        const auto shardId = nextShardId();

        const auto toStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
        if (!toStatus.isOK()) {
            continue;
        }

        const auto to = toStatus.getValue();

        // Can't move chunk to shard it's already on
        if (to->getId() == chunk.getShardId()) {
            continue;
        }

        ChunkType chunkType;
        chunkType.setNS(nss);
        chunkType.setMin(chunk.getMin());
        chunkType.setMax(chunk.getMax());
        chunkType.setShard(chunk.getShardId());
        chunkType.setVersion(chunkManager->getVersion());

        Status moveStatus = configsvr_client::moveChunk(
            opCtx,
            chunkType,
            to->getId(),
            Grid::get(opCtx)->getBalancerConfiguration()->getMaxChunkSizeBytes(),
            MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
            true);
        if (!moveStatus.isOK()) {
            warning() << "couldn't move chunk " << redact(chunk.toString()) << " to shard " << *to
                      << " while sharding collection " << nss.ns() << causedBy(redact(moveStatus));
        }
    }

    if (finalSplitPoints.empty()) {
        return;
    }

    // Reload the config info, after all the migrations
    routingInfo = uassertStatusOK(catalogCache->getCollectionRoutingInfoWithRefresh(opCtx, nss));
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection was successfully written as sharded but got dropped before it "
            "could be evenly distributed",
            routingInfo.cm());
    chunkManager = routingInfo.cm();

    // Subdivide the big chunks by splitting at each of the points in "finalSplitPoints"
    // that we haven't already split by.
    boost::optional<Chunk> currentChunk(
        chunkManager->findIntersectingChunkWithSimpleCollation(finalSplitPoints[0]));

    std::vector<BSONObj> subSplits;
    for (unsigned i = 0; i <= finalSplitPoints.size(); i++) {
        if (i == finalSplitPoints.size() || !currentChunk->containsKey(finalSplitPoints[i])) {
            if (!subSplits.empty()) {
                auto splitStatus = shardutil::splitChunkAtMultiplePoints(
                    opCtx,
                    currentChunk->getShardId(),
                    nss,
                    chunkManager->getShardKeyPattern(),
                    chunkManager->getVersion(),
                    ChunkRange(currentChunk->getMin(), currentChunk->getMax()),
                    &subSplits);
                if (!splitStatus.isOK()) {
                    warning() << "couldn't split chunk " << redact(currentChunk->toString())
                              << " while sharding collection " << nss.ns()
                              << causedBy(redact(splitStatus.getStatus()));
                }

                subSplits.clear();
            }

            if (i < finalSplitPoints.size()) {
                currentChunk.emplace(
                    chunkManager->findIntersectingChunkWithSimpleCollation(finalSplitPoints[i]));
            }
        } else {
            BSONObj splitPoint(finalSplitPoints[i]);

            // Do not split on the boundaries
            if (currentChunk->getMin().woCompare(splitPoint) == 0) {
                continue;
            }

            subSplits.push_back(splitPoint);
        }
    }
}
boost::optional<UUID> getUUIDFromPrimaryShard(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const std::shared_ptr<Shard>& primaryShard) {
    // Obtain the collection's UUID from the primary shard's listCollections response.
    BSONObj res;
    {
        auto listCollectionsCmd =
            BSON("listCollections" << 1 << "filter" << BSON("name" << nss.coll()));
        auto allRes = uassertStatusOK(primaryShard->runExhaustiveCursorCommand(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            nss.db().toString(),
            listCollectionsCmd,
            Milliseconds(-1)));
        const auto& all = allRes.docs;
        if (!all.empty()) {
            res = all.front().getOwned();
        }
    }

    uassert(ErrorCodes::InternalError,
            str::stream() << "expected the primary shard host " << primaryShard->getConnString()
                          << " for database " << nss.db() << " to return an entry for " << nss.ns()
                          << " in its listCollections response, but it did not",
            !res.isEmpty());

    BSONObj collectionInfo;
    if (res["info"].type() == BSONType::Object) {
        collectionInfo = res["info"].Obj();
    }

    uassert(ErrorCodes::InternalError,
            str::stream() << "expected primary shard to return 'info' field as part of "
                             "listCollections for "
                          << nss.ns() << ", but got " << res,
            !collectionInfo.isEmpty());

    uassert(ErrorCodes::InternalError,
            str::stream() << "expected primary shard to return a UUID for collection " << nss.ns()
                          << " as part of 'info' field but got " << res,
            collectionInfo.hasField("uuid"));

    return uassertStatusOK(UUID::parse(collectionInfo["uuid"]));
}

/**
 * Internal sharding command run on config servers to shard a collection.
 */
class ConfigSvrShardCollectionCommand : public BasicCommand {
public:
    ConfigSvrShardCollectionCommand() : BasicCommand("_configsvrShardCollection") {}

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Shards a collection. Requires key. Optional unique. Sharding must "
               "already be enabled for the database";
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {

        uassert(ErrorCodes::IllegalOperation,
                "_configsvrShardCollection can only be run on config servers",
                serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

        // Set the operation context read concern level to local for reads into the config database.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "shardCollection must be called with majority writeConcern, got "
                              << cmdObj,
                opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

        const NamespaceString nss(parseNs(dbname, cmdObj));
        auto request = ConfigsvrShardCollectionRequest::parse(
            IDLParserErrorContext("ConfigsvrShardCollectionRequest"), cmdObj);

        auto const catalogManager = ShardingCatalogManager::get(opCtx);
        auto const catalogCache = Grid::get(opCtx)->catalogCache();
        auto const catalogClient = Grid::get(opCtx)->catalogClient();
        auto shardRegistry = Grid::get(opCtx)->shardRegistry();

        // Make the distlocks boost::optional so that they can be released by being reset below.
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
                    opCtx, nss.db().toString(), repl::ReadConcernArgs::get(opCtx).getLevel()))
                .value;
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "sharding not enabled for db " << nss.db(),
                dbType.getSharded());

        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Namespace too long. Namespace: " << nss
                              << " Max: " << NamespaceString::MaxNsShardedCollectionLen,
                nss.size() <= NamespaceString::MaxNsShardedCollectionLen);

        // Get variables required throughout this command.

        auto proposedKey(request.getKey().getOwned());
        ShardKeyPattern shardKeyPattern(proposedKey);

        std::vector<ShardId> shardIds;
        shardRegistry->getAllShardIds(opCtx, &shardIds);
        uassert(ErrorCodes::IllegalOperation,
                "cannot shard collections before there are shards",
                !shardIds.empty());

        // Handle collections in the config db separately.
        if (nss.db() == NamespaceString::kConfigDb) {
            // Only whitelisted collections in config may be sharded (unless we are in test mode)
            uassert(ErrorCodes::IllegalOperation,
                    "only special collections in the config db may be sharded",
                    nss == NamespaceString::kLogicalSessionsNamespace);

            auto configShard = uassertStatusOK(shardRegistry->getShard(opCtx, dbType.getPrimary()));
            auto countCmd = BSON("count" << nss.coll());
            auto countRes = uassertStatusOK(
                configShard->runCommand(opCtx,
                                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                        nss.db().toString(),
                                        countCmd,
                                        Shard::RetryPolicy::kIdempotent));
            auto numDocs = countRes.response["n"].Int();

            // If this is a collection on the config db, it must be empty to be sharded,
            // otherwise we might end up with chunks on the config servers.
            uassert(ErrorCodes::IllegalOperation,
                    "collections in the config db must be empty to be sharded",
                    numDocs == 0);
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

        auto primaryShard = uassertStatusOK(shardRegistry->getShard(opCtx, primaryShardId));

        // Step 1.
        validateAndDeduceFullRequestOptions(
            opCtx, nss, shardKeyPattern, shardIds.size(), primaryShard, &request);

        // The collation option should have been set to the collection default collation after being
        // validated.
        invariant(request.getCollation());

        boost::optional<UUID> uuid;

        // The primary shard will read the config.tags collection so we need to lock the zone
        // mutex.
        Lock::ExclusiveLock lk = catalogManager->lockZoneMutex(opCtx);

        ShardsvrShardCollection shardsvrShardCollectionRequest;
        shardsvrShardCollectionRequest.set_shardsvrShardCollection(nss);
        shardsvrShardCollectionRequest.setKey(request.getKey());
        shardsvrShardCollectionRequest.setUnique(request.getUnique());
        shardsvrShardCollectionRequest.setNumInitialChunks(request.getNumInitialChunks());
        shardsvrShardCollectionRequest.setInitialSplitPoints(request.getInitialSplitPoints());
        shardsvrShardCollectionRequest.setCollation(request.getCollation());
        shardsvrShardCollectionRequest.setGetUUIDfromPrimaryShard(
            request.getGetUUIDfromPrimaryShard());

        auto cmdResponse = uassertStatusOK(primaryShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            "admin",
            CommandHelpers::appendMajorityWriteConcern(CommandHelpers::appendPassthroughFields(
                cmdObj, shardsvrShardCollectionRequest.toBSON())),
            Shard::RetryPolicy::kIdempotent));

        if (cmdResponse.commandStatus != ErrorCodes::CommandNotFound) {
            uassertStatusOK(cmdResponse.commandStatus);

            auto shardCollResponse = ShardsvrShardCollectionResponse::parse(
                IDLParserErrorContext("ShardsvrShardCollectionResponse"), cmdResponse.response);
            auto uuid = std::move(shardCollResponse.getCollectionUUID());

            result << "collectionsharded" << nss.ns();
            if (uuid) {
                result << "collectionUUID" << *uuid;
            }

            auto routingInfo =
                uassertStatusOK(catalogCache->getCollectionRoutingInfoWithRefresh(opCtx, nss));
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    "Collection was successfully written as sharded but got dropped before it "
                    "could be evenly distributed",
                    routingInfo.cm());

            return true;
        } else {
            // Step 2.
            if (auto existingColl =
                    InitialSplitPolicy::checkIfCollectionAlreadyShardedWithSameOptions(
                        opCtx,
                        nss,
                        shardsvrShardCollectionRequest,
                        repl::ReadConcernLevel::kLocalReadConcern)) {
                result << "collectionsharded" << nss.ns();
                if (existingColl->getUUID()) {
                    result << "collectionUUID" << *existingColl->getUUID();
                }
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
                return true;
            }

            // This check for empty collection is racy, because it is not guaranteed that documents
            // will not show up in the collection right after the count below has executed. It is
            // left here for backwards compatiblity with pre-4.0.4 clusters, which do not support
            // sharding being performed by the primary shard.
            auto countCmd = BSON("count" << nss.coll());
            auto countRes = uassertStatusOK(
                primaryShard->runCommand(opCtx,
                                         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                         nss.db().toString(),
                                         countCmd,
                                         Shard::RetryPolicy::kIdempotent));

            const bool isEmpty = (countRes.response["n"].Int() == 0);

            // Map/reduce with output to an empty collection assumes it has full control of the
            // output collection and it would be an unsupported operation if the collection is being
            // concurrently written
            const bool fromMapReduce = bool(request.getInitialSplitPoints());
            if (fromMapReduce) {
                uassert(ErrorCodes::ConflictingOperationInProgress,
                        str::stream() << "Map reduce with sharded output to a new collection found "
                                      << nss.ns() << " to be non-empty which is not supported.",
                        isEmpty);
            }

            // Step 3.
            validateShardKeyAgainstExistingIndexes(
                opCtx, nss, proposedKey, shardKeyPattern, primaryShard, request);

            // Step 4.
            if (request.getGetUUIDfromPrimaryShard()) {
                uuid = getUUIDFromPrimaryShard(opCtx, nss, primaryShard);
            } else {
                uuid = UUID::gen();
            }

            // Step 5.
            std::vector<BSONObj> initialSplitPoints;  // there will be at most numShards-1 of these
            std::vector<BSONObj> finalSplitPoints;    // all of the desired split points
            if (request.getInitialSplitPoints()) {
                initialSplitPoints = *request.getInitialSplitPoints();
            } else {
                InitialSplitPolicy::calculateHashedSplitPointsForEmptyCollection(
                    shardKeyPattern,
                    isEmpty,
                    shardIds.size(),
                    request.getNumInitialChunks(),
                    &initialSplitPoints,
                    &finalSplitPoints);
            }

            LOG(0) << "CMD: shardcollection: " << cmdObj;

            audit::logShardCollection(
                opCtx->getClient(), nss.ns(), proposedKey, request.getUnique());

            // Step 6. Actually shard the collection.
            catalogManager->shardCollection(opCtx,
                                            nss,
                                            uuid,
                                            shardKeyPattern,
                                            *request.getCollation(),
                                            request.getUnique(),
                                            initialSplitPoints,
                                            fromMapReduce,
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

            // Step 7. If the collection is empty and using hashed sharding, migrate initial chunks
            // to spread them evenly across shards from the beginning. Otherwise rely on the
            // balancer to do it.
            if (isEmpty && shardKeyPattern.isHashedPattern()) {
                migrateAndFurtherSplitInitialChunks(opCtx, nss, shardIds, finalSplitPoints);
            }

            return true;
        }
    }

} configsvrShardCollectionCmd;

}  // namespace
}  // namespace mongo
