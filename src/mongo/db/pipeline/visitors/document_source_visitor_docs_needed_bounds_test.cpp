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
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
class VisitorDocsNeededBoundsTest : public AggregationContextFixture {
protected:
    VisitorDocsNeededBoundsTest() {
        // Need to intialize the sharding state so that any pipelines that use $lookup pass pipeline
        // validation.
        ShardingState::create(getServiceContext());
    }

    DocsNeededBounds buildPipelineAndExtractBounds(DocumentSourceContainer sources) {
        auto pipeline = Pipeline::create(sources, getExpCtx());
        return extractDocsNeededBounds(*pipeline);
    }

    void assertDiscreteAndEq(DocsNeededConstraint bounds, long long expectedValue) {
        ASSERT_TRUE(std::holds_alternative<long long>(bounds));
        ASSERT_EQUALS(std::get<long long>(bounds), expectedValue);
    }

    void assertUnknown(DocsNeededConstraint bounds) {
        ASSERT_TRUE(std::holds_alternative<docs_needed_bounds::Unknown>(bounds));
    }

    void assertNeedAll(DocsNeededConstraint bounds) {
        ASSERT_TRUE(std::holds_alternative<docs_needed_bounds::NeedAll>(bounds));
    }

    // Helper functions for creating document source stages for more concise pipeline building
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
        NamespaceString nsToUnionWith = NamespaceString::createNamespaceString_forTest(
            expCtx->getNamespaceString().dbName(), "coll");
        expCtx->setResolvedNamespaces(
            ResolvedNamespaceMap{{nsToUnionWith, {nsToUnionWith, std::vector<BSONObj>()}}});
        auto bson =
            BSON("$unionWith" << BSON(
                     "coll" << nsToUnionWith.coll() << "pipeline"
                            << BSON_ARRAY(BSON("$addFields" << BSON("a" << BSON("$const" << 3))))));
        return DocumentSourceUnionWith::createFromBson(bson.firstElement(), expCtx);
    }

    auto facet(std::vector<DocumentSourceContainer> subpipelines) {
        auto expCtx = getExpCtx();
        ASSERT(!subpipelines.empty());
        std::vector<DocumentSourceFacet::FacetPipeline> facets;
        for (auto&& subpipeline : subpipelines) {
            facets.emplace_back("subpipeline", Pipeline::create(subpipeline, expCtx));
        }
        return DocumentSourceFacet::create(std::move(facets), expCtx);
    }

    auto group() {
        auto expCtx = getExpCtx();
        VariablesParseState vps = expCtx->variablesParseState;
        auto x = ExpressionFieldPath::parse(expCtx.get(), "$x", vps);
        return DocumentSourceGroup::create(expCtx, x, {}, false);
    }

    auto bucketAuto() {
        auto expCtx = getExpCtx();
        VariablesParseState vps = expCtx->variablesParseState;
        auto groupByExpression = ExpressionFieldPath::parse(expCtx.get(), "$a", vps);
        const int numBuckets = 2;
        return DocumentSourceBucketAuto::create(expCtx, groupByExpression, numBuckets);
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
        NamespaceString lookupNs = NamespaceString::createNamespaceString_forTest(
            expCtx->getNamespaceString().dbName(), "coll");
        expCtx->setResolvedNamespaces(
            ResolvedNamespaceMap{{lookupNs, {lookupNs, std::vector<BSONObj>()}}});
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
        static_cast<DocumentSourceLookUp*>(lookupStage.get())->setUnwindStage_forTest(unwindStage);
        return lookupStage;
    }

    auto setVariableFromSubPipeline() {
        auto expCtx = getExpCtx();
        auto ctxForSubPipeline =
            makeCopyForSubPipelineFromExpressionContext(expCtx, expCtx->getNamespaceString());
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

TEST_F(VisitorDocsNeededBoundsTest, EmptyPipeline) {
    auto bounds = buildPipelineAndExtractBounds({});
    assertUnknown(bounds.getMinBounds());
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, Limit) {
    auto bounds = buildPipelineAndExtractBounds({limit(10)});
    assertDiscreteAndEq(bounds.getMinBounds(), 10);
    assertDiscreteAndEq(bounds.getMaxBounds(), 10);
}

TEST_F(VisitorDocsNeededBoundsTest, SkipLimit) {
    auto bounds = buildPipelineAndExtractBounds({skip(15), limit(10)});
    assertDiscreteAndEq(bounds.getMinBounds(), 25);
    assertDiscreteAndEq(bounds.getMaxBounds(), 25);
}

TEST_F(VisitorDocsNeededBoundsTest, SkipOverflow) {
    // If the skip value causes the bounds to overflow 64 bits, NeedAll should be returned.
    auto bounds = buildPipelineAndExtractBounds({skip(LLONG_MAX), limit(10)});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, Sort) {
    auto bounds = buildPipelineAndExtractBounds({sort()});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, Search) {
    auto bounds = buildPipelineAndExtractBounds({search()});
    assertUnknown(bounds.getMinBounds());
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SearchSort) {
    auto bounds = buildPipelineAndExtractBounds({search(), sort()});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SearchLimit) {
    auto bounds = buildPipelineAndExtractBounds({search(), limit(15)});
    assertDiscreteAndEq(bounds.getMinBounds(), 15);
    assertDiscreteAndEq(bounds.getMaxBounds(), 15);
}

TEST_F(VisitorDocsNeededBoundsTest, SearchLimitLimitLimit1) {
    auto bounds = buildPipelineAndExtractBounds({search(), limit(20), limit(30), limit(15)});
    assertDiscreteAndEq(bounds.getMinBounds(), 15);
    assertDiscreteAndEq(bounds.getMaxBounds(), 15);
}

TEST_F(VisitorDocsNeededBoundsTest, SearchLimitLimitLimit2) {
    auto bounds = buildPipelineAndExtractBounds({search(), limit(15), limit(30), limit(20)});
    assertDiscreteAndEq(bounds.getMinBounds(), 15);
    assertDiscreteAndEq(bounds.getMaxBounds(), 15);
}

TEST_F(VisitorDocsNeededBoundsTest, LimitSkipLimit) {
    auto bounds = buildPipelineAndExtractBounds({limit(15), skip(7), limit(5)});
    assertDiscreteAndEq(bounds.getMinBounds(), 12);
    assertDiscreteAndEq(bounds.getMaxBounds(), 12);
}

TEST_F(VisitorDocsNeededBoundsTest, SkipSkipLimit) {
    auto bounds = buildPipelineAndExtractBounds({skip(15), skip(7), limit(5)});
    assertDiscreteAndEq(bounds.getMinBounds(), 27);
    assertDiscreteAndEq(bounds.getMaxBounds(), 27);
}

TEST_F(VisitorDocsNeededBoundsTest, LimitSkipSkip) {
    auto bounds = buildPipelineAndExtractBounds({limit(15), skip(2), skip(4)});
    assertDiscreteAndEq(bounds.getMinBounds(), 15);
    assertDiscreteAndEq(bounds.getMaxBounds(), 15);
}

TEST_F(VisitorDocsNeededBoundsTest, SkipSkipSkip) {
    auto bounds = buildPipelineAndExtractBounds({skip(15), skip(5), skip(7)});
    assertUnknown(bounds.getMinBounds());
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SkipLimitSortSkipLimit) {
    auto bounds = buildPipelineAndExtractBounds({skip(5), limit(25), sort(), skip(3), limit(7)});
    assertDiscreteAndEq(bounds.getMinBounds(), 30);
    assertDiscreteAndEq(bounds.getMaxBounds(), 30);
}

TEST_F(VisitorDocsNeededBoundsTest, SortSkipLimit) {
    auto bounds = buildPipelineAndExtractBounds({sort(), skip(5), limit(25)});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, Match) {
    auto bounds = buildPipelineAndExtractBounds({match()});
    assertUnknown(bounds.getMaxBounds());
    assertUnknown(bounds.getMinBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SkipMatchLimit) {
    auto bounds = buildPipelineAndExtractBounds({skip(8), match(), limit(25)});
    assertDiscreteAndEq(bounds.getMinBounds(), 33);
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SkipLimitMatch) {
    auto bounds = buildPipelineAndExtractBounds({skip(8), limit(25), match()});
    assertDiscreteAndEq(bounds.getMinBounds(), 33);
    assertDiscreteAndEq(bounds.getMaxBounds(), 33);
}

TEST_F(VisitorDocsNeededBoundsTest, MatchSkipLimit) {
    auto bounds = buildPipelineAndExtractBounds({match(), skip(8), limit(25)});
    assertDiscreteAndEq(bounds.getMinBounds(), 33);
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SkipLimitMatchSkipLimit1) {
    auto bounds = buildPipelineAndExtractBounds({skip(3), limit(4), match(), skip(8), limit(25)});
    assertDiscreteAndEq(bounds.getMinBounds(), 7);
    assertDiscreteAndEq(bounds.getMaxBounds(), 7);
}

TEST_F(VisitorDocsNeededBoundsTest, SkipLimitMatchSkipLimit2) {
    // Walking the pipeline in reverse, the {$limit: 7}, {$skip: 3} sequence imposes upper and lower
    // bounds of 10, but the $match overrides the upper bound to Unknown. Then, {$limit: 150}
    // constrains the upper bounds, but not the lower bounds since the existing bounds of 10 is
    // smaller. Last, {$skip: 50} is added to upper and lower bounds.
    auto bounds = buildPipelineAndExtractBounds({skip(50), limit(150), match(), skip(3), limit(7)});
    assertDiscreteAndEq(bounds.getMinBounds(), 60);
    assertDiscreteAndEq(bounds.getMaxBounds(), 200);
}

TEST_F(VisitorDocsNeededBoundsTest, MatchSortSkipLimit) {
    auto bounds = buildPipelineAndExtractBounds({match(), sort(), skip(3), limit(7)});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, MatchSkipLimitSort) {
    auto bounds = buildPipelineAndExtractBounds({match(), skip(3), limit(7), sort()});
    assertDiscreteAndEq(bounds.getMinBounds(), 10);
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, MatchSkipSortLimit) {
    auto bounds = buildPipelineAndExtractBounds({match(), skip(3), sort(), limit(7)});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SearchUnionWith) {
    auto bounds = buildPipelineAndExtractBounds({search(), unionWith()});
    assertUnknown(bounds.getMinBounds());
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SearchSkipUnionWithLimit) {
    auto bounds = buildPipelineAndExtractBounds({search(), skip(8), unionWith(), limit(25)});
    assertUnknown(bounds.getMinBounds());
    assertDiscreteAndEq(bounds.getMaxBounds(), 33);
}

TEST_F(VisitorDocsNeededBoundsTest, SearchSkipLimitUnionWith) {
    auto bounds = buildPipelineAndExtractBounds({search(), skip(8), limit(25), unionWith()});
    assertDiscreteAndEq(bounds.getMinBounds(), 33);
    assertDiscreteAndEq(bounds.getMaxBounds(), 33);
}

TEST_F(VisitorDocsNeededBoundsTest, SearchUnionWithSkipLimit) {
    auto bounds = buildPipelineAndExtractBounds({search(), unionWith(), skip(8), limit(25)});
    assertUnknown(bounds.getMinBounds());
    assertDiscreteAndEq(bounds.getMaxBounds(), 33);
}

TEST_F(VisitorDocsNeededBoundsTest, SkipLimitUnionWithSkipLimit1) {
    auto bounds =
        buildPipelineAndExtractBounds({skip(3), limit(4), unionWith(), skip(8), limit(25)});
    assertDiscreteAndEq(bounds.getMinBounds(), 7);
    assertDiscreteAndEq(bounds.getMaxBounds(), 7);
}

TEST_F(VisitorDocsNeededBoundsTest, SkipLimitUnionWithSkipLimit2) {
    // Walking the pipeline in reverse, the {$limit: 7}, {$skip: 3} sequence imposes upper and lower
    // bounds of 10, but the $unionWith overrides the lower bound to Unknown (i.e., it's possible we
    // need even fewer than 10). Then, {$limit: 150} has no affect on the bounds since the existing
    // upper bounds is already smaller. Last, {$skip: 50} is added to the bounds.
    auto bounds =
        buildPipelineAndExtractBounds({skip(50), limit(150), unionWith(), skip(3), limit(7)});
    assertUnknown(bounds.getMinBounds());
    assertDiscreteAndEq(bounds.getMaxBounds(), 60);
}

TEST_F(VisitorDocsNeededBoundsTest, UnionWithSortSkipLimit) {
    auto bounds = buildPipelineAndExtractBounds({unionWith(), sort(), skip(3), limit(7)});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, UnionWithSkipLimitSort) {
    auto bounds = buildPipelineAndExtractBounds({unionWith(), skip(3), limit(7), sort()});
    assertUnknown(bounds.getMinBounds());
    assertDiscreteAndEq(bounds.getMaxBounds(), 10);
}

TEST_F(VisitorDocsNeededBoundsTest, UnionWithSkipSortLimit) {
    auto bounds = buildPipelineAndExtractBounds({unionWith(), skip(3), sort(), limit(7)});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SearchUnwind1) {
    auto bounds =
        buildPipelineAndExtractBounds({search(), unwind(/*includeNullIfEmptyOrMissing*/ false)});
    assertUnknown(bounds.getMinBounds());
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SearchUnwind2) {
    auto bounds =
        buildPipelineAndExtractBounds({search(), unwind(/*includeNullIfEmptyOrMissing*/ true)});
    assertUnknown(bounds.getMinBounds());
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SkipUnwindLimit1) {
    auto bounds = buildPipelineAndExtractBounds(
        {skip(8), unwind(/*includeNullIfEmptyOrMissing*/ false), limit(25)});
    assertUnknown(bounds.getMinBounds());
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SkipUnwindLimit2) {
    auto bounds = buildPipelineAndExtractBounds(
        {skip(8), unwind(/*includeNullIfEmptyOrMissing*/ true), limit(25)});
    assertUnknown(bounds.getMinBounds());
    assertDiscreteAndEq(bounds.getMaxBounds(), 33);
}

TEST_F(VisitorDocsNeededBoundsTest, SearchSkipLimitUnwind) {
    auto bounds = buildPipelineAndExtractBounds(
        {search(), skip(8), limit(25), unwind(/*includeNullIfEmptyOrMissing*/ false)});
    assertDiscreteAndEq(bounds.getMinBounds(), 33);
    assertDiscreteAndEq(bounds.getMaxBounds(), 33);
}

TEST_F(VisitorDocsNeededBoundsTest, UnwindSkipLimit1) {
    auto bounds = buildPipelineAndExtractBounds(
        {unwind(/*includeNullIfEmptyOrMissing*/ false), skip(8), limit(25)});
    assertUnknown(bounds.getMinBounds());
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, UnwindSkipLimit2) {
    auto bounds = buildPipelineAndExtractBounds(
        {unwind(/*includeNullIfEmptyOrMissing*/ true), skip(8), limit(25)});
    assertUnknown(bounds.getMinBounds());
    assertDiscreteAndEq(bounds.getMaxBounds(), 33);
}

TEST_F(VisitorDocsNeededBoundsTest, SkipLimitUnwindSkipLimit1) {
    // This is an odd pipeline. Technically, we can infer nothing because of the $unwind. However,
    // for the sake of algorithmic simplicity, we'll assume the pipeline is written well, such that
    // we can base the constraints on the earliest $skip+$limit.
    auto bounds = buildPipelineAndExtractBounds(
        {skip(3), limit(5), unwind(/*includeNullIfEmptyOrMissing*/ false), skip(8), limit(25)});
    assertDiscreteAndEq(bounds.getMinBounds(), 8);
    assertDiscreteAndEq(bounds.getMaxBounds(), 8);
}

TEST_F(VisitorDocsNeededBoundsTest, SkipLimitUnwindSkipLimit2) {
    // Same note as SkipLimitUnwindSkipLimit1.
    auto bounds = buildPipelineAndExtractBounds(
        {skip(50), limit(150), unwind(/*includeNullIfEmptyOrMissing*/ false), skip(3), limit(7)});
    assertDiscreteAndEq(bounds.getMinBounds(), 200);
    assertDiscreteAndEq(bounds.getMaxBounds(), 200);
}

TEST_F(VisitorDocsNeededBoundsTest, UnwindSortSkipLimit) {
    auto bounds = buildPipelineAndExtractBounds(
        {unwind(/*includeNullIfEmptyOrMissing*/ false), sort(), skip(3), limit(7)});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, UnwindSkipLimitSort) {
    auto bounds = buildPipelineAndExtractBounds(
        {unwind(/*includeNullIfEmptyOrMissing*/ false), skip(3), limit(7), sort()});
    assertUnknown(bounds.getMinBounds());
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, UnwindSkipSortLimit) {
    auto bounds = buildPipelineAndExtractBounds(
        {unwind(/*includeNullIfEmptyOrMissing*/ false), skip(3), sort(), limit(7)});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, ProjectSkipLimit) {
    auto bounds = buildPipelineAndExtractBounds({project(), skip(3), limit(7)});
    assertDiscreteAndEq(bounds.getMinBounds(), 10);
    assertDiscreteAndEq(bounds.getMaxBounds(), 10);
}

TEST_F(VisitorDocsNeededBoundsTest, SkipLimitProject) {
    auto bounds = buildPipelineAndExtractBounds({skip(15), limit(35), project()});
    assertDiscreteAndEq(bounds.getMinBounds(), 50);
    assertDiscreteAndEq(bounds.getMaxBounds(), 50);
}

TEST_F(VisitorDocsNeededBoundsTest, ProjectMatchSort) {
    auto bounds = buildPipelineAndExtractBounds({project(), match(), sort()});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, FacetsWithLimits) {
    DocumentSourceContainer pipeline1 = {limit(150), match(), limit(50)};
    DocumentSourceContainer pipeline2 = {limit(100), project()};
    auto bounds = buildPipelineAndExtractBounds({facet({pipeline1, pipeline2})});
    assertDiscreteAndEq(bounds.getMinBounds(), 100);
    assertDiscreteAndEq(bounds.getMaxBounds(), 150);
}

TEST_F(VisitorDocsNeededBoundsTest, FacetsWithUnknownAndLimit) {
    DocumentSourceContainer pipeline1 = {limit(75), match()};
    DocumentSourceContainer pipeline2 = {project()};
    auto bounds = buildPipelineAndExtractBounds({facet({pipeline1, pipeline2})});
    assertDiscreteAndEq(bounds.getMinBounds(), 75);
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SearchFacet) {
    DocumentSourceContainer pipeline1 = {project()};
    DocumentSourceContainer pipeline2 = {match()};
    auto bounds = buildPipelineAndExtractBounds({search(), facet({pipeline1, pipeline2})});
    assertUnknown(bounds.getMinBounds());
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SearchFacetLimit) {
    DocumentSourceContainer pipeline1 = {project()};
    DocumentSourceContainer pipeline2 = {match()};
    auto bounds =
        buildPipelineAndExtractBounds({search(), facet({pipeline1, pipeline2}), limit(50)});
    assertDiscreteAndEq(bounds.getMinBounds(), 50);
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SearchFacetSort) {
    DocumentSourceContainer pipeline1 = {match(), project()};
    DocumentSourceContainer pipeline2 = {match(), project()};
    auto bounds = buildPipelineAndExtractBounds({search(), facet({pipeline1, pipeline2}), sort()});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, NeedAllFacetLimit) {
    // Having one $facet pipeline with a $sort will require the entire pipeline needing all
    // documents.
    DocumentSourceContainer pipeline1 = {match(), sort()};
    DocumentSourceContainer pipeline2 = {match(), project()};
    auto bounds = buildPipelineAndExtractBounds({facet({pipeline1, pipeline2}), limit(30)});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, LimitProjectNeedAllFacet) {
    // The $limit value should hold when putting a $facet pipeline that yields NeedAll after a
    // $limit stage.
    DocumentSourceContainer pipeline1 = {match(), sort()};
    DocumentSourceContainer pipeline2 = {match(), project()};
    auto bounds =
        buildPipelineAndExtractBounds({limit(70), project(), facet({pipeline1, pipeline2})});
    assertDiscreteAndEq(bounds.getMinBounds(), 70);
    assertDiscreteAndEq(bounds.getMaxBounds(), 70);
}

TEST_F(VisitorDocsNeededBoundsTest, MatchSortFacet) {
    DocumentSourceContainer pipeline1 = {match(), limit(50)};
    DocumentSourceContainer pipeline2 = {match(), project()};
    auto bounds = buildPipelineAndExtractBounds({match(), sort(), facet({pipeline1, pipeline2})});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, ProjectGroup) {
    auto bounds = buildPipelineAndExtractBounds({project(), group()});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, ProjectSample) {
    auto bounds = buildPipelineAndExtractBounds({project(), sample()});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, ProjectBucketAuto) {
    auto bounds = buildPipelineAndExtractBounds({project(), bucketAuto()});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, ProjectLookup) {
    auto bounds = buildPipelineAndExtractBounds({project(), lookup()});
    assertUnknown(bounds.getMinBounds());
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, LimitLookup) {
    auto bounds = buildPipelineAndExtractBounds({limit(25), lookup()});
    assertDiscreteAndEq(bounds.getMinBounds(), 25);
    assertDiscreteAndEq(bounds.getMaxBounds(), 25);
}

TEST_F(VisitorDocsNeededBoundsTest, LookupWithUnwindLimit1) {
    auto bounds = buildPipelineAndExtractBounds(
        {lookupWithUnwind(/*includeNullIfEmptyOrMissing*/ true), limit(25)});
    assertUnknown(bounds.getMinBounds());
    assertDiscreteAndEq(bounds.getMaxBounds(), 25);
}

TEST_F(VisitorDocsNeededBoundsTest, LookupWithUnwindLimit2) {
    auto bounds = buildPipelineAndExtractBounds(
        {lookupWithUnwind(/*includeNullIfEmptyOrMissing*/ false), limit(25)});
    assertUnknown(bounds.getMinBounds());
    assertUnknown(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SearchSetVariableFromSubPipelineLimit) {
    auto bounds =
        buildPipelineAndExtractBounds({search(), setVariableFromSubPipeline(), limit(15)});
    assertDiscreteAndEq(bounds.getMinBounds(), 15);
    assertDiscreteAndEq(bounds.getMaxBounds(), 15);
}

TEST_F(VisitorDocsNeededBoundsTest, SearchSetVariableFromSubPipelineGroup) {
    auto bounds = buildPipelineAndExtractBounds({search(), setVariableFromSubPipeline(), group()});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, SearchBucketAutoSetVariableFromSubPipelineLookup) {
    auto bounds =
        buildPipelineAndExtractBounds({search(), bucketAuto(), setVariableFromSubPipeline()});
    assertNeedAll(bounds.getMinBounds());
    assertNeedAll(bounds.getMaxBounds());
}

TEST_F(VisitorDocsNeededBoundsTest, InternalSearchIdLookup) {
    // In the context of using these bounds for mongot batchSize tuning, $_internalSearchIdLookUp
    // should never be encountered since this algorithm is run prior to desugaring $search.
    // For that reason, this stage does not have an implemented visitor and should fall into the
    // "unknown" case.
    auto bounds = buildPipelineAndExtractBounds({internalSearchIdLookup()});
    assertUnknown(bounds.getMinBounds());
    assertUnknown(bounds.getMaxBounds());
}
}  // namespace mongo
