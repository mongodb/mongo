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

#include "mongo/platform/basic.h"


#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/stub_mongo_process_interface.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/query/cluster_aggregation_planner.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {
using MergeStrategyDescriptor = DocumentSourceMerge::MergeStrategyDescriptor;
using WhenMatched = MergeStrategyDescriptor::WhenMatched;
using WhenNotMatched = MergeStrategyDescriptor::WhenNotMatched;

const NamespaceString kTestAggregateNss = NamespaceString{"unittests", "cluster_exchange"};
const NamespaceString kTestOutNss = NamespaceString{"unittests", "out_ns"};

/**
 * For the purposes of this test, assume every collection is sharded. Stages may ask this during
 * setup. For example, to compute its constraints, the $merge stage needs to know if the output
 * collection is sharded.
 */
class FakeMongoProcessInterface : public StubMongoProcessInterface {
public:
    bool isSharded(OperationContext* opCtx, const NamespaceString& ns) override {
        return true;
    }
};

class ClusterExchangeTest : public CatalogCacheTestFixture {
public:
    void setUp() {
        CatalogCacheTestFixture::setUp();
        _expCtx = new ExpressionContextForTest(operationContext(),
                                               AggregationRequest{kTestAggregateNss, {}});
        _expCtx->mongoProcessInterface = std::make_shared<FakeMongoProcessInterface>();
        _expCtx->inMongos = true;
    }

    boost::intrusive_ptr<ExpressionContext> expCtx() {
        return _expCtx;
    }

    boost::intrusive_ptr<DocumentSource> parse(const std::string& json) {
        auto stages = DocumentSource::parse(_expCtx, fromjson(json));
        ASSERT_EQ(stages.size(), 1UL);
        return stages.front();
    }

    std::vector<ChunkType> makeChunks(const NamespaceString& nss,
                                      const OID epoch,
                                      std::vector<std::pair<ChunkRange, ShardId>> chunkInfos) {
        ChunkVersion version(1, 0, epoch);
        std::vector<ChunkType> chunks;
        for (auto&& pair : chunkInfos) {
            chunks.emplace_back(nss, pair.first, version, pair.second);
            version.incMinor();
        }
        return chunks;
    }

    void loadRoutingTable(NamespaceString nss,
                          const OID epoch,
                          const ShardKeyPattern& shardKey,
                          const std::vector<ChunkType>& chunkDistribution) {
        auto future = scheduleRoutingInfoRefresh(nss);

        // Mock the expected config server queries.
        expectGetDatabase(nss);
        expectGetCollection(nss, epoch, shardKey);
        expectGetCollection(nss, epoch, shardKey);
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            std::vector<BSONObj> response;
            for (auto&& chunk : chunkDistribution) {
                response.push_back(chunk.toConfigBSON());
            }
            return response;
        }());

        future.default_timed_get().get();
    }

protected:
    boost::intrusive_ptr<ExpressionContext> _expCtx;
    boost::optional<BSONObj> _mergeLetVariables;
    boost::optional<std::vector<BSONObj>> _mergePipeline;
    std::set<FieldPath> _mergeOnFields{"_id"};
    boost::optional<ChunkVersion> _mergeTargetCollectionVersion;
};

TEST_F(ClusterExchangeTest, ShouldNotExchangeIfPipelineDoesNotEndWithOut) {
    setupNShards(2);
    auto mergePipe =
        unittest::assertGet(Pipeline::create({DocumentSourceLimit::create(expCtx(), 1)}, expCtx()));
    ASSERT_FALSE(cluster_aggregation_planner::checkIfEligibleForExchange(operationContext(),
                                                                         mergePipe.get()));
    mergePipe = unittest::assertGet(
        Pipeline::create({DocumentSourceMatch::create(BSONObj(), expCtx())}, expCtx()));
    ASSERT_FALSE(cluster_aggregation_planner::checkIfEligibleForExchange(operationContext(),
                                                                         mergePipe.get()));
}

TEST_F(ClusterExchangeTest, ShouldNotExchangeIfPipelineEndsWithOut) {
    setupNShards(2);

    // For this test pretend 'kTestOutNss' is not sharded so that we can use $out.
    const auto originalMongoProcessInterface = expCtx()->mongoProcessInterface;
    expCtx()->mongoProcessInterface = std::make_shared<StubMongoProcessInterface>();
    ON_BLOCK_EXIT([&]() { expCtx()->mongoProcessInterface = originalMongoProcessInterface; });

    auto mergePipe = unittest::assertGet(
        Pipeline::create({DocumentSourceOut::create(kTestOutNss, expCtx())}, expCtx()));
    ASSERT_FALSE(cluster_aggregation_planner::checkIfEligibleForExchange(operationContext(),
                                                                         mergePipe.get()));
}

TEST_F(ClusterExchangeTest, SingleMergeStageNotEligibleForExchangeIfOutputDatabaseDoesNotExist) {
    setupNShards(2);
    auto mergePipe = unittest::assertGet(
        Pipeline::create({DocumentSourceMerge::create(kTestOutNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionVersion)},
                         expCtx()));

    auto future = launchAsync([&] {
        ASSERT_THROWS_CODE(cluster_aggregation_planner::checkIfEligibleForExchange(
                               operationContext(), mergePipe.get()),
                           AssertionException,
                           ErrorCodes::NamespaceNotFound);
    });

    // Mock out a response as if the database doesn't exist.
    expectFindSendBSONObjVector(kConfigHostAndPort, []() { return std::vector<BSONObj>{}; }());
    expectFindSendBSONObjVector(kConfigHostAndPort, []() { return std::vector<BSONObj>{}; }());

    future.default_timed_get();
}

// If the output collection doesn't exist, we don't know how to distribute the output documents so
// cannot insert an $exchange. The $merge stage should later create a new, unsharded collection.
TEST_F(ClusterExchangeTest, SingleMergeStageNotEligibleForExchangeIfOutputCollectionDoesNotExist) {
    setupNShards(2);
    auto mergePipe = unittest::assertGet(
        Pipeline::create({DocumentSourceMerge::create(kTestOutNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionVersion)},

                         expCtx()));

    auto future = launchAsync([&] {
        ASSERT_FALSE(cluster_aggregation_planner::checkIfEligibleForExchange(operationContext(),
                                                                             mergePipe.get()));
    });

    expectGetDatabase(kTestOutNss);
    // Pretend there are no collections in this database.
    expectFindSendBSONObjVector(kConfigHostAndPort, std::vector<BSONObj>());

    future.default_timed_get();
}

// A $limit stage requires a single merger.
TEST_F(ClusterExchangeTest, LimitFollowedByMergeStageIsNotEligibleForExchange) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestOutNss);

    auto mergePipe = unittest::assertGet(
        Pipeline::create({DocumentSourceLimit::create(expCtx(), 6),
                          DocumentSourceMerge::create(kTestOutNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionVersion)},
                         expCtx()));

    auto future = launchAsync([&] {
        ASSERT_FALSE(cluster_aggregation_planner::checkIfEligibleForExchange(operationContext(),
                                                                             mergePipe.get()));
    });

    future.default_timed_get();
}

TEST_F(ClusterExchangeTest, GroupFollowedByMergeIsEligbleForExchange) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestOutNss);

    auto mergePipe = unittest::assertGet(
        Pipeline::create({parse("{$group: {_id: '$x', $doingMerge: true}}"),
                          DocumentSourceMerge::create(kTestOutNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionVersion)},
                         expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("_id" << 1));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 2UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().get();
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
    loadRoutingTableWithTwoChunksAndTwoShards(kTestOutNss);

    auto mergePipe = unittest::assertGet(
        Pipeline::create({parse("{$group: {_id: '$x', $doingMerge: true}}"),
                          parse("{$project: {temporarily_renamed: '$_id'}}"),
                          parse("{$project: {_id: '$temporarily_renamed'}}"),
                          DocumentSourceMerge::create(kTestOutNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionVersion)},
                         expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("_id" << 1));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 2UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().get();
        const auto& consumerIds = exchangeSpec->exchangeSpec.getConsumerIds().get();
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
    loadRoutingTableWithTwoChunksAndTwoShards(kTestOutNss);

    auto mergePipe = unittest::assertGet(
        Pipeline::create({parse("{$group: {_id: '$x', $doingMerge: true}}"),
                          parse("{$match: {_id: {$gte: 0}}}"),
                          DocumentSourceMerge::create(kTestOutNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionVersion)},
                         expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("_id" << 1));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 2UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().get();
        const auto& consumerIds = exchangeSpec->exchangeSpec.getConsumerIds().get();
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
    loadRoutingTableWithTwoChunksAndTwoShards(kTestOutNss);

    // This would be the merging half of the pipeline if the original pipeline was
    // [{$sort: {x: 1}},
    //  {$group: {_id: "$x"}},
    //  {$out: {to: "sharded_by_id", mode: "replaceDocuments"}}].
    // No $sort stage appears in the merging half since we'd expect that to be absorbed by the
    // $mergeCursors and AsyncResultsMerger.
    auto mergePipe = unittest::assertGet(
        Pipeline::create({parse("{$group: {_id: '$x'}}"),
                          DocumentSourceMerge::create(kTestOutNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionVersion)},
                         expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("x" << 1));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 2UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().get();
        const auto& consumerIds = exchangeSpec->exchangeSpec.getConsumerIds().get();
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
    loadRoutingTableWithTwoChunksAndTwoShardsHash(kTestOutNss);

    // This would be the merging half of the pipeline if the original pipeline was
    // [{$sort: {x: 1}},
    //  {$group: {_id: "$x"}},
    //  {$merge: {into: "sharded_by_id",  whenMatched: "fail", whenNotMatched: "insert"}}].
    // No $sort stage appears in the merging half since we'd expect that to be absorbed by the
    // $mergeCursors and AsyncResultsMerger.
    auto mergePipe = unittest::assertGet(
        Pipeline::create({parse("{$group: {_id: '$x'}}"),
                          DocumentSourceMerge::create(kTestOutNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionVersion)},
                         expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(),
                          BSON("x"
                               << "hashed"));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 2UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().get();
        const auto& consumerIds = exchangeSpec->exchangeSpec.getConsumerIds().get();
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
    loadRoutingTableWithTwoChunksAndTwoShards(kTestOutNss);

    auto mergePipe = unittest::assertGet(Pipeline::create(
        {parse("{$group: {"
               "  _id: {region: '$region', country: '$country'},"
               "  population: {$sum: '$population'},"
               "  cities: {$push: {name: '$city', population: '$population'}}"
               "}}"),
         parse(
             "{$project: {_id: '$_id.country', region: '$_id.region', population: 1, cities: 1}}"),
         DocumentSourceMerge::create(kTestOutNss,
                                     expCtx(),
                                     WhenMatched::kFail,
                                     WhenNotMatched::kInsert,
                                     _mergeLetVariables,
                                     _mergePipeline,
                                     _mergeOnFields,
                                     _mergeTargetCollectionVersion)},
        expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
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
    loadRoutingTableWithTwoChunksAndTwoShards(kTestOutNss);

    // As an example of a pipeline that might replace a map reduce, imagine that we are performing a
    // word count, and the shards part of the pipeline tokenized some text field of each document
    // into {word: <token>, count: 1}. Then this is the merging half of the pipeline:
    auto mergePipe = unittest::assertGet(
        Pipeline::create({parse("{$group: {"
                                "  _id: '$word',"
                                "  count: {$sum: 1},"
                                "  $doingMerge: true"
                                "}}"),
                          DocumentSourceMerge::create(kTestOutNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionVersion)},
                         expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("_id" << 1));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 2UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().get();
        const auto& consumerIds = exchangeSpec->exchangeSpec.getConsumerIds().get();
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
    ShardKeyPattern shardKey(BSON("word" << 1));
    loadRoutingTable(kTestOutNss,
                     epoch,
                     shardKey,
                     makeChunks(kTestOutNss,
                                epoch,
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
    auto mergePipe = unittest::assertGet(
        Pipeline::create({parse("{$group: {"
                                "  _id: '$word',"
                                "  count: {$sum: 1},"
                                "  $doingMerge: true"
                                "}}"),
                          parse("{$project: {word: '$_id', count: 1}}"),
                          DocumentSourceMerge::create(kTestOutNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionVersion)},
                         expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("_id" << 1));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 2UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().get();
        const auto& consumerIds = exchangeSpec->exchangeSpec.getConsumerIds().get();
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
    ShardKeyPattern shardKey(BSON("x" << 1 << "y" << 1));

    setupNShards(3);
    const std::vector<std::string> xBoundaries = {"a", "g", "m", "r", "u"};
    auto chunks = [&]() {
        std::vector<ChunkType> chunks;
        ChunkVersion version(1, 0, epoch);
        chunks.emplace_back(kTestOutNss,
                            ChunkRange{BSON("x" << MINKEY << "y" << MINKEY),
                                       BSON("x" << xBoundaries[0] << "y" << MINKEY)},
                            version,
                            ShardId("0"));
        for (std::size_t i = 0; i < xBoundaries.size() - 1; ++i) {
            chunks.emplace_back(kTestOutNss,
                                ChunkRange{BSON("x" << xBoundaries[i] << "y" << MINKEY),
                                           BSON("x" << xBoundaries[i + 1] << "y" << MINKEY)},
                                version,
                                ShardId(str::stream() << (i + 1) % 3));
        }
        chunks.emplace_back(kTestOutNss,
                            ChunkRange{BSON("x" << xBoundaries.back() << "y" << MINKEY),
                                       BSON("x" << MAXKEY << "y" << MAXKEY)},
                            version,
                            ShardId(str::stream() << xBoundaries.size() % 3));
        return chunks;
    }();

    loadRoutingTable(kTestOutNss, epoch, shardKey, chunks);

    auto mergePipe = unittest::assertGet(
        Pipeline::create({parse("{$group: {"
                                "  _id: '$x',"
                                "  $doingMerge: true"
                                "}}"),
                          parse("{$project: {x: '$_id', y: '$_id'}}"),
                          DocumentSourceMerge::create(kTestOutNss,
                                                      expCtx(),
                                                      WhenMatched::kFail,
                                                      WhenNotMatched::kInsert,
                                                      _mergeLetVariables,
                                                      _mergePipeline,
                                                      _mergeOnFields,
                                                      _mergeTargetCollectionVersion)},
                         expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->exchangeSpec.getPolicy() == ExchangePolicyEnum::kKeyRange);
        ASSERT_BSONOBJ_EQ(exchangeSpec->exchangeSpec.getKey(), BSON("_id" << 1 << "_id" << 1));
        ASSERT_EQ(exchangeSpec->consumerShards.size(), 3UL);  // One for each shard.
        const auto& boundaries = exchangeSpec->exchangeSpec.getBoundaries().get();
        const auto& consumerIds = exchangeSpec->exchangeSpec.getConsumerIds().get();
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
