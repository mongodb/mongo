/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_key_util.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace shardkeyutil {

namespace {

constexpr StringData kCheckShardingIndexCmdName = "checkShardingIndex"_sd;
constexpr StringData kKeyPatternField = "keyPattern"_sd;

/**
 * Constructs the BSON specification document for the create indexes command using the given
 * namespace, index key and options.
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
            indexName << currentKey.str();  // This should match up with shell command.
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

}  // namespace

void validateShardKeyIndexExistsOrCreateIfPossible(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const BSONObj& proposedKey,
                                                   const ShardKeyPattern& shardKeyPattern,
                                                   const boost::optional<BSONObj>& defaultCollation,
                                                   bool unique,
                                                   const ShardKeyValidationBehaviors& behaviors) {
    auto indexes = behaviors.loadIndexes(nss);

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
            // Check iv.
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
    if (hasUsefulIndexForKey && unique) {
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

    if (hasUsefulIndexForKey) {
        // Check 2.iii Make sure that there is a useful, non-multikey index available.
        behaviors.verifyUsefulNonMultiKeyIndex(nss, proposedKey);
        return;
    }

    // 4. If no useful index, verify we can create one.
    behaviors.verifyCanCreateShardKeyIndex(nss);

    // 5. If no useful index exists and we can create one, create one on proposedKey. Only need
    //    to call ensureIndex on primary shard, since indexes get copied to receiving shard
    //    whenever a migrate occurs. If the collection has a default collation, explicitly send
    //    the simple collation as part of the createIndex request.
    behaviors.createShardKeyIndex(nss, proposedKey, defaultCollation, unique);
}

std::vector<BSONObj> ValidationBehaviorsShardCollection::loadIndexes(
    const NamespaceString& nss) const {
    std::list<BSONObj> indexes = _localClient->getIndexSpecs(nss);
    // Convert std::list to a std::vector.
    return std::vector<BSONObj>{std::make_move_iterator(std::begin(indexes)),
                                std::make_move_iterator(std::end(indexes))};
}

void ValidationBehaviorsShardCollection::verifyUsefulNonMultiKeyIndex(
    const NamespaceString& nss, const BSONObj& proposedKey) const {
    BSONObj res;
    auto success = _localClient->runCommand(
        "admin",
        BSON(kCheckShardingIndexCmdName << nss.ns() << kKeyPatternField << proposedKey),
        res);
    uassert(ErrorCodes::OperationFailed, res["errmsg"].str(), success);
}

void ValidationBehaviorsShardCollection::verifyCanCreateShardKeyIndex(
    const NamespaceString& nss) const {
    uassert(ErrorCodes::InvalidOptions,
            "Please create an index that starts with the proposed shard key before "
            "sharding the collection",
            _localClient->findOne(nss.ns(), Query()).isEmpty());
}

void ValidationBehaviorsShardCollection::createShardKeyIndex(
    const NamespaceString& nss,
    const BSONObj& proposedKey,
    const boost::optional<BSONObj>& defaultCollation,
    bool unique) const {
    BSONObj collation =
        defaultCollation && !defaultCollation->isEmpty() ? CollationSpec::kSimpleSpec : BSONObj();
    auto createIndexesCmd = makeCreateIndexesCmd(nss, proposedKey, collation, unique);
    BSONObj res;
    _localClient->runCommand(nss.db().toString(), createIndexesCmd, res);
    uassertStatusOK(getStatusFromCommandResult(res));
}

ValidationBehaviorsRefineShardKey::ValidationBehaviorsRefineShardKey(OperationContext* opCtx,
                                                                     const NamespaceString& nss)
    : _opCtx(opCtx) {
    auto routingInfo = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx, nss));
    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "refineCollectionShardKey namespace " << nss.toString()
                          << " is not sharded",
            routingInfo.cm());
    const auto minKeyShardId = routingInfo.cm()->getMinKeyShardIdWithSimpleCollation();
    _indexShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, minKeyShardId));
    _cm = routingInfo.cm();
}

std::vector<BSONObj> ValidationBehaviorsRefineShardKey::loadIndexes(
    const NamespaceString& nss) const {
    auto indexesRes = _indexShard->runExhaustiveCursorCommand(
        _opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        nss.db().toString(),
        appendShardVersion(BSON("listIndexes" << nss.coll()),
                           _cm->getVersion(_indexShard->getId())),
        Milliseconds(-1));
    if (indexesRes.getStatus().code() != ErrorCodes::NamespaceNotFound) {
        return uassertStatusOK(indexesRes).docs;
    }
    return {};
}

void ValidationBehaviorsRefineShardKey::verifyUsefulNonMultiKeyIndex(
    const NamespaceString& nss, const BSONObj& proposedKey) const {
    auto checkShardingIndexRes = uassertStatusOK(_indexShard->runCommand(
        _opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        "admin",
        appendShardVersion(
            BSON(kCheckShardingIndexCmdName << nss.ns() << kKeyPatternField << proposedKey),
            _cm->getVersion(_indexShard->getId())),
        Shard::RetryPolicy::kIdempotent));
    if (checkShardingIndexRes.commandStatus == ErrorCodes::UnknownError) {
        // CheckShardingIndex returns UnknownError if a compatible shard key index cannot be found,
        // but we return OperationFailed to correspond with the shardCollection behavior.
        uasserted(ErrorCodes::OperationFailed, checkShardingIndexRes.response["errmsg"].str());
    }
    // Rethrow any other error to allow retries on retryable errors.
    uassertStatusOK(checkShardingIndexRes.commandStatus);
}

void ValidationBehaviorsRefineShardKey::verifyCanCreateShardKeyIndex(
    const NamespaceString& nss) const {
    uasserted(ErrorCodes::InvalidOptions,
              "Please create an index that starts with the proposed shard key before "
              "refining the shard key of the collection");
}

void ValidationBehaviorsRefineShardKey::createShardKeyIndex(
    const NamespaceString& nss,
    const BSONObj& proposedKey,
    const boost::optional<BSONObj>& defaultCollation,
    bool unique) const {
    MONGO_UNREACHABLE;
}

}  // namespace shardkeyutil
}  // namespace mongo
