// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include <boost/none.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/s/query/exec/sharded_agg_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

// Use this new name to register these tests under their own unit test suite.
using DispatchShardPipelineTest = ShardedAggTestFixture;
using sharded_agg_helpers::PipelineDataSource;

TEST_F(DispatchShardPipelineTest, DoesNotSplitPipelineIfTargetingOneShard) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    auto shards = setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestAggregateNss);

    auto stages = std::vector{
        fromjson("{$match: {_id: {$gte: 0}}}"),
        fromjson("{$sort: {score: -1}}"),
        fromjson("{$group: {_id: '$username', high_scores: {$push: '$score'}}}"),
    };
    auto pipeline = Pipeline::create(
        {parseStage(stages[0]), parseStage(stages[1]), parseStage(stages[2])}, expCtx());
    const auto serializedCommand =
        Document(AggregateCommandRequest(expCtx()->getNamespaceString(), stages).toBSON());
    const auto pipelineDataSource = PipelineDataSource::kNormal;
    const bool eligibleForSampling = false;

    routing_context_utils::withValidatedRoutingContext(
        operationContext(), std::vector{kTestAggregateNss}, [&](RoutingContext& routingCtx) {
            auto future = launchAsync([&] {
                auto results = sharded_agg_helpers::dispatchShardPipeline(routingCtx,
                                                                          serializedCommand,
                                                                          pipelineDataSource,
                                                                          eligibleForSampling,
                                                                          std::move(pipeline),
                                                                          boost::none /*explain*/,
                                                                          kTestAggregateNss);
                ASSERT_EQ(results.remoteCursors.size(), 1UL);
                ASSERT(!results.splitPipeline);
            });

            onCommand([&](const executor::RemoteCommandRequest& request) {
                ASSERT_EQ(request.target, HostAndPort(shards[1].getHost()));
                return CursorResponse(kTestAggregateNss, CursorId{0}, std::vector<BSONObj>{})
                    .toBSON(CursorResponse::ResponseType::InitialResponse);
            });

            future.default_timed_get();
        });
}

TEST_F(DispatchShardPipelineTest, DoesSplitPipelineIfMatchSpansTwoShards) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestAggregateNss);
    auto stages = std::vector{
        fromjson("{$match: {_id: {$gte: -10}}}"),
        fromjson("{$sort: {score: -1}}"),
        fromjson("{$group: {_id: '$username', high_scores: {$push: '$score'}}}"),
    };
    auto pipeline = Pipeline::create(
        {parseStage(stages[0]), parseStage(stages[1]), parseStage(stages[2])}, expCtx());
    const auto serializedCommand =
        Document(AggregateCommandRequest(expCtx()->getNamespaceString(), stages).toBSON());
    const auto pipelineDataSource = PipelineDataSource::kNormal;
    const bool eligibleForSampling = false;

    routing_context_utils::withValidatedRoutingContext(
        operationContext(), std::vector{kTestAggregateNss}, [&](RoutingContext& routingCtx) {
            auto future = launchAsync([&] {
                auto results = sharded_agg_helpers::dispatchShardPipeline(routingCtx,
                                                                          serializedCommand,
                                                                          pipelineDataSource,
                                                                          eligibleForSampling,
                                                                          std::move(pipeline),
                                                                          boost::none /*explain*/,
                                                                          kTestAggregateNss);
                ASSERT_EQ(results.remoteCursors.size(), 2UL);
                ASSERT(bool(results.splitPipeline));
            });

            onCommand([&](const executor::RemoteCommandRequest& request) {
                return CursorResponse(kTestAggregateNss, CursorId{0}, std::vector<BSONObj>{})
                    .toBSON(CursorResponse::ResponseType::InitialResponse);
            });
            onCommand([&](const executor::RemoteCommandRequest& request) {
                return CursorResponse(kTestAggregateNss, CursorId{0}, std::vector<BSONObj>{})
                    .toBSON(CursorResponse::ResponseType::InitialResponse);
            });

            future.default_timed_get();
        });
}

TEST_F(DispatchShardPipelineTest, DispatchShardPipelineRetriesOnNetworkError) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestAggregateNss);
    auto stages = std::vector{
        fromjson("{$match: {_id: {$gte: -10}}}"),
        fromjson("{$sort: {score: -1}}"),
        fromjson("{$group: {_id: '$username', high_scores: {$push: '$score'}}}"),
    };
    auto pipeline = Pipeline::create(
        {parseStage(stages[0]), parseStage(stages[1]), parseStage(stages[2])}, expCtx());
    const auto serializedCommand =
        Document(AggregateCommandRequest(expCtx()->getNamespaceString(), stages).toBSON());
    const auto pipelineDataSource = PipelineDataSource::kNormal;
    const bool eligibleForSampling = false;

    routing_context_utils::withValidatedRoutingContext(
        operationContext(), std::vector{kTestAggregateNss}, [&](RoutingContext& routingCtx) {
            auto future = launchAsync([&] {
                // Shouldn't throw.
                auto results = sharded_agg_helpers::dispatchShardPipeline(routingCtx,
                                                                          serializedCommand,
                                                                          pipelineDataSource,
                                                                          eligibleForSampling,
                                                                          std::move(pipeline),
                                                                          boost::none /*explain*/,
                                                                          kTestAggregateNss);
                ASSERT_EQ(results.remoteCursors.size(), 2UL);
                ASSERT(bool(results.splitPipeline));
            });

            // Mock out responses, one success, one error.
            onCommand([&](const executor::RemoteCommandRequest& request) {
                return CursorResponse(kTestAggregateNss, CursorId{0}, std::vector<BSONObj>{})
                    .toBSON(CursorResponse::ResponseType::InitialResponse);
            });
            HostAndPort unreachableShard;
            onCommand([&](const executor::RemoteCommandRequest& request) {
                unreachableShard = request.target;
                return Status{ErrorCodes::HostUnreachable, "Mock error: Couldn't find host"};
            });

            // That error should be retried, but only the one on that shard.
            onCommand([&](const executor::RemoteCommandRequest& request) {
                // Test that it's retrying to the shard we failed earlier.
                ASSERT_EQ(request.target, unreachableShard);
                return CursorResponse(kTestAggregateNss, CursorId{0}, std::vector<BSONObj>{})
                    .toBSON(CursorResponse::ResponseType::InitialResponse);
            });
            future.default_timed_get();
        });
}

// Test that this helper is not responsible for retrying StaleConfig errors. This should happen at a
// higher level.
TEST_F(DispatchShardPipelineTest, DispatchShardPipelineDoesNotRetryOnStaleConfigError) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestAggregateNss);
    auto stages = std::vector{
        fromjson("{$match: {_id: {$gte: 0}}}"),
        fromjson("{$sort: {score: -1}}"),
        fromjson("{$group: {_id: '$username', high_scores: {$push: '$score'}}}"),
    };
    auto pipeline = Pipeline::create(
        {parseStage(stages[0]), parseStage(stages[1]), parseStage(stages[2])}, expCtx());
    const auto serializedCommand =
        Document(AggregateCommandRequest(expCtx()->getNamespaceString(), stages).toBSON());
    const auto pipelineDataSource = PipelineDataSource::kNormal;
    const bool eligibleForSampling = false;

    routing_context_utils::withValidatedRoutingContext(
        operationContext(), std::vector{kTestAggregateNss}, [&](RoutingContext& routingCtx) {
            auto future = launchAsync([&] {
                ASSERT_THROWS_CODE(
                    sharded_agg_helpers::dispatchShardPipeline(routingCtx,
                                                               serializedCommand,
                                                               pipelineDataSource,
                                                               eligibleForSampling,
                                                               std::move(pipeline),
                                                               boost::none /*explain*/,
                                                               kTestAggregateNss),
                    AssertionException,
                    ErrorCodes::StaleConfig);
            });

            // Mock out an error response.
            onCommand([&](const executor::RemoteCommandRequest& request) {
                OID epoch{OID::gen()};
                Timestamp timestamp{1, 0};
                return createErrorCursorResponse(
                    {StaleConfigInfo(
                         kTestAggregateNss,
                         ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {1, 0})),
                         boost::none,
                         ShardId{"0"}),
                     "Mock error: shard version mismatch"});
            });
            future.default_timed_get();
        });
}

TEST_F(DispatchShardPipelineTest, WrappedDispatchDoesRetryOnStaleConfigError) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestAggregateNss);
    auto stages = std::vector{
        fromjson("{$match: {_id: {$gte: 0}}}"),
        fromjson("{$sort: {score: -1}}"),
        fromjson("{$group: {_id: '$username', high_scores: {$push: '$score'}}}"),
    };
    auto pipeline = Pipeline::create(
        {parseStage(stages[0]), parseStage(stages[1]), parseStage(stages[2])}, expCtx());
    const auto serializedCommand =
        Document(AggregateCommandRequest(expCtx()->getNamespaceString(), stages).toBSON());
    const auto pipelineDataSource = PipelineDataSource::kNormal;
    const bool eligibleForSampling = false;
    auto future = launchAsync([&] {
        // Shouldn't throw.
        sharding::router::CollectionRouter router(operationContext(), kTestAggregateNss);
        auto results = router.routeWithRoutingContext(
            "dispatch shard pipeline"sv, [&](OperationContext* opCtx, RoutingContext& routingCtx) {
                return sharded_agg_helpers::dispatchShardPipeline(routingCtx,
                                                                  serializedCommand,
                                                                  pipelineDataSource,
                                                                  eligibleForSampling,
                                                                  pipeline->clone(),
                                                                  boost::none /*explain*/,
                                                                  kTestAggregateNss);
            });
        ASSERT_EQ(results.remoteCursors.size(), 1UL);
        ASSERT(!bool(results.splitPipeline));
    });

    const OID epoch{OID::gen()};
    const Timestamp timestamp{1, 0};
    const UUID uuid{UUID::gen()};

    // Mock out one error response, then expect a refresh of the sharding catalog for that
    // namespace, then mock out a successful response.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return createErrorCursorResponse(
            {StaleConfigInfo(kTestAggregateNss,
                             ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {2, 0})),
                             boost::none,
                             ShardId{"0"}),
             "Mock error: shard version mismatch"});
    });

    // Mock the expected config server queries.
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    ChunkVersion version({epoch, timestamp}, {2, 0});

    ChunkType chunk1(
        uuid, {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)}, version, {"0"});
    chunk1.setName(OID::gen());
    version.incMinor();

    ChunkType chunk2(
        uuid, {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()}, version, {"1"});
    chunk2.setName(OID::gen());
    version.incMinor();

    expectCollectionAndChunksAggregation(
        kTestAggregateNss, epoch, timestamp, uuid, shardKeyPattern, {chunk1, chunk2});

    // That error should be retried, but only the one on that shard.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return CursorResponse(kTestAggregateNss, CursorId{0}, std::vector<BSONObj>{})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}
}  // namespace
}  // namespace mongo
