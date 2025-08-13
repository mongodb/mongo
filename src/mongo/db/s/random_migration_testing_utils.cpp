/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/s/random_migration_testing_utils.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/shard_key_index_util.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration

namespace mongo {

namespace {

/*
 * Generates a random string between min and max alphabetically. If no document can be created,
 * returns boost::none.
 */
boost::optional<std::string> generateRandomStringBetween(const std::string& min,
                                                         const std::string& max,
                                                         int maxLength,
                                                         std::default_random_engine& gen) {
    std::string randomString;
    size_t i = 0;

    // First, copy all the letters that are the same between min and max.
    while (i < min.size() && min.at(i) == max.at(i)) {
        randomString.push_back(min[i++]);
    }
    if (i < min.size()) {
        if (max.at(i) - min.at(i) > 1) {
            // If there are letters between the next letter in min and max, choose a random one.
            std::uniform_int_distribution<int> dist(min.at(i) + 1, max.at(i) - 1);
            randomString.push_back(dist(gen));
        } else {
            // If not, use the letter from min and then add a random character.
            randomString.push_back(min[i++]);
            // Since we want vaguely human readable strings, cap the max at 126 which means if min
            // has 126, we can't choose one higher.
            while (i < min.size() && min.at(i) == 126) {
                randomString.push_back(126);
                i++;
            }
            // If there are characters left in min, make sure we choose one after it alphabetically.
            if (i < min.size()) {
                std::uniform_int_distribution<int> dist(min.at(i) + 1, 126);
                randomString.push_back(dist(gen));
            } else {
                std::uniform_int_distribution<int> dist(32, 126);
                randomString.push_back(dist(gen));
            }
        }
    } else {
        tassert(10587401,
                str::stream() << "Unexpected string comparison result between " << min << " and "
                              << max,
                max.size() > min.size());
        // Since we want vaguely human readable strings, cap the min at 32 which means if max has
        // 32, we can't choose one lower.
        while (i < max.size() && max.at(i) == 32) {
            randomString.push_back(32);
            i++;
        }
        if (i == max.size()) {
            return boost::none;
        }
        std::uniform_int_distribution<int> dist(32, max.at(i) - 1);
        randomString.push_back(dist(gen));
    }
    // Add some extra random letters for additional randomness.
    std::uniform_int_distribution<int> lengthDist(randomString.size(), maxLength);
    std::uniform_int_distribution<int> charDist(32, 126);
    size_t intendedLength = lengthDist(gen);
    while (intendedLength > randomString.size()) {
        randomString.push_back(charDist(gen));
    }
    return randomString;
}

/*
 * Generates a random document between min and max. If min is MinKey and max is MaxKey, returns 0 as
 * the split point. Otherwise, looks at the type of min or max and tries to generate a valid
 * document of that type.
 */
boost::optional<BSONObj> generateRandomDocument(const BSONObj& minDoc,
                                                const BSONObj& maxDoc,
                                                std::default_random_engine& gen) {
    std::vector<BSONElement> minElems;
    minDoc.elems(minElems);
    BSONObjBuilder randomDocument;
    for (const auto& minField : minElems) {
        const auto& name = minField.fieldName();
        const auto& maxField = maxDoc.getField(name);
        if (minField.type() == BSONType::minKey && maxField.type() == BSONType::maxKey) {
            // Since we don't know what type the shard key is, just use an int.
            randomDocument.appendNumber(name, 0);
        } else {
            BSONType type = minField.type() == BSONType::minKey ? maxField.type() : minField.type();
            switch (type) {
                case BSONType::numberInt: {
                    int min = minField.type() == BSONType::minKey ? std::numeric_limits<int>::min()
                                                                  : minField.numberInt();
                    int max = maxField.type() == BSONType::maxKey ? std::numeric_limits<int>::max()
                                                                  : maxField.numberInt();
                    if (max <= std::numeric_limits<int>::min() + 2 || max - 2 < min) {
                        return boost::none;
                    }
                    std::uniform_int_distribution<int> dist(min + 1, max - 1);
                    randomDocument.appendNumber(name, dist(gen));
                    break;
                }
                case BSONType::numberDouble: {
                    double min = minField.type() == BSONType::minKey
                        ? std::numeric_limits<double>::min()
                        : minField.numberDouble();
                    double max = maxField.type() == BSONType::maxKey
                        ? std::numeric_limits<double>::max()
                        : maxField.numberDouble();
                    if (max <= std::numeric_limits<int>::min() + .00002 || max - 0.00002 < min) {
                        return boost::none;
                    }
                    std::uniform_real_distribution<double> dist(min + 0.00001, max - 0.00001);
                    randomDocument.appendNumber(name, dist(gen));
                    break;
                }
                case BSONType::numberLong: {
                    long long min = minField.type() == BSONType::minKey
                        ? std::numeric_limits<long long>::min()
                        : minField.numberLong();
                    long long max = maxField.type() == BSONType::maxKey
                        ? std::numeric_limits<long long>::max()
                        : maxField.numberLong();
                    if (max <= std::numeric_limits<int>::min() + 2 || max - 2 < min) {
                        return boost::none;
                    }
                    std::uniform_int_distribution<long long> dist(min + 1, max - 1);
                    randomDocument.appendNumber(name, dist(gen));
                    break;
                }
                case BSONType::string: {
                    std::string min = minField.type() == BSONType::minKey ? " " : minField.String();
                    int maxLength = std::max((int)min.size() + 1, 10);
                    std::string max = maxField.type() == BSONType::maxKey
                        ? std::string(maxLength, '~')
                        : maxField.String();
                    if (auto randomString = generateRandomStringBetween(min, max, maxLength, gen)) {
                        randomDocument.append(name, *randomString);
                    } else {
                        return boost::none;
                    }
                    break;
                }
                default: {
                    return boost::none;
                }
            }
        }
    }
    return randomDocument.obj();
}

/*
 * Finds all documents between min and max. If there are any, chooses a random one. If not, returns
 * boost::none.
 */
boost::optional<BSONObj> findExistingRandomDocument(OperationContext* opCtx,
                                                    const CollectionAcquisition& acquisition,
                                                    const BSONObj& skPattern,
                                                    const BSONObj& min,
                                                    const BSONObj& max,
                                                    std::default_random_engine& gen) {
    DBDirectClient client(opCtx);
    std::vector<BSONObj> docs;
    const auto shardKeyIdx = findShardKeyPrefixedIndex(opCtx,
                                                       acquisition.getCollectionPtr(),
                                                       skPattern,
                                                       /*requireSingleKey=*/true);
    FindCommandRequest findCmd(acquisition.nss());
    findCmd.setFilter({});
    BSONObj projection = skPattern;
    if (!skPattern.hasField("_id")) {
        projection = projection.addField(BSON("_id" << 0).firstElement());
    }
    findCmd.setProjection(projection);
    findCmd.setSort(skPattern);
    findCmd.setMin(min);
    findCmd.setMax(max);
    findCmd.setHint(shardKeyIdx->keyPattern());
    std::unique_ptr<DBClientCursor> cursor;
    try {
        cursor = client.find(std::move(findCmd));
    } catch (const DBException& ex) {
        LOGV2(10587402,
              "Failed to find existing documents in range",
              "error"_attr = redact(ex),
              "min"_attr = min,
              "max"_attr = max,
              "shardKey"_attr = skPattern);
        return boost::none;
    }
    while (cursor->more()) {
        const auto& nextDoc = cursor->next();
        // min is inclusive and we need exclusive so just manually exclude it here.
        if (nextDoc.woCompare(min) != 0) {
            docs.push_back(nextDoc);
        }
    }
    if (docs.size() > 0) {
        std::uniform_int_distribution<int> dist(0, docs.size() - 1);
        return docs[dist(gen)];
    }
    return boost::none;
}
}  // namespace

namespace random_migration_testing_utils {

bool isCurrentShardDraining(OperationContext* opCtx) {
    const auto& shardId = ShardingState::get(opCtx)->shardId();
    const auto& allShards = Grid::get(opCtx)
                                ->catalogClient()
                                ->getAllShards(opCtx, repl::ReadConcernLevel::kMajorityReadConcern)
                                .value;
    for (const auto& shard : allShards) {
        if (shard.getName() == shardId) {
            return shard.getDraining();
        }
    }
    return false;
}

boost::optional<BSONObj> generateRandomSplitPoint(OperationContext* opCtx,
                                                  const CollectionAcquisition& acquisition,
                                                  const BSONObj& skPattern,
                                                  const BSONObj& min,
                                                  const BSONObj& max) {
    std::default_random_engine gen(time(nullptr));

    // First check if there is any document locally and use that as the split point if so. This
    // makes it more likely that the test will target multiple chunks as the split points are
    // actually relevant documents.
    if (auto existingDoc =
            findExistingRandomDocument(opCtx, acquisition, skPattern, min, max, gen)) {
        return *existingDoc;
    }

    // If no document exists, then try to generate a new one in between min and max.
    return generateRandomDocument(min, max, gen);
}
}  // namespace random_migration_testing_utils
}  // namespace mongo
