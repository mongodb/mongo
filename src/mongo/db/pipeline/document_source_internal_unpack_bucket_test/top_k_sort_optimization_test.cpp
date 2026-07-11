// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

namespace mongo {
namespace {

using TopKSortOptimization = AggregationContextFixture;
const auto kExplain = query_shape::SerializationOptions{
    .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)};

const auto commonInitialUnpackSpecObj = fromjson(R"(
{
    $_internalUnpackBucket: {
        exclude: [],
        timeField: 'time',
        metaField: 'tag',
        bucketMaxSpanSeconds: 3600
    }
}
)");

const auto firstLastGroupSpecObj = fromjson(R"(
{
    $group: {
        _id: {
            hour: {$dateTrunc: {date: '$time', unit: 'hour'}},
            symbol: '$tag.symbol'
        },
        open: {$first: '$price'},
        close: {$last: '$price'}
    }
}
)");

// The $group should absorb the $sort stage by replacing $first/$last by $top/$bottom respectively.
constexpr size_t kExpectedGroupIndexForMatchOnly = 2;
const auto expectedOptimizedGroupForMatchOnly = fromjson(R"(
{
    $group: {
        _id: {hour: {$dateTrunc: {date: '$time', unit: {$const: 'hour'}}}, symbol: '$tag.symbol'},
        open: {$top: {output: '$price', sortBy: {time: 1}}},
        close: {$bottom: {output: '$price', sortBy: {time: 1}}},
        $willBeMerged: false
    }
}
)");

// The $match stage should be pushed down before the $_internalUnpackBucket.
constexpr size_t kExpectedMatchIndexForMatchOnly = 0;
const auto expectedOptimizedMatchForMatchOnly = fromjson(R"(
{
    $match: {
        $or: [
            {"control.max.price": {$_internalExprGte: 100}},
            {$expr: {$ne: [{$type: ["$control.min.price"]}, {$type: ["$control.max.price"]}]}}
        ]
    }
}
)");

// The $_internalUnpackBucket should have the event filter while $match being pushed down.
constexpr size_t kExpectedUnpackBucketIndexForMatchOnly = 1;
const auto expectedOptimizedUnpackBucketForMatchOnly = fromjson(R"(
{
    $_internalUnpackBucket: {
        include: ["price", "time", "tag"],
        timeField: 'time',
        metaField: 'tag',
        bucketMaxSpanSeconds: 3600,
        eventFilter: {price: {$gte: 100}}
    }
}
)");

TEST_F(TopKSortOptimization, MatchOnlyAfterTopKSortPushedDownWithTopKSortOptimizationApplied) {
    // The $match stage is after the $sort stage.
    auto pipeline =
        pipeline_factory::makePipeline(makeVector(commonInitialUnpackSpecObj,
                                                  fromjson("{$sort: {time: 1}}"),
                                                  fromjson("{$match: {price: {$gte: 100}}}"),
                                                  firstLastGroupSpecObj),
                                       getExpCtx(),
                                       pipeline_factory::kOptionsMinimal);

    ASSERT_EQ(pipeline->size(), 4U);

    pipeline_optimization::optimizePipeline(*pipeline);

    // The $match stage should be pushed down before the $_internalUnpackBucket and the
    // $_internalUnpackBucket should have the event filter and the $sort stage should be absorbed
    // into the $group stage.
    auto actualOptimizedPipeline = pipeline->serializeToBson();
    ASSERT_EQ(actualOptimizedPipeline.size(), 3U)
        << "Expected three stages but got: " << to_string(actualOptimizedPipeline);

    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedGroupIndexForMatchOnly],
                      expectedOptimizedGroupForMatchOnly);
    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedMatchIndexForMatchOnly],
                      expectedOptimizedMatchForMatchOnly);
    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedUnpackBucketIndexForMatchOnly],
                      expectedOptimizedUnpackBucketForMatchOnly);
}

TEST_F(TopKSortOptimization, MatchOnlyBeforeTopKSortPushedDownWithTopKSortOptimizationApplied) {
    // The $match stage is before the $sort stage.
    auto pipeline =
        pipeline_factory::makePipeline(makeVector(commonInitialUnpackSpecObj,
                                                  fromjson("{$match: {price: {$gte: 100}}}"),
                                                  fromjson("{$sort: {time: 1}}"),
                                                  firstLastGroupSpecObj),
                                       getExpCtx(),
                                       pipeline_factory::kOptionsMinimal);

    ASSERT_EQ(pipeline->size(), 4U);

    pipeline_optimization::optimizePipeline(*pipeline);

    // The $match stage should be pushed down before the $_internalUnpackBucket and the
    // $_internalUnpackBucket should have the event filter and the $sort stage should be absorbed
    // into the $group stage.
    auto actualOptimizedPipeline = pipeline->serializeToBson();
    ASSERT_EQ(actualOptimizedPipeline.size(), 3U)
        << "Expected three stages but got: " << to_string(actualOptimizedPipeline);

    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedGroupIndexForMatchOnly],
                      expectedOptimizedGroupForMatchOnly);
    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedMatchIndexForMatchOnly],
                      expectedOptimizedMatchForMatchOnly);
    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedUnpackBucketIndexForMatchOnly],
                      expectedOptimizedUnpackBucketForMatchOnly);
}

const auto firstSumGroupSpecObj = fromjson(R"(
{
    $group: {
        _id: {hour: {$dateTrunc: {date: '$time', unit: 'hour'}}, symbol: '$s'},
        open: {$first: '$price'},
        totalVol: {$sum: '$vol'}
    }
}
)");

// The $group should absorb the $sort stage by replacing $first by $top.
constexpr size_t kExpectedGroupIndexForMatchAndProject = 2;
const auto expectedOptimizedGroupForMatchAndProject = fromjson(R"(
{
    $group: {
        _id: {hour: {$dateTrunc: {date: '$time', unit: {$const: 'hour'}}}, symbol: '$s'},
        open: {$top: {output: '$price', sortBy: {s: 1}}},
        totalVol: {$sum: '$vol'},
        $willBeMerged: false
    }
}
)");

// The $_internalUnpackBucket should have the event filter while absorbing the $match and also have
// the computed meta field for 's' for renaming 'tag.symbol' to 's'.
constexpr size_t kExpectedUnpackBucketIndexForMatchAndProject = 1;
const auto expectedOptimizedUnpackBucketForMatchAndProject = fromjson(R"(
{
    $_internalUnpackBucket: {
        include: ['_id', 'tag'],
        timeField: 'time',
        metaField: 'tag',
        bucketMaxSpanSeconds: 3600
    }
}
    )");

// The $match stage should be pushed down before the $_internalUnpackBucket because the predicate is
// on subfield of meta. The $project stages gets absorbed into the unpack stage.
constexpr size_t kExpectedMatchIndexForMatchAndProject = 0;
const auto expectedOptimizedMatch = fromjson(R"(
{
    $match: {'meta.symbol': {$in: ['abc', 'bcd']}}
}
)");

TEST_F(TopKSortOptimization,
       Project_Match_Before_TopKSort_Optimized_WithTopKSortOptimizationApplied) {
    auto pipeline = pipeline_factory::makePipeline(
        makeVector(commonInitialUnpackSpecObj,
                   fromjson("{$project: {tag: 1}}"),
                   // The $match should be pushed before the $_internalUnpackBucket.
                   fromjson("{$match: {'tag.symbol': {$in: ['abc', 'bcd']}}}"),
                   // And yet the $sort can be absorbed into the $group stage.
                   fromjson("{$sort: {s: 1}}"),
                   firstSumGroupSpecObj),
        getExpCtx(),
        pipeline_factory::kOptionsMinimal);

    ASSERT_EQ(pipeline->size(), 5U);

    pipeline_optimization::optimizePipeline(*pipeline);

    // The $match stage should be pushed down before the $_internalUnpackBucket and the $sort stage
    // should be absorbed into the $group stage. The $project stage should be absorbed into the
    // $_internalUnpackBucket stage.
    auto actualOptimizedPipeline = pipeline->serializeToBson();
    ASSERT_EQ(actualOptimizedPipeline.size(), 3U)
        << "Expected three stages but got: " << to_string(actualOptimizedPipeline);

    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedGroupIndexForMatchAndProject],
                      expectedOptimizedGroupForMatchAndProject);
    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedUnpackBucketIndexForMatchAndProject],
                      expectedOptimizedUnpackBucketForMatchAndProject);
    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedMatchIndexForMatchAndProject],
                      expectedOptimizedMatch);
}

TEST_F(TopKSortOptimization,
       Match_Project_Before_TopKSort_Optimized_WithTopKSortOptimizationApplied) {
    auto pipeline = pipeline_factory::makePipeline(
        makeVector(commonInitialUnpackSpecObj,
                   // The $match should be pushed before the $_internalUnpackBucket.
                   fromjson("{$match: {'tag.symbol': {$in: ['abc', 'bcd']}}}"),
                   // Renames 'tag.symbol' to 's'.
                   fromjson("{$project: {s: '$tag.symbol'}}"),
                   // And yet the $sort can be absorbed into the $group stage.
                   fromjson("{$sort: {s: 1}}"),
                   firstSumGroupSpecObj),
        getExpCtx(),
        pipeline_factory::kOptionsMinimal);

    ASSERT_EQ(pipeline->size(), 5U);

    pipeline_optimization::optimizePipeline(*pipeline);

    // The $match stage should be pushed down before the $_internalUnpackBucket and the $sort stage
    // should be absorbed into the $group stage. The $project stage should be absorbed into the
    // $_internalUnpackBucket stage.
    auto actualOptimizedPipeline = pipeline->serializeToBson();
    ASSERT_EQ(actualOptimizedPipeline.size(), 4U)
        << "Expected four stages but got: " << to_string(actualOptimizedPipeline);

    // Hides the global 'kExpectedGroupIndex' intentionally since the optimized query is slightly
    // different.
    const size_t kExpectedGroupIndex = 3;
    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedGroupIndex],
                      expectedOptimizedGroupForMatchAndProject);

    // Hides the global 'kExpectedGroupIndex' & 'expectedOptimizedUnpackBucket' intentionally since
    // the optimized query is slightly different.
    const size_t kExpectedUnpackBucketIndex = 2;
    const auto expectedOptimizedUnpackBucket = fromjson(R"(
{
    $_internalUnpackBucket: {
        include: ['_id', 's'],
        timeField: 'time',
        metaField: 'tag',
        bucketMaxSpanSeconds: 3600,
        computedMetaProjFields: ['s']
    }
}
        )");
    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedUnpackBucketIndex],
                      expectedOptimizedUnpackBucket);

    const size_t kExpectedAddFieldsIndex = 1;
    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedAddFieldsIndex],
                      fromjson("{ $addFields: { s: '$meta.symbol' } }"));

    // The $match stage should be pushed down before the $_internalUnpackBucket.
    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedMatchIndexForMatchAndProject],
                      expectedOptimizedMatch);
}

TEST_F(TopKSortOptimization,
       Project_TopKSort_Then_Match_Optimized_WithTopKSortOptimizationApplied) {
    auto pipeline = pipeline_factory::makePipeline(
        makeVector(commonInitialUnpackSpecObj,
                   fromjson("{$project: {tag: 1}}"),
                   // And yet the $sort can be absorbed into the $group stage.
                   fromjson("{$sort: {'s': 1}}"),
                   // The $match should be pushed before the $_internalUnpackBucket.
                   fromjson("{$match: {'tag.symbol': {$in: ['abc', 'bcd']}}}"),
                   firstSumGroupSpecObj),
        getExpCtx(),
        pipeline_factory::kOptionsMinimal);

    ASSERT_EQ(pipeline->size(), 5U);

    pipeline_optimization::optimizePipeline(*pipeline);

    // The $match stage should be pushed before the $_internalUnpackBucket and the
    // $_internalUnpackBucket absorb the $project stage. The $sort stage should be absorbed
    // into the $group stage.
    auto actualOptimizedPipeline = pipeline->serializeToBson();
    ASSERT_EQ(actualOptimizedPipeline.size(), 3U)
        << "Expected three stages but got: " << to_string(actualOptimizedPipeline);

    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedGroupIndexForMatchAndProject],
                      expectedOptimizedGroupForMatchAndProject);
    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedUnpackBucketIndexForMatchAndProject],
                      expectedOptimizedUnpackBucketForMatchAndProject);
    ASSERT_BSONOBJ_EQ(actualOptimizedPipeline[kExpectedMatchIndexForMatchAndProject],
                      expectedOptimizedMatch);
}
}  // namespace
}  // namespace mongo
