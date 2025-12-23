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

#include "mongo/db/pipeline/split_pipeline.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/pipeline_factory.h"

#include <deque>

namespace mongo::sharded_agg_helpers {
namespace {

class SplitPipelineTest : public AggregationContextFixture {
public:
    void checkPipelineContents(const Pipeline* pipeline, std::deque<StringData> expectedStages) {
        ASSERT_EQ(pipeline->size(), expectedStages.size());
        for (auto stage : pipeline->getSources()) {
            ASSERT_EQ(stage->getSourceName(), expectedStages.front());
            expectedStages.pop_front();
        }
    }

    void checkSortSpec(const SplitPipeline& splitPipeline,
                       boost::optional<BSONObj> expectedSortSpec) {
        ASSERT_EQ(splitPipeline.shardCursorsSortSpec.has_value(), expectedSortSpec.has_value());
        if (expectedSortSpec) {
            ASSERT_BSONOBJ_EQ(*splitPipeline.shardCursorsSortSpec, *expectedSortSpec);
        }
    }

    struct ExpectedSplitPipeline {
        std::deque<StringData> shardsStages;
        std::deque<StringData> mergeStages;
        boost::optional<BSONObj> sortSpec;
    };
    SplitPipeline testPipelineSplit(const std::vector<BSONObj>& inputPipeline,
                                    const ExpectedSplitPipeline& expectedSplitPipeline) {
        auto pipeline = pipeline_factory::makePipeline(
            inputPipeline, getExpCtx(), pipeline_factory::kOptionsMinimal);

        auto actualSplitPipeline = SplitPipeline::split(std::move(pipeline));

        checkPipelineContents(actualSplitPipeline.shardsPipeline.get(),
                              expectedSplitPipeline.shardsStages);
        checkPipelineContents(actualSplitPipeline.mergePipeline.get(),
                              expectedSplitPipeline.mergeStages);
        checkSortSpec(actualSplitPipeline, expectedSplitPipeline.sortSpec);

        return actualSplitPipeline;
    }
};

TEST_F(SplitPipelineTest, SimpleIdLookupPushdown) {
    // When IdLookup is the first stage in the merging pipeline it should be pushed down.
    testPipelineSplit(
        {
            BSON("$vectorSearch" << BSONObj()),
            BSON("$_internalSearchIdLookup" << BSONObj()),
        },
        ExpectedSplitPipeline{
            .shardsStages =
                {
                    "$vectorSearch",
                    "$_internalSearchIdLookup",
                },
            .mergeStages = {},
            .sortSpec = BSON("$vectorSearchScore" << -1),
        });
}

TEST_F(SplitPipelineTest, PushdownIdLookupOverLimit) {
    // When IdLookup is only preceded by a $limit in the merging pipeline, it should be pushed down.
    testPipelineSplit(
        {
            BSON("$vectorSearch" << BSON("limit" << 10)),
            BSON("$_internalSearchIdLookup" << BSONObj()),
            BSON("$match" << BSON("x" << 5)),
            BSON("$sort" << BSON("y" << 1)),
        },
        ExpectedSplitPipeline{
            .shardsStages =
                {
                    "$vectorSearch",
                    "$_internalSearchIdLookup",
                    "$limit",
                },
            .mergeStages =
                {
                    "$limit",
                    "$match",
                    "$sort",
                },
            .sortSpec = BSON("$vectorSearchScore" << -1),
        });
}

TEST_F(SplitPipelineTest, PushdownIdLookupOverMultipleLimits) {
    // IdLookup should be pushed down even over multiple $limits.
    testPipelineSplit(
        {
            BSON("$limit" << 100),
            BSON("$limit" << 10),
            BSON("$_internalSearchIdLookup" << BSONObj()),
        },
        ExpectedSplitPipeline{
            .shardsStages =
                {
                    "$limit",
                    "$_internalSearchIdLookup",
                    "$limit",
                },
            .mergeStages =
                {
                    "$limit",
                    "$limit",
                },
            .sortSpec = boost::none,
        });
}

TEST_F(SplitPipelineTest, PushdownIdLookupDoesNotBlockLimitFieldsSentToMergerOptimization) {
    // IdLookup should be pushed down even over multiple $limits.
    testPipelineSplit(
        {
            BSON("$limit" << 10),
            BSON("$_internalSearchIdLookup" << BSONObj()),
            BSON("$project" << BSON("x" << 1)),
        },
        ExpectedSplitPipeline{
            .shardsStages =
                {
                    "$limit",
                    "$_internalSearchIdLookup",
                    "$limit",
                    "$project",
                },
            .mergeStages =
                {
                    "$limit",
                    "$project",
                },
            .sortSpec = boost::none,
        });
}

TEST_F(SplitPipelineTest, IdLookupInInvalidMergingPipelineLocation) {
    // When IdLookup is preceded by a stage other than $limit in the merging pipeline, it should
    // error.
    ASSERT_THROWS_CODE(testPipelineSplit(
                           {
                               BSON("$sort" << BSON("x" << 1)),
                               BSON("$count" << "count"),
                               BSON("$_internalSearchIdLookup" << BSONObj()),
                           },
                           ExpectedSplitPipeline{}),
                       AssertionException,
                       11027701);
}

TEST_F(SplitPipelineTest, SplitAtIdLookup) {
    // When IdLookup is the first stage in the pipeline that requires a split, it should end up on
    // the shards.
    testPipelineSplit(
        {
            BSON("$match" << BSON("x" << 1)),
            BSON("$_internalSearchIdLookup" << BSONObj()),
            BSON("$match" << BSON("y" << 1)),
        },
        ExpectedSplitPipeline{
            .shardsStages =
                {
                    "$match",
                    "$_internalSearchIdLookup",
                },
            .mergeStages = {"$match"},
            .sortSpec = boost::none,
        });
}

}  // namespace
}  // namespace mongo::sharded_agg_helpers
