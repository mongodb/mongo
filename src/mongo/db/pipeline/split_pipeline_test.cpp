// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/split_pipeline.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_internal_hybrid_search.h"
#include "mongo/db/pipeline/pipeline_factory.h"

#include <deque>
#include <string_view>

namespace mongo::sharded_agg_helpers {
namespace {

class SplitPipelineTest : public AggregationContextFixture {
public:
    void checkPipelineContents(const Pipeline* pipeline,
                               std::deque<std::string_view> expectedStages) {
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
        std::deque<std::string_view> shardsStages;
        std::deque<std::string_view> mergeStages;
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

TEST_F(SplitPipelineTest, HybridSearchMarkerIsMovedToShardsHalf) {
    // The trailing $_internalHybridSearch marker lands on the merge half ($sort splits first);
    // the splitter must move it to the shards half, where collection acquisitions re-validate
    // canRunOnTimeseries.
    auto pipeline = pipeline_factory::makePipeline(
        {BSON("$match" << BSON("x" << 1)), BSON("$sort" << BSON("x" << 1))},
        getExpCtx(),
        pipeline_factory::kOptionsMinimal);
    pipeline->addFinalSource(make_intrusive<DocumentSourceInternalHybridSearch>(getExpCtx()));

    auto splitPipeline = SplitPipeline::split(std::move(pipeline));
    checkPipelineContents(splitPipeline.shardsPipeline.get(),
                          {"$match", "$sort", "$_internalHybridSearch"});
    checkPipelineContents(splitPipeline.mergePipeline.get(), {});
}

TEST_F(SplitPipelineTest, HybridSearchMarkerInShardsHalfIsNotDuplicated) {
    // A marker that already pushed down whole (no split point) must not be added twice.
    auto pipeline = pipeline_factory::makePipeline(
        {BSON("$match" << BSON("x" << 1))}, getExpCtx(), pipeline_factory::kOptionsMinimal);
    pipeline->addFinalSource(make_intrusive<DocumentSourceInternalHybridSearch>(getExpCtx()));

    auto splitPipeline = SplitPipeline::split(std::move(pipeline));
    checkPipelineContents(splitPipeline.shardsPipeline.get(), {"$match", "$_internalHybridSearch"});
    checkPipelineContents(splitPipeline.mergePipeline.get(), {});
}

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

TEST_F(SplitPipelineTest, PushDownIdLookup) {
    // IdLookup should be pushed down without forcing a pipeline split.
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
                    "$match",
                },
            .mergeStages = {},
            .sortSpec = boost::none,
        });
}

}  // namespace
}  // namespace mongo::sharded_agg_helpers
