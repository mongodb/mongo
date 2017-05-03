/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <list>
#include <set>
#include <vector>

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_write.h"
#include "mongo/s/config_server_client.h"
#include "mongo/s/grid.h"
#include "mongo/s/migration_secondary_throttle_options.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

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

class ShardCollectionCmd : public Command {
public:
    ShardCollectionCmd() : Command("shardCollection", false, "shardcollection") {}

    bool slaveOk() const override {
        return true;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void help(std::stringstream& help) const override {
        help << "Shard a collection. Requires key. Optional unique."
             << " Sharding must already be enabled for the database.\n"
             << "   { enablesharding : \"<dbname>\" }\n";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::enableSharding)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             BSONObj& cmdObj,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbname, cmdObj));

        auto const catalogClient = Grid::get(opCtx)->catalogClient(opCtx);
        auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
        auto const catalogCache = Grid::get(opCtx)->catalogCache();

        auto dbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, nss.db()));

        // Ensure sharding is allowed on the database
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "sharding not enabled for db " << nss.db(),
                dbInfo.shardingEnabled());

        auto routingInfo = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));

        // Ensure that the collection is not sharded already
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "sharding already enabled for collection " << nss.ns(),
                !routingInfo.cm());

        // NOTE: We *must* take ownership of the key here - otherwise the shared BSONObj becomes
        // corrupt as soon as the command ends.
        BSONObj proposedKey = cmdObj.getObjectField("key").getOwned();
        if (proposedKey.isEmpty()) {
            errmsg = "no shard key";
            return false;
        }

        ShardKeyPattern proposedKeyPattern(proposedKey);
        if (!proposedKeyPattern.isValid()) {
            errmsg = str::stream() << "Unsupported shard key pattern. Pattern must"
                                   << " either be a single hashed field, or a list"
                                   << " of ascending fields.";
            return false;
        }

        bool isHashedShardKey = proposedKeyPattern.isHashedPattern();

        if (isHashedShardKey && cmdObj["unique"].trueValue()) {
            dassert(proposedKey.nFields() == 1);

            // it's possible to ensure uniqueness on the hashed field by
            // declaring an additional (non-hashed) unique index on the field,
            // but the hashed shard key itself should not be declared unique
            errmsg = "hashed shard keys cannot be declared unique.";
            return false;
        }

        uassert(ErrorCodes::IllegalOperation, "can't shard system namespaces", !nss.isSystem());

        bool simpleCollationSpecified = false;
        {
            BSONElement collationElement;
            Status collationStatus =
                bsonExtractTypedField(cmdObj, "collation", BSONType::Object, &collationElement);
            if (collationStatus.isOK()) {
                // Ensure that the collation is valid. Currently we only allow the simple collation.
                auto collator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                    ->makeFromBSON(collationElement.Obj());
                if (!collator.getStatus().isOK()) {
                    return appendCommandStatus(result, collator.getStatus());
                }

                if (collator.getValue()) {
                    return appendCommandStatus(
                        result,
                        {ErrorCodes::BadValue,
                         str::stream()
                             << "The collation for shardCollection must be {locale: 'simple'}, "
                             << "but found: "
                             << collationElement.Obj()});
                }

                simpleCollationSpecified = true;
            } else if (collationStatus != ErrorCodes::NoSuchKey) {
                return appendCommandStatus(result, collationStatus);
            }
        }

        std::vector<ShardId> shardIds;
        shardRegistry->getAllShardIds(&shardIds);

        const int numShards = shardIds.size();

        // Cannot have more than 8192 initial chunks per shard. Setting a maximum of 1,000,000
        // chunks in total to limit the amount of memory this command consumes so there is less
        // danger of an OOM error.
        const int maxNumInitialChunksForShards = numShards * 8192;
        const int maxNumInitialChunksTotal = 1000 * 1000;  // Arbitrary limit to memory consumption
        int numChunks = cmdObj["numInitialChunks"].numberInt();
        if (numChunks > maxNumInitialChunksForShards || numChunks > maxNumInitialChunksTotal) {
            errmsg = str::stream()
                << "numInitialChunks cannot be more than either: " << maxNumInitialChunksForShards
                << ", 8192 * number of shards; or " << maxNumInitialChunksTotal;
            return false;
        }

        // The rest of the checks require a connection to the primary db
        auto primaryShard = routingInfo.primary();
        ScopedDbConnection conn(primaryShard->getConnString());

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
                auto status = bsonExtractStringField(res, "type", &namespaceType);
                if (!status.isOK()) {
                    conn.done();
                    return appendCommandStatus(result, status);
                }

                if (namespaceType == "view") {
                    conn.done();
                    return appendCommandStatus(
                        result,
                        {ErrorCodes::CommandNotSupportedOnView, "Views cannot be sharded."});
                }
            }

            BSONObj collectionOptions;
            if (res["options"].type() == BSONType::Object) {
                collectionOptions = res["options"].Obj();
            }

            // Check that collection is not capped.
            if (collectionOptions["capped"].trueValue()) {
                errmsg = "can't shard capped collection";
                conn.done();
                return false;
            }

            // Get collection default collation.
            {
                BSONElement collationElement;
                auto status = bsonExtractTypedField(
                    collectionOptions, "collation", BSONType::Object, &collationElement);
                if (status.isOK()) {
                    defaultCollation = collationElement.Obj().getOwned();
                    if (defaultCollation.isEmpty()) {
                        conn.done();
                        return appendCommandStatus(
                            result,
                            {ErrorCodes::BadValue,
                             "Default collation in collection metadata cannot be empty."});
                    }
                } else if (status != ErrorCodes::NoSuchKey) {
                    conn.done();
                    return appendCommandStatus(
                        result,
                        {status.code(),
                         str::stream()
                             << "Could not parse default collation in collection metadata "
                             << causedBy(status)});
                }
            }

            // If the collection has a non-simple default collation but the user did not specify the
            // simple collation explicitly, return an error.
            if (!defaultCollation.isEmpty() && !simpleCollationSpecified) {
                conn.done();
                return appendCommandStatus(result,
                                           {ErrorCodes::BadValue,
                                            str::stream()
                                                << "Collection has default collation: "
                                                << collectionOptions["collation"]
                                                << ". Must specify collation {locale: 'simple'}"});
            }
        }

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
        ShardKeyPattern proposedShardKey(proposedKey);

        for (const auto& idx : indexes) {
            BSONObj currentKey = idx["key"].embeddedObject();
            bool isUnique = idx["unique"].trueValue();

            if (isUnique && !proposedShardKey.isUniqueIndexCompatible(currentKey)) {
                errmsg = str::stream() << "can't shard collection '" << nss.ns() << "' "
                                       << "with unique index on " << currentKey << " "
                                       << "and proposed shard key " << proposedKey << ". "
                                       << "Uniqueness can't be maintained unless "
                                       << "shard key is a prefix";
                conn.done();
                return false;
            }
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
                if (isHashedShardKey && !idx["seed"].eoo() &&
                    idx["seed"].numberInt() != BSONElementHasher::DEFAULT_HASH_SEED) {
                    errmsg = str::stream() << "can't shard collection " << nss.ns()
                                           << " with hashed shard key " << proposedKey
                                           << " because the hashed index uses a non-default"
                                           << " seed of " << idx["seed"].numberInt();
                    conn.done();
                    return false;
                }

                hasUsefulIndexForKey = true;
            }
        }

        // 3. If proposed key is required to be unique, additionally check for exact match.
        bool careAboutUnique = cmdObj["unique"].trueValue();

        if (hasUsefulIndexForKey && careAboutUnique) {
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

                if (!isExplicitlyUnique && !isCurrentID) {
                    errmsg = str::stream() << "can't shard collection " << nss.ns() << ", "
                                           << proposedKey << " index not unique, "
                                           << "and unique index explicitly specified";
                    conn.done();
                    return false;
                }
            }
        }

        if (hasUsefulIndexForKey) {
            // Check 2.iii and 2.iv. Make sure no null entries in the sharding index
            // and that there is a useful, non-multikey index available
            BSONObjBuilder checkShardingIndexCmd;
            checkShardingIndexCmd.append("checkShardingIndex", nss.ns());
            checkShardingIndexCmd.append("keyPattern", proposedKey);

            if (!conn.get()->runCommand("admin", checkShardingIndexCmd.obj(), res)) {
                errmsg = res["errmsg"].str();
                conn.done();
                return false;
            }
        } else if (conn->count(nss.ns()) != 0) {
            // 4. if no useful index, and collection is non-empty, fail
            errmsg = str::stream() << "please create an index that starts with the "
                                   << "shard key before sharding.";
            result.append("proposedKey", proposedKey);
            result.append("curIndexes", indexes);
            conn.done();
            return false;
        } else {
            // 5. If no useful index exists, and collection empty, create one on proposedKey.
            //    Only need to call ensureIndex on primary shard, since indexes get copied to
            //    receiving shard whenever a migrate occurs.
            //    If the collection has a default collation, explicitly send the simple
            //    collation as part of the createIndex request.
            BSONObj collation =
                !defaultCollation.isEmpty() ? CollationSpec::kSimpleSpec : BSONObj();

            auto createIndexesCmd =
                makeCreateIndexesCmd(nss, proposedKey, collation, careAboutUnique);

            const auto swResponse = primaryShard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                nss.db().toString(),
                createIndexesCmd,
                Shard::RetryPolicy::kNotIdempotent);
            auto createIndexesStatus = swResponse.getStatus();
            if (createIndexesStatus.isOK()) {
                const auto response = swResponse.getValue();
                createIndexesStatus = (!response.commandStatus.isOK())
                    ? response.commandStatus
                    : response.writeConcernStatus;
            }
            if (!createIndexesStatus.isOK()) {
                errmsg = str::stream() << "ensureIndex failed to create index on "
                                       << "primary shard: " << createIndexesStatus.reason();
                conn.done();
                return false;
            }
        }

        bool isEmpty = (conn->count(nss.ns()) == 0);

        conn.done();

        // Pre-splitting:
        // For new collections which use hashed shard keys, we can can pre-split the
        // range of possible hashes into a large number of chunks, and distribute them
        // evenly at creation time. Until we design a better initialization scheme, the
        // safest way to pre-split is to
        // 1. make one big chunk for each shard
        // 2. move them one at a time
        // 3. split the big chunks to achieve the desired total number of initial chunks

        std::vector<BSONObj> initSplits;  // there will be at most numShards-1 of these
        std::vector<BSONObj> allSplits;   // all of the initial desired split points

        // only pre-split when using a hashed shard key and collection is still empty
        if (isHashedShardKey && isEmpty) {
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
                allSplits.push_back(BSON(proposedKey.firstElementFieldName() << current));
                current += intervalSize;
            } else {
                current += intervalSize / 2;
            }

            for (int i = 0; i < (numChunks - 1) / 2; i++) {
                allSplits.push_back(BSON(proposedKey.firstElementFieldName() << current));
                allSplits.push_back(BSON(proposedKey.firstElementFieldName() << -current));
                current += intervalSize;
            }

            sort(allSplits.begin(),
                 allSplits.end(),
                 SimpleBSONObjComparator::kInstance.makeLessThan());

            // 1. the initial splits define the "big chunks" that we will subdivide later
            int lastIndex = -1;
            for (int i = 1; i < numShards; i++) {
                if (lastIndex < (i * numChunks) / numShards - 1) {
                    lastIndex = (i * numChunks) / numShards - 1;
                    initSplits.push_back(allSplits[lastIndex]);
                }
            }
        } else if (numChunks > 0) {
            conn.done();
            return appendCommandStatus(
                result,
                {ErrorCodes::InvalidOptions,
                 str::stream() << (!isHashedShardKey ? "numInitialChunks is not supported "
                                                       "when the shard key is not hashed."
                                                     : "numInitialChunks is not supported "
                                                       "when the collection is not empty.")});
        }

        LOG(0) << "CMD: shardcollection: " << cmdObj;

        audit::logShardCollection(Client::getCurrent(), nss.ns(), proposedKey, careAboutUnique);

        uassertStatusOK(catalogClient->shardCollection(opCtx,
                                                       nss.ns(),
                                                       proposedShardKey,
                                                       defaultCollation,
                                                       careAboutUnique,
                                                       initSplits,
                                                       std::set<ShardId>{}));

        result << "collectionsharded" << nss.ns();

        // Make sure the cached metadata for the collection knows that we are now sharded
        catalogCache->invalidateShardedCollection(nss);

        // Only initially move chunks when using a hashed shard key
        if (isHashedShardKey && isEmpty) {
            routingInfo = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    "Collection was successfully written as sharded but got dropped before it "
                    "could be evenly distributed",
                    routingInfo.cm());
            auto chunkManager = routingInfo.cm();

            const auto chunkMap = chunkManager->chunkMap();

            // 2. Move and commit each "big chunk" to a different shard.
            int i = 0;
            for (ChunkMap::const_iterator c = chunkMap.begin(); c != chunkMap.end(); ++c, ++i) {
                const ShardId& shardId = shardIds[i % numShards];
                const auto toStatus = shardRegistry->getShard(opCtx, shardId);
                if (!toStatus.isOK()) {
                    continue;
                }
                const auto to = toStatus.getValue();

                auto chunk = c->second;

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
                    MigrationSecondaryThrottleOptions::create(
                        MigrationSecondaryThrottleOptions::kOff),
                    true);
                if (!moveStatus.isOK()) {
                    warning() << "couldn't move chunk " << redact(chunk->toString()) << " to shard "
                              << *to << " while sharding collection " << nss.ns()
                              << causedBy(redact(moveStatus));
                }
            }

            if (allSplits.empty()) {
                return true;
            }

            // Reload the config info, after all the migrations
            catalogCache->invalidateShardedCollection(nss);
            routingInfo = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    "Collection was successfully written as sharded but got dropped before it "
                    "could be evenly distributed",
                    routingInfo.cm());
            chunkManager = routingInfo.cm();

            // 3. Subdivide the big chunks by splitting at each of the points in "allSplits"
            //    that we haven't already split by.
            auto currentChunk =
                chunkManager->findIntersectingChunkWithSimpleCollation(allSplits[0]);

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
                        currentChunk =
                            chunkManager->findIntersectingChunkWithSimpleCollation(allSplits[i]);
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

        return true;
    }

} shardCollectionCmd;

}  // namespace
}  // namespace mongo
