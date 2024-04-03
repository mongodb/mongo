/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/pipeline/visitors/document_source_visitor_docs_needed_bounds.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_bucket_auto.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/s/sharding_state.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

namespace mongo {
class DocsNeededBoundsTest : public AggregationContextFixture {
protected:
    DocsNeededBoundsTest() {
        // Need to intialize the sharding state so that any pipelines that use $lookup pass pipeline
        // validation.
        ShardingState::create(getServiceContext());
    }

    std::pair<DocsNeededBounds, DocsNeededBounds> buildPipelineAndExtractBounds(
        Pipeline::SourceContainer sources) {
        auto pipeline = Pipeline::create(sources, getExpCtx());
        return extractDocsNeededBounds(*pipeline);
    }

    void assertDiscreteAndEq(DocsNeededBounds bounds, long long expectedValue) {
        ASSERT_TRUE(std::holds_alternative<DiscreteValue>(bounds));
        ASSERT_EQUALS(std::get<DiscreteValue>(bounds).value, expectedValue);
    }

    void assertUnknown(DocsNeededBounds bounds) {
        ASSERT_TRUE(std::holds_alternative<Unknown>(bounds));
    }

    void assertNeedAll(DocsNeededBounds bounds) {
        ASSERT_TRUE(std::holds_alternative<NeedAll>(bounds));
    }

    // Helper functions for creating document source stages for more consise pipeline building
    // below. For the sake of these tests, $limit, $skip, and $unwind are the only stages for which
    // we care about the parameters applied to the stage.

    auto limit(long long limit) {
        return DocumentSourceLimit::create(getExpCtx(), limit);
    }

    auto skip(long long skip) {
        return DocumentSourceSkip::create(getExpCtx(), skip);
    }

    auto search() {
        auto expCtx = getExpCtx();
        const auto mongotQuery = fromjson("{term: 'mykeyword'}");
        return DocumentSourceSearch::createFromBson(BSON("$search" << mongotQuery).firstElement(),
                                                    expCtx);
    }

    auto unwind(bool includeNullIfEmptyOrMissing) {
        return DocumentSourceUnwind::create(
            getExpCtx(), "array", includeNullIfEmptyOrMissing, boost::none);
    }

    auto sort() {
        return DocumentSourceSort::create(getExpCtx(), BSON("a" << 1));
    }

    auto match() {
        return DocumentSourceMatch::create(BSON("a" << 1), getExpCtx());
    }

    auto project() {
        return DocumentSourceProject::create(BSON("a" << 1), getExpCtx(), "$project"_sd);
    }

    auto sample() {
        return DocumentSourceSample::create(getExpCtx(), 5);
    }

    auto unionWith() {
        auto expCtx = getExpCtx();
        NamespaceString nsToUnionWith =
            NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "coll");
        expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
            {nsToUnionWith.coll().toString(), {nsToUnionWith, std::vector<BSONObj>()}}});
        auto bson =
            BSON("$unionWith" << BSON(
                     "coll" << nsToUnionWith.coll() << "pipeline"
                            << BSON_ARRAY(BSON("$addFields" << BSON("a" << BSON("$const" << 3))))));
        return DocumentSourceUnionWith::createFromBson(bson.firstElement(), expCtx);
    }

    auto facet() {
        auto expCtx = getExpCtx();
        std::vector<DocumentSourceFacet::FacetPipeline> facets;
        facets.emplace_back("subpipeline", Pipeline::create({}, expCtx));
        return DocumentSourceFacet::create(std::move(facets), expCtx);
    }

    auto group() {
        auto expCtx = getExpCtx();
        VariablesParseState vps = expCtx->variablesParseState;
        auto x = ExpressionFieldPath::parse(expCtx.get(), "$x", vps);
        return DocumentSourceGroup::create(expCtx, x, {});
    }

    auto bucketAuto() {
        auto expCtx = getExpCtx();
        VariablesParseState vps = expCtx->variablesParseState;
        auto groupByExpression = ExpressionFieldPath::parse(expCtx.get(), "$a", vps);
        const size_t maxMemoryUsageBytes = 2000;
        const int numBuckets = 2;
        return DocumentSourceBucketAuto::create(
            expCtx, groupByExpression, numBuckets, {}, nullptr, maxMemoryUsageBytes);
    }

    auto setWindowFields() {
        auto spec = fromjson(R"(
            {$_internalSetWindowFields: {output: {'x.y.z':
            {$sum: 1}}}})");
        return DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(),
                                                                     getExpCtx());
    }

    auto lookup() {
        auto expCtx = getExpCtx();
        NamespaceString lookupNs =
            NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "coll");
        expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
            {lookupNs.coll().toString(), {lookupNs, std::vector<BSONObj>()}}});
        auto bson = BSON("$lookup" << BSON("from" << lookupNs.coll() << "as"
                                                  << "out"
                                                  << "localField"
                                                  << "foo_id"
                                                  << "foreignField"
                                                  << "_id"));
        return DocumentSourceLookUp::createFromBson(bson.firstElement(), expCtx);
    }

    auto lookupWithUnwind(bool includeNullIfEmptyOrMissing) {
        auto lookupStage = lookup();
        auto unwindStage = unwind(includeNullIfEmptyOrMissing);
        static_cast<DocumentSourceLookUp*>(lookupStage.get())->setUnwindStage(unwindStage);
        return lookupStage;
    }

    auto setVariableFromSubPipeline() {
        auto expCtx = getExpCtx();
        auto ctxForSubPipeline = expCtx->copyForSubPipeline(expCtx->ns);
        return DocumentSourceSetVariableFromSubPipeline::create(
            expCtx, Pipeline::create({}, ctxForSubPipeline), Variables::kSearchMetaId);
    }

    auto internalSearchIdLookup() {
        return DocumentSourceInternalSearchIdLookUp::createFromBson(
            BSON("$_internalSearchIdLookup" << BSONObj()).firstElement(), getExpCtx());
    }
};

/**
 * The tests below each build a pipeline, then run it through extractDocsNeededBounds() and
 * confirm that the constraints were computed properly.
 */

TEST_F(DocsNeededBoundsTest, EmptyPipeline) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, Limit) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({limit(10)});
    assertDiscreteAndEq(minBounds, 10);
    assertDiscreteAndEq(maxBounds, 10);
}

TEST_F(DocsNeededBoundsTest, SkipLimit) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({skip(15), limit(10)});
    assertDiscreteAndEq(minBounds, 25);
    assertDiscreteAndEq(maxBounds, 25);
}

TEST_F(DocsNeededBoundsTest, SkipOverflow) {
    // If the skip value causes the bounds to overflow 64 bits, NeedAll should be returned.
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({skip(LLONG_MAX), limit(10)});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, Sort) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({sort()});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, Search) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({search()});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SearchSort) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({search(), sort()});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SearchLimit) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({search(), limit(15)});
    assertDiscreteAndEq(minBounds, 15);
    assertDiscreteAndEq(maxBounds, 15);
}

TEST_F(DocsNeededBoundsTest, SearchLimitLimitLimit1) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({search(), limit(20), limit(30), limit(15)});
    assertDiscreteAndEq(minBounds, 15);
    assertDiscreteAndEq(maxBounds, 15);
}

TEST_F(DocsNeededBoundsTest, SearchLimitLimitLimit2) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({search(), limit(15), limit(30), limit(20)});
    assertDiscreteAndEq(minBounds, 15);
    assertDiscreteAndEq(maxBounds, 15);
}

TEST_F(DocsNeededBoundsTest, LimitSkipLimit) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({limit(15), skip(7), limit(5)});
    assertDiscreteAndEq(minBounds, 12);
    assertDiscreteAndEq(maxBounds, 12);
}

TEST_F(DocsNeededBoundsTest, SkipSkipLimit) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({skip(15), skip(7), limit(5)});
    assertDiscreteAndEq(minBounds, 27);
    assertDiscreteAndEq(maxBounds, 27);
}

TEST_F(DocsNeededBoundsTest, LimitSkipSkip) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({limit(15), skip(2), skip(4)});
    assertDiscreteAndEq(minBounds, 15);
    assertDiscreteAndEq(maxBounds, 15);
}

TEST_F(DocsNeededBoundsTest, SkipSkipSkip) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({skip(15), skip(5), skip(7)});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SkipLimitSortSkipLimit) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({skip(5), limit(25), sort(), skip(3), limit(7)});
    assertDiscreteAndEq(minBounds, 30);
    assertDiscreteAndEq(maxBounds, 30);
}

TEST_F(DocsNeededBoundsTest, SortSkipLimit) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({sort(), skip(5), limit(25)});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, Match) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({match()});
    assertUnknown(maxBounds);
    assertUnknown(minBounds);
}

TEST_F(DocsNeededBoundsTest, SkipMatchLimit) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({skip(8), match(), limit(25)});
    assertDiscreteAndEq(minBounds, 33);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SkipLimitMatch) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({skip(8), limit(25), match()});
    assertDiscreteAndEq(minBounds, 33);
    assertDiscreteAndEq(maxBounds, 33);
}

TEST_F(DocsNeededBoundsTest, MatchSkipLimit) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({match(), skip(8), limit(25)});
    assertDiscreteAndEq(minBounds, 33);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SkipLimitMatchSkipLimit1) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({skip(3), limit(4), match(), skip(8), limit(25)});
    assertDiscreteAndEq(minBounds, 7);
    assertDiscreteAndEq(maxBounds, 7);
}

TEST_F(DocsNeededBoundsTest, SkipLimitMatchSkipLimit2) {
    // Walking the pipeline in reverse, the {$limit: 7}, {$skip: 3} sequence imposes upper and lower
    // bounds of 10, but the $match overrides the upper bound to Unknown. Then, {$limit: 150}
    // constrains the upper bounds, but not the lower bounds since the existing bounds of 10 is
    // smaller. Last, {$skip: 50} is added to upper and lower bounds.
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({skip(50), limit(150), match(), skip(3), limit(7)});
    assertDiscreteAndEq(minBounds, 60);
    assertDiscreteAndEq(maxBounds, 200);
}

TEST_F(DocsNeededBoundsTest, MatchSortSkipLimit) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({match(), sort(), skip(3), limit(7)});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, MatchSkipLimitSort) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({match(), skip(3), limit(7), sort()});
    assertDiscreteAndEq(minBounds, 10);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, MatchSkipSortLimit) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({match(), skip(3), sort(), limit(7)});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SearchUnionWith) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({search(), unionWith()});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SearchSkipUnionWithLimit) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({search(), skip(8), unionWith(), limit(25)});
    assertUnknown(minBounds);
    assertDiscreteAndEq(maxBounds, 33);
}

TEST_F(DocsNeededBoundsTest, SearchSkipLimitUnionWith) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({search(), skip(8), limit(25), unionWith()});
    assertDiscreteAndEq(minBounds, 33);
    assertDiscreteAndEq(maxBounds, 33);
}

TEST_F(DocsNeededBoundsTest, SearchUnionWithSkipLimit) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({search(), unionWith(), skip(8), limit(25)});
    assertUnknown(minBounds);
    assertDiscreteAndEq(maxBounds, 33);
}

TEST_F(DocsNeededBoundsTest, SkipLimitUnionWithSkipLimit1) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({skip(3), limit(4), unionWith(), skip(8), limit(25)});
    assertDiscreteAndEq(minBounds, 7);
    assertDiscreteAndEq(maxBounds, 7);
}

TEST_F(DocsNeededBoundsTest, SkipLimitUnionWithSkipLimit2) {
    // Walking the pipeline in reverse, the {$limit: 7}, {$skip: 3} sequence imposes upper and lower
    // bounds of 10, but the $unionWith overrides the lower bound to Unknown (i.e., it's possible we
    // need even fewer than 10). Then, {$limit: 150} has no affect on the bounds since the existing
    // upper bounds is already smaller. Last, {$skip: 50} is added to the bounds.
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({skip(50), limit(150), unionWith(), skip(3), limit(7)});
    assertUnknown(minBounds);
    assertDiscreteAndEq(maxBounds, 60);
}

TEST_F(DocsNeededBoundsTest, UnionWithSortSkipLimit) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({unionWith(), sort(), skip(3), limit(7)});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, UnionWithSkipLimitSort) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({unionWith(), skip(3), limit(7), sort()});
    assertUnknown(minBounds);
    assertDiscreteAndEq(maxBounds, 10);
}

TEST_F(DocsNeededBoundsTest, UnionWithSkipSortLimit) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({unionWith(), skip(3), sort(), limit(7)});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SearchUnwind1) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({search(), unwind(/*includeNullIfEmptyOrMissing*/ false)});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SearchUnwind2) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({search(), unwind(/*includeNullIfEmptyOrMissing*/ true)});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SkipUnwindLimit1) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds(
        {skip(8), unwind(/*includeNullIfEmptyOrMissing*/ false), limit(25)});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SkipUnwindLimit2) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds(
        {skip(8), unwind(/*includeNullIfEmptyOrMissing*/ true), limit(25)});
    assertUnknown(minBounds);
    assertDiscreteAndEq(maxBounds, 33);
}

TEST_F(DocsNeededBoundsTest, SearchSkipLimitUnwind) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds(
        {search(), skip(8), limit(25), unwind(/*includeNullIfEmptyOrMissing*/ false)});
    assertDiscreteAndEq(minBounds, 33);
    assertDiscreteAndEq(maxBounds, 33);
}

TEST_F(DocsNeededBoundsTest, UnwindSkipLimit1) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds(
        {unwind(/*includeNullIfEmptyOrMissing*/ false), skip(8), limit(25)});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, UnwindSkipLimit2) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds(
        {unwind(/*includeNullIfEmptyOrMissing*/ true), skip(8), limit(25)});
    assertUnknown(minBounds);
    assertDiscreteAndEq(maxBounds, 33);
}

TEST_F(DocsNeededBoundsTest, SkipLimitUnwindSkipLimit1) {
    // This is an odd pipeline. Technically, we can infer nothing because of the $unwind. However,
    // for the sake of algorithmic simplicity, we'll assume the pipeline is written well, such that
    // we can base the constraints on the earliest $skip+$limit.
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds(
        {skip(3), limit(5), unwind(/*includeNullIfEmptyOrMissing*/ false), skip(8), limit(25)});
    assertDiscreteAndEq(minBounds, 8);
    assertDiscreteAndEq(maxBounds, 8);
}

TEST_F(DocsNeededBoundsTest, SkipLimitUnwindSkipLimit2) {
    // Same note as SkipLimitUnwindSkipLimit1.
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds(
        {skip(50), limit(150), unwind(/*includeNullIfEmptyOrMissing*/ false), skip(3), limit(7)});
    assertDiscreteAndEq(minBounds, 200);
    assertDiscreteAndEq(maxBounds, 200);
}

TEST_F(DocsNeededBoundsTest, UnwindSortSkipLimit) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds(
        {unwind(/*includeNullIfEmptyOrMissing*/ false), sort(), skip(3), limit(7)});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, UnwindSkipLimitSort) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds(
        {unwind(/*includeNullIfEmptyOrMissing*/ false), skip(3), limit(7), sort()});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, UnwindSkipSortLimit) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds(
        {unwind(/*includeNullIfEmptyOrMissing*/ false), skip(3), sort(), limit(7)});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, ProjectSkipLimit) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({project(), skip(3), limit(7)});
    assertDiscreteAndEq(minBounds, 10);
    assertDiscreteAndEq(maxBounds, 10);
}

TEST_F(DocsNeededBoundsTest, SkipLimitProject) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({skip(15), limit(35), project()});
    assertDiscreteAndEq(minBounds, 50);
    assertDiscreteAndEq(maxBounds, 50);
}

TEST_F(DocsNeededBoundsTest, ProjectMatchSort) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({project(), match(), sort()});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SearchFacet) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({search(), facet()});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SearchFacetSort) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({search(), facet(), sort()});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, FacetProjectLimit) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({facet(), project(), limit(30)});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, LimitProjectFacet) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({limit(70), project(), facet()});
    assertDiscreteAndEq(minBounds, 70);
    assertDiscreteAndEq(maxBounds, 70);
}

TEST_F(DocsNeededBoundsTest, MatchSortFacet) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({match(), sort(), facet()});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, ProjectGroup) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({project(), group()});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, ProjectSample) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({project(), sample()});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, ProjectBucketAuto) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({project(), bucketAuto()});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, ProjectLookup) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({project(), lookup()});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, LimitLookup) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({limit(25), lookup()});
    assertDiscreteAndEq(minBounds, 25);
    assertDiscreteAndEq(maxBounds, 25);
}

TEST_F(DocsNeededBoundsTest, LookupWithUnwindLimit1) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds(
        {lookupWithUnwind(/*includeNullIfEmptyOrMissing*/ true), limit(25)});
    assertUnknown(minBounds);
    assertDiscreteAndEq(maxBounds, 25);
}

TEST_F(DocsNeededBoundsTest, LookupWithUnwindLimit2) {
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds(
        {lookupWithUnwind(/*includeNullIfEmptyOrMissing*/ false), limit(25)});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SearchSetVariableFromSubPipelineLimit) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({search(), setVariableFromSubPipeline(), limit(15)});
    assertDiscreteAndEq(minBounds, 15);
    assertDiscreteAndEq(maxBounds, 15);
}

TEST_F(DocsNeededBoundsTest, SearchSetVariableFromSubPipelineGroup) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({search(), setVariableFromSubPipeline(), group()});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, SearchBucketAutoSetVariableFromSubPipelineLookup) {
    auto [minBounds, maxBounds] =
        buildPipelineAndExtractBounds({search(), bucketAuto(), setVariableFromSubPipeline()});
    assertNeedAll(minBounds);
    assertNeedAll(maxBounds);
}

TEST_F(DocsNeededBoundsTest, InternalSearchIdLookup) {
    // In the context of using these bounds for mongot batchSize tuning, $_internalSearchIdLookUp
    // should never be encountered since this algorithm is run prior to desugaring $search.
    // For that reason, this stage does not have an implemented visitor and should fall into the
    // "unknown" case.
    auto [minBounds, maxBounds] = buildPipelineAndExtractBounds({internalSearchIdLookup()});
    assertUnknown(minBounds);
    assertUnknown(maxBounds);
}
}  // namespace mongo
