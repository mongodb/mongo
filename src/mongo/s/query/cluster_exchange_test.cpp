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

#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/query/sharded_agg_test_fixture.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using MergeStrategyDescriptor = DocumentSourceMerge::MergeStrategyDescriptor;
using WhenMatched = MergeStrategyDescriptor::WhenMatched;
using WhenNotMatched = MergeStrategyDescriptor::WhenNotMatched;

const NamespaceString kTestTargetNss =
    NamespaceString::createNamespaceString_forTest("unittests", "out_ns");

class ClusterExchangeTest : public ShardedAggTestFixture {
protected:
    boost::optional<BSONObj> _mergeLetVariables;
    boost::optional<std::vector<BSONObj>> _mergePipeline;
    std::set<FieldPath> _mergeOnFields{"_id"};
    boost::optional<ChunkVersion> _mergeTargetCollectionPlacementVersion;
};

TEST_F(ClusterExchangeTest, ShouldNotExchangeIfPipelineDoesNotEndWithMerge) {
    setupNShards(2);
    auto mergePipe = Pipeline::create({DocumentSourceLimit::create(expCtx(), 1)}, expCtx());
    ASSERT_FALSE(
        sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get()));
    mergePipe = Pipeline::create({DocumentSourceMatch::create(BSONObj(), expCtx())}, expCtx());
    ASSERT_FALSE(
        sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get()));
}

TEST_F(ClusterExchangeTest, ShouldNotExchangeIfPipelineEndsWithOut) {
    setupNShards(2);

    // For this test pretend 'kTestTargetNss' is not sharded so that we can use $out.
    const auto originalMongoProcessInterface = expCtx()->mongoProcessInterface;
    expCtx()->mongoProcessInterface = std::make_shared<StubMongoProcessInterface>();
    ON_BLOCK_EXIT([&]() { expCtx()->mongoProcessInterface = originalMongoProcessInterface; });

    auto mergePipe =
        Pipeline::create({DocumentSourceOut::create(kTestTargetNss, expCtx())}, expCtx());
    ASSERT_FALSE(
        sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get()));
}

TEST_F(ClusterExchangeTest, SingleMergeStageNotEligibleForExchangeIfOutputDatabaseDoesNotExist) {
    setupNShards(2);
    auto mergePipe =
        Pipeline::create({DocumentSourceMerge::create(kTestTargetNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionPlacementVersion)},
                         expCtx());

    auto future = launchAsync([&] {
        ASSERT_THROWS_CODE(
            sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get()),
            AssertionException,
            ErrorCodes::NamespaceNotFound);
    });

    // Mock out a response as if the database doesn't exist.
    expectFindSendBSONObjVector(kConfigHostAndPort, []() {
        return std::vector<BSONObj>{};
    }());
    expectFindSendBSONObjVector(kConfigHostAndPort, []() {
        return std::vector<BSONObj>{};
    }());

    future.default_timed_get();
}

// If the output collection doesn't exist, we don't know how to distribute the output documents so
// cannot insert an $exchange. The $merge stage should later create a new, unsharded collection.
TEST_F(ClusterExchangeTest, SingleMergeStageNotEligibleForExchangeIfOutputCollectionDoesNotExist) {
    setupNShards(2);
    auto mergePipe =
        Pipeline::create({DocumentSourceMerge::create(kTestTargetNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionPlacementVersion)},

                         expCtx());

    auto future = launchAsync([&] {
        ASSERT_FALSE(
            sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get()));
    });

    expectGetDatabase(kTestTargetNss);
    // Pretend there are no collections in this database.
    expectFindSendBSONObjVector(kConfigHostAndPort, std::vector<BSONObj>());
    if (feature_flags::gGlobalIndexesShardingCatalog.isEnabledAndIgnoreFCVUnsafe()) {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, kConfigHostAndPort);
            ASSERT_EQ(request.dbname, "config");
            return CursorResponse(CollectionType::ConfigNS, CursorId{0}, {})
                .toBSON(CursorResponse::ResponseType::InitialResponse);
        });
    }

    future.default_timed_get();
}

// A $limit stage requires a single merger.
TEST_F(ClusterExchangeTest, LimitFollowedByMergeStageIsNotEligibleForExchange) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestTargetNss);

    auto mergePipe =
        Pipeline::create({DocumentSourceLimit::create(expCtx(), 6),
                          DocumentSourceMerge::create(kTestTargetNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionPlacementVersion)},
                         expCtx());

    auto future = launchAsync([&] {
        ASSERT_FALSE(
            sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get()));
    });

    future.default_timed_get();
}

TEST_F(ClusterExchangeTest, GroupFollowedByMergeIsEligbleForExchange) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestTargetNss);

    auto mergePipe =
        Pipeline::create({parseStage("{$group: {_id: '$x', $doingMerge: true}}"),
                          DocumentSourceMerge::create(kTestTargetNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionPlacementVersion)},
                         expCtx());

    auto future = launchAsync([&] {
        auto exchangeSpec =
            sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("_id" << 1));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 2UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().value();
        ASSERT_EQ(boundaries.size(), 3UL);

        ASSERT_BSONOBJ_EQ(boundaries[0], BSON("_id" << MINKEY));
        ASSERT_BSONOBJ_EQ(boundaries[1], BSON("_id" << 0));
        ASSERT_BSONOBJ_EQ(boundaries[2], BSON("_id" << MAXKEY));
    });

    future.default_timed_get();
}

TEST_F(ClusterExchangeTest, RenamesAreEligibleForExchange) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestTargetNss);

    auto mergePipe =
        Pipeline::create({parseStage("{$group: {_id: '$x', $doingMerge: true}}"),
                          parseStage("{$project: {temporarily_renamed: '$_id'}}"),
                          parseStage("{$project: {_id: '$temporarily_renamed'}}"),
                          DocumentSourceMerge::create(kTestTargetNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionPlacementVersion)},
                         expCtx());

    auto future = launchAsync([&] {
        auto exchangeSpec =
            sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("_id" << 1));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 2UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().value();
        const auto& consumerIds = exchangeSpec->exchangeSpec.getConsumerIds().value();
        ASSERT_EQ(boundaries.size(), 3UL);

        ASSERT_BSONOBJ_EQ(boundaries[0], BSON("_id" << MINKEY));
        ASSERT_BSONOBJ_EQ(boundaries[1], BSON("_id" << 0));
        ASSERT_BSONOBJ_EQ(boundaries[2], BSON("_id" << MAXKEY));

        ASSERT_EQ(consumerIds[0], 0);
        ASSERT_EQ(consumerIds[1], 1);
    });

    future.default_timed_get();
}

TEST_F(ClusterExchangeTest, MatchesAreEligibleForExchange) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestTargetNss);

    auto mergePipe =
        Pipeline::create({parseStage("{$group: {_id: '$x', $doingMerge: true}}"),
                          parseStage("{$match: {_id: {$gte: 0}}}"),
                          DocumentSourceMerge::create(kTestTargetNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionPlacementVersion)},
                         expCtx());

    auto future = launchAsync([&] {
        auto exchangeSpec =
            sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("_id" << 1));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 2UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().value();
        const auto& consumerIds = exchangeSpec->exchangeSpec.getConsumerIds().value();
        ASSERT_EQ(boundaries.size(), 3UL);

        ASSERT_BSONOBJ_EQ(boundaries[0], BSON("_id" << MINKEY));
        ASSERT_BSONOBJ_EQ(boundaries[1], BSON("_id" << 0));
        ASSERT_BSONOBJ_EQ(boundaries[2], BSON("_id" << MAXKEY));

        ASSERT_EQ(consumerIds[0], 0);
        ASSERT_EQ(consumerIds[1], 1);
    });

    future.default_timed_get();
}

TEST_F(ClusterExchangeTest, SortThenGroupIsEligibleForExchange) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestTargetNss);

    // This would be the merging half of the pipeline if the original pipeline was
    // [{$sort: {x: 1}},
    //  {$group: {_id: "$x"}},
    //  {$out: {to: "sharded_by_id", mode: "replaceDocuments"}}].
    // No $sort stage appears in the merging half since we'd expect that to be absorbed by the
    // $mergeCursors and AsyncResultsMerger.
    auto mergePipe =
        Pipeline::create({parseStage("{$group: {_id: '$x'}}"),
                          DocumentSourceMerge::create(kTestTargetNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionPlacementVersion)},
                         expCtx());

    auto future = launchAsync([&] {
        auto exchangeSpec =
            sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("x" << 1));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 2UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().value();
        const auto& consumerIds = exchangeSpec->exchangeSpec.getConsumerIds().value();
        ASSERT_EQ(boundaries.size(), 3UL);

        ASSERT_BSONOBJ_EQ(boundaries[0], BSON("x" << MINKEY));
        ASSERT_BSONOBJ_EQ(boundaries[1], BSON("x" << 0));
        ASSERT_BSONOBJ_EQ(boundaries[2], BSON("x" << MAXKEY));

        ASSERT_EQ(consumerIds[0], 0);
        ASSERT_EQ(consumerIds[1], 1);
    });

    future.default_timed_get();
}

TEST_F(ClusterExchangeTest, SortThenGroupIsEligibleForExchangeHash) {
    // Sharded by {_id: "hashed"}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShardsHash(kTestTargetNss);

    // This would be the merging half of the pipeline if the original pipeline was
    // [{$sort: {x: 1}},
    //  {$group: {_id: "$x"}},
    //  {$merge: {into: "sharded_by_id",  whenMatched: "fail", whenNotMatched: "insert"}}].
    // No $sort stage appears in the merging half since we'd expect that to be absorbed by the
    // $mergeCursors and AsyncResultsMerger.
    auto mergePipe =
        Pipeline::create({parseStage("{$group: {_id: '$x'}}"),
                          DocumentSourceMerge::create(kTestTargetNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionPlacementVersion)},
                         expCtx());

    auto future = launchAsync([&] {
        auto exchangeSpec =
            sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(),
                          BSON("x"
                               << "hashed"));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 2UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().value();
        const auto& consumerIds = exchangeSpec->exchangeSpec.getConsumerIds().value();
        ASSERT_EQ(boundaries.size(), 3UL);

        ASSERT_BSONOBJ_EQ(boundaries[0], BSON("x" << MINKEY));
        ASSERT_BSONOBJ_EQ(boundaries[1], BSON("x" << 0));
        ASSERT_BSONOBJ_EQ(boundaries[2], BSON("x" << MAXKEY));

        ASSERT_EQ(consumerIds[0], 0);
        ASSERT_EQ(consumerIds[1], 1);
    });

    future.default_timed_get();
}

TEST_F(ClusterExchangeTest, ProjectThroughDottedFieldDoesNotPreserveShardKey) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestTargetNss);

    auto mergePipe = Pipeline::create(
        {parseStage("{$group: {"
                    "  _id: {region: '$region', country: '$country'},"
                    "  population: {$sum: '$population'},"
                    "  cities: {$push: {name: '$city', population: '$population'}}"
                    "}}"),
         parseStage(
             "{$project: {_id: '$_id.country', region: '$_id.region', population: 1, cities: 1}}"),
         DocumentSourceMerge::create(kTestTargetNss,
                                     expCtx(),
                                     WhenMatched::kFail,
                                     WhenNotMatched::kInsert,
                                     _mergeLetVariables,
                                     _mergePipeline,
                                     _mergeOnFields,
                                     _mergeTargetCollectionPlacementVersion)},
        expCtx());

    auto future = launchAsync([&] {
        auto exchangeSpec =
            sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get());
        // Because '_id' is populated from '$_id.country', we cannot prove that '_id' is a simple
        // rename. We cannot prove that '_id' is not an array, and thus the $project could do more
        // than a rename.
        ASSERT_FALSE(exchangeSpec);
    });

    future.default_timed_get();
}

TEST_F(ClusterExchangeTest, WordCountUseCaseExample) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestTargetNss);

    // As an example of a pipeline that might replace a map reduce, imagine that we are performing a
    // word count, and the shards part of the pipeline tokenized some text field of each document
    // into {word: <token>, count: 1}. Then this is the merging half of the pipeline:
    auto mergePipe =
        Pipeline::create({parseStage("{$group: {"
                                     "  _id: '$word',"
                                     "  count: {$sum: 1},"
                                     "  $doingMerge: true"
                                     "}}"),
                          DocumentSourceMerge::create(kTestTargetNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionPlacementVersion)},
                         expCtx());

    auto future = launchAsync([&] {
        auto exchangeSpec =
            sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("_id" << 1));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 2UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().value();
        const auto& consumerIds = exchangeSpec->exchangeSpec.getConsumerIds().value();
        ASSERT_EQ(boundaries.size(), 3UL);

        ASSERT_BSONOBJ_EQ(boundaries[0], BSON("_id" << MINKEY));
        ASSERT_BSONOBJ_EQ(boundaries[1], BSON("_id" << 0));
        ASSERT_BSONOBJ_EQ(boundaries[2], BSON("_id" << MAXKEY));

        ASSERT_EQ(consumerIds[0], 0);
        ASSERT_EQ(consumerIds[1], 1);
    });

    future.default_timed_get();
}

TEST_F(ClusterExchangeTest, WordCountUseCaseExampleShardedByWord) {
    setupNShards(2);
    const OID epoch = OID::gen();
    const Timestamp timestamp = Timestamp(1);
    ShardKeyPattern shardKey(BSON("word" << 1));
    loadRoutingTable(kTestTargetNss,
                     epoch,
                     timestamp,
                     shardKey,
                     makeChunks(UUID::gen(),
                                epoch,
                                timestamp,
                                {{ChunkRange{BSON("word" << MINKEY),
                                             BSON("word"
                                                  << "hello")},
                                  ShardId("0")},
                                 {ChunkRange{BSON("word"
                                                  << "hello"),
                                             BSON("word"
                                                  << "world")},
                                  ShardId("1")},
                                 {ChunkRange{BSON("word"
                                                  << "world"),
                                             BSON("word" << MAXKEY)},
                                  ShardId("1")}}));

    // As an example of a pipeline that might replace a map reduce, imagine that we are performing a
    // word count, and the shards part of the pipeline tokenized some text field of each document
    // into {word: <token>, count: 1}. Then this is the merging half of the pipeline:
    auto mergePipe =
        Pipeline::create({parseStage("{$group: {"
                                     "  _id: '$word',"
                                     "  count: {$sum: 1},"
                                     "  $doingMerge: true"
                                     "}}"),
                          parseStage("{$project: {word: '$_id', count: 1}}"),
                          DocumentSourceMerge::create(kTestTargetNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionPlacementVersion)},
                         expCtx());

    auto future = launchAsync([&] {
        auto exchangeSpec =
            sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("_id" << 1));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 2UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().value();
        const auto& consumerIds = exchangeSpec->exchangeSpec.getConsumerIds().value();
        ASSERT_EQ(boundaries.size(), 4UL);
        ASSERT_EQ(consumerIds.size(), 3UL);

        ASSERT_BSONOBJ_EQ(boundaries[0], BSON("_id" << MINKEY));
        ASSERT_BSONOBJ_EQ(boundaries[1],
                          BSON("_id"
                               << "hello"));
        ASSERT_BSONOBJ_EQ(boundaries[2],
                          BSON("_id"
                               << "world"));
        ASSERT_BSONOBJ_EQ(boundaries[3], BSON("_id" << MAXKEY));

        ASSERT_EQ(consumerIds[0], 0);
        ASSERT_EQ(consumerIds[1], 1);
        ASSERT_EQ(consumerIds[2], 1);
    });

    future.default_timed_get();
}

// We'd like to test that a compound shard key pattern can be used. Strangely, the only case we can
// actually perform an exchange today on a compound shard key is when the shard key contains fields
// which are all duplicates. This is due to the limitations of tracking renames through dots, see
// SERVER-36787 for an example.
TEST_F(ClusterExchangeTest, CompoundShardKeyThreeShards) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    const Timestamp timestamp(1, 1);
    ShardKeyPattern shardKey(BSON("x" << 1 << "y" << 1));

    setupNShards(3);
    const std::vector<std::string> xBoundaries = {"a", "g", "m", "r", "u"};
    auto chunks = [&]() {
        std::vector<ChunkType> chunks;
        ChunkVersion version({epoch, timestamp}, {1, 0});
        chunks.emplace_back(uuid,
                            ChunkRange{BSON("x" << MINKEY << "y" << MINKEY),
                                       BSON("x" << xBoundaries[0] << "y" << MINKEY)},
                            version,
                            ShardId("0"));
        chunks.back().setName(OID::gen());
        for (std::size_t i = 0; i < xBoundaries.size() - 1; ++i) {
            chunks.emplace_back(uuid,
                                ChunkRange{BSON("x" << xBoundaries[i] << "y" << MINKEY),
                                           BSON("x" << xBoundaries[i + 1] << "y" << MINKEY)},
                                version,
                                ShardId(str::stream() << (i + 1) % 3));
            chunks.back().setName(OID::gen());
        }
        chunks.emplace_back(uuid,
                            ChunkRange{BSON("x" << xBoundaries.back() << "y" << MINKEY),
                                       BSON("x" << MAXKEY << "y" << MAXKEY)},
                            version,
                            ShardId(str::stream() << xBoundaries.size() % 3));
        chunks.back().setName(OID::gen());
        return chunks;
    }();

    loadRoutingTable(kTestTargetNss, epoch, timestamp, shardKey, chunks);

    auto mergePipe =
        Pipeline::create({parseStage("{$group: {"
                                     "  _id: '$x',"
                                     "  $doingMerge: true"
                                     "}}"),
                          parseStage("{$project: {x: '$_id', y: '$_id'}}"),
                          DocumentSourceMerge::create(kTestTargetNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionPlacementVersion)},
                         expCtx());

    auto future = launchAsync([&] {
        auto exchangeSpec =
            sharded_agg_helpers::checkIfEligibleForExchange(operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("_id" << 1 << "_id" << 1));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 3UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().value();
        const auto& consumerIds = exchangeSpec->exchangeSpec.getConsumerIds().value();
        ASSERT_EQ(boundaries.size(), chunks.size() + 1);
        ASSERT_EQ(consumerIds.size(), chunks.size());

        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("_id" << 1 << "_id" << 1));

        // Make sure each shard has the same chunks that it started with, just with the names of the
        // boundary fields translated. For each chunk that we created to begin with, make sure its
        // corresponding/translated chunk is present on the same shard in the same order.
        int counter = 0;
        for (auto&& chunk : chunks) {
            ASSERT_EQ(consumerIds[counter], (counter % 3));

            auto expectedChunkMin = [&]() {
                ASSERT_EQ(chunk.getMin().nFields(), 2);
                return BSON("_id" << chunk.getMin()["x"] << "_id" << chunk.getMin()["y"]);
            }();
            ASSERT_BSONOBJ_EQ(boundaries[counter], expectedChunkMin);

            auto expectedChunkMax = [&]() {
                ASSERT_EQ(chunk.getMax().nFields(), 2);
                return BSON("_id" << chunk.getMax()["x"] << "_id" << chunk.getMax()["y"]);
            }();
            ASSERT_BSONOBJ_EQ(boundaries[counter + 1], expectedChunkMax);

            ++counter;
        }
    });

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
