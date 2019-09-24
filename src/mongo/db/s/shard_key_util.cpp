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

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/s/shard_key_util.h"
#include "mongo/s/cluster_commands_helpers.h"

namespace mongo {
namespace shardkeyutil {

BSONObj makeCreateIndexesCmd(const NamespaceString& nss,
                             const BSONObj& keys,
                             const BSONObj& collation,
                             const bool unique) {
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
    return appendAllowImplicitCreate(createIndexes.obj(), false);
}

void validateShardKeyAgainstExistingIndexes(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const BSONObj& proposedKey,
                                            const ShardKeyPattern& shardKeyPattern,
                                            const std::shared_ptr<Shard>& primaryShard,
                                            const boost::optional<BSONObj>& defaultCollation,
                                            const bool unique,
                                            const bool createIndexIfPossible) {
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

    auto countCmd = BSON("count" << nss.coll());
    auto countRes =
        uassertStatusOK(primaryShard->runCommand(opCtx,
                                                 ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                                 nss.db().toString(),
                                                 countCmd,
                                                 Shard::RetryPolicy::kIdempotent));
    const bool isEmpty = (countRes.response["n"].Int() == 0);

    if (hasUsefulIndexForKey) {
        // Check 2.iii Make sure that there is a useful, non-multikey index available.
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
    } else if (!isEmpty || !createIndexIfPossible) {
        // 4. If no useful index, and collection is non-empty or createIndexIfPossible is false,
        //    fail
        uasserted(ErrorCodes::InvalidOptions,
                  "Please create an index that starts with the proposed shard key before "
                  "sharding the collection");
    } else {
        // 5. If no useful index exists, and collection empty and createIndexIfPossible is true,
        //    create one on proposedKey. Only need to call ensureIndex on primary shard, since
        //    indexes get copied to receiving shard whenever a migrate occurs. If the collection
        //    has a default collation, explicitly send the simple collation as part of the
        //    createIndex request.
        invariant(createIndexIfPossible);

        BSONObj collation = !defaultCollation->isEmpty() ? CollationSpec::kSimpleSpec : BSONObj();
        auto createIndexesCmd = makeCreateIndexesCmd(nss, proposedKey, collation, unique);

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

}  // namespace shardkeyutil
}  // namespace mongo
