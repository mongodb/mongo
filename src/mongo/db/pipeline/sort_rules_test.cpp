/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/pipeline/optimization/sort_rules.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/pipeline_split_state.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/unittest/unittest.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using rule_based_rewrites::pipeline::sortIsRedundantGivenPrecedingStages;
namespace rbr = rule_based_rewrites::pipeline;

// Simulates a stage that provides a fixed sort pattern but explicitly does NOT set sort key
// metadata. Used to test that the rule refuses to erase a $sort whose needsMerge flag requires
// sort key metadata when the preceding stage cannot provide it.
class DocumentSourceSortPatternNoMetaMock final : public DocumentSourceMock {
public:
    static boost::intrusive_ptr<DocumentSourceSortPatternNoMetaMock> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, BSONObj sortPattern) {
        return new DocumentSourceSortPatternNoMetaMock(
            std::deque<GetNextResult>{}, expCtx, std::move(sortPattern));
    }
    bool providesSortKeyMetadata() const override {
        return false;
    }

private:
    DocumentSourceSortPatternNoMetaMock(std::deque<GetNextResult> results,
                                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        BSONObj sortPatternBson)
        : DocumentSourceMock(
              std::move(results), expCtx, SortPattern(std::move(sortPatternBson), expCtx)) {}
};

// Like DocumentSourceSortPatternNoMetaMock but providesSortKeyMetadata() returns true. Models an
// extension stage (e.g. $testVectorSearch) that both establishes a sort order and unconditionally
// writes $sortKey metadata on every output document.
class DocumentSourceSortPatternWithMetaMock final : public DocumentSourceMock {
public:
    static boost::intrusive_ptr<DocumentSourceSortPatternWithMetaMock> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, BSONObj sortPattern) {
        return new DocumentSourceSortPatternWithMetaMock(
            std::deque<GetNextResult>{}, expCtx, std::move(sortPattern));
    }
    bool providesSortKeyMetadata() const override {
        return true;
    }

private:
    DocumentSourceSortPatternWithMetaMock(std::deque<GetNextResult> results,
                                          const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          BSONObj sortPatternBson)
        : DocumentSourceMock(
              std::move(results), expCtx, SortPattern(std::move(sortPatternBson), expCtx)) {}
};

// Simulates a stage whose order-preservation depends on the pipeline split state: order-preserving
// in kUnsplit (single-node pass-through) but not in kSplitForMerge (merge side, data reshuffled).
// Poses as $_internalSearchIdLookup so sortKeyFieldsPreservedBy treats it as an audited
// pass-through; the purpose of this mock is to test split-state-dependent
// preservesOrderAndMetadata behavior, not stage identity.
class DocumentSourceSplitStateMock final : public DocumentSourceMock {
public:
    static boost::intrusive_ptr<DocumentSourceSplitStateMock> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceSplitStateMock(std::deque<GetNextResult>{}, expCtx);
    }
    using DocumentSourceMock::DocumentSourceMock;
    Id getId() const override {
        return DocumentSourceInternalSearchIdLookUp::id;
    }
    StageConstraints constraints(PipelineSplitState pipeState) const override {
        StageConstraints c = mockConstraints;
        c.preservesOrderAndMetadata = (pipeState != PipelineSplitState::kSplitForMerge);
        return c;
    }
};

class SortRuleTest : public AggregationContextFixture {
protected:
    std::unique_ptr<Pipeline> makePipeline(std::vector<BSONObj> stages) {
        return pipeline_factory::makePipeline(
            stages, getExpCtx(), pipeline_factory::kOptionsMinimal);
    }

    // Returns true if REDUNDANT_SORT_REMOVAL's precondition fires at stage index `idx`.
    bool isRedundantAt(Pipeline& pipeline, size_t idx) {
        auto& sources = pipeline.getSources();
        rbr::PipelineRewriteContext ctx(*getExpCtx(), sources, std::next(sources.begin(), idx));
        return sortIsRedundantGivenPrecedingStages(ctx);
    }

    // Applies REDUNDANT_SORT_REMOVAL's transform at stage index `idx`.
    void eraseAt(Pipeline& pipeline, size_t idx) {
        auto& sources = pipeline.getSources();
        rbr::PipelineRewriteContext ctx(*getExpCtx(), sources, std::next(sources.begin(), idx));
        rbr::Transforms::eraseCurrent(ctx);
    }
};

// [$sort{a}, $limit{5}, $sort{a,b}]: preceding sort {a} does NOT subsume {a,b}, so not erased.
TEST_F(SortRuleTest, DoesNotEraseWhenPrecedingSortIsInsufficientPrefix) {
    auto pipeline = makePipeline(
        {fromjson("{$sort: {a: 1}}"), fromjson("{$limit: 5}"), fromjson("{$sort: {a: 1, b: 1}}")});
    ASSERT_FALSE(isRedundantAt(*pipeline, 2));
}

// [$sort{a:1}, $sort{a:-1}]: isExtensionOf enforces direction equality and {a:1} is NOT an
// extension of {a:-1} even though they share the same field, so the rule does not fire.
TEST_F(SortRuleTest, DoesNotEraseWhenSortDirectionsDiffer) {
    auto pipeline = makePipeline({fromjson("{$sort: {a: 1}}"), fromjson("{$sort: {a: -1}}")});
    ASSERT_FALSE(isRedundantAt(*pipeline, 1));
}

// A $sort that is the first stage has no preceding sort so the rule does not fire.
TEST_F(SortRuleTest, DoesNotEraseFirstStageSort) {
    auto pipeline = makePipeline({fromjson("{$sort: {a: 1}}")});
    ASSERT_FALSE(isRedundantAt(*pipeline, 0));
}

// [$match{a:1}, $sort{a}]: $match has no sort pattern so the rule does not fire.
TEST_F(SortRuleTest, DoesNotEraseWhenPrecedingStageHasNoSortPattern) {
    auto pipeline = makePipeline({fromjson("{$match: {a: 1}}"), fromjson("{$sort: {a: 1}}")});
    ASSERT_FALSE(isRedundantAt(*pipeline, 1));
}

// [$sort{a,b,limit:5}, $sort{a, outputSortKeyMetadata:true}]: the second sort sets sort key
// metadata but the preceding sort does not so the rule does not fire.
TEST_F(SortRuleTest, DoesNotEraseWhenCurrentSortSetsKeyMetadata) {
    auto pipeline =
        makePipeline({fromjson("{$sort: {a: 1, b: 1, $_internalLimit: 5}}"),
                      fromjson("{$sort: {a: 1, $_internalOutputSortKeyMetadata: true}}")});
    ASSERT_FALSE(isRedundantAt(*pipeline, 1));
}

// [$mock{sortPattern:{a:1}, setsSortKeyMetadata:false}, $sort{a}] with needsMerge=true:
// $sort sets sort key metadata (needsMerge path) but the preceding stage does not — rule must NOT
// fire even though the sort pattern covers {a:1}.
TEST_F(SortRuleTest, DoesNotEraseWhenPrecedingStageLacksSortKeyMetadata) {
    auto expCtx = getExpCtx();
    expCtx->setNeedsMerge(true);
    DocumentSourceContainer stages;
    stages.push_back(DocumentSourceSortPatternNoMetaMock::create(expCtx, fromjson("{a: 1}")));
    stages.push_back(
        DocumentSourceSort::createFromBson(fromjson("{$sort: {a: 1}}").firstElement(), expCtx));

    auto sortItr = std::prev(stages.end());
    rbr::PipelineRewriteContext ctx(*getExpCtx(), stages, sortItr);
    ASSERT_FALSE(sortIsRedundantGivenPrecedingStages(ctx));
    ASSERT_EQ(stages.size(), 2U);
}

// [$mock{sortPattern:{a:1}, setsSortKeyMetadata:true}, $sort{a}] with needsMerge=true:
// models an extension stage like $testVectorSearch that establishes a sort order AND writes
// $sortKey. Both requirements are satisfied so the rule fires.
TEST_F(SortRuleTest, ErasesRedundantSortWhenPrecedingExtensionStageSortKeyMetadata) {
    auto expCtx = getExpCtx();
    expCtx->setNeedsMerge(true);
    DocumentSourceContainer stages;
    stages.push_back(DocumentSourceSortPatternWithMetaMock::create(expCtx, fromjson("{a: 1}")));
    stages.push_back(
        DocumentSourceSort::createFromBson(fromjson("{$sort: {a: 1}}").firstElement(), expCtx));

    auto sortItr = std::prev(stages.end());
    rbr::PipelineRewriteContext ctx(*getExpCtx(), stages, sortItr);
    ASSERT_TRUE(sortIsRedundantGivenPrecedingStages(ctx));
    rbr::Transforms::eraseCurrent(ctx);
    ASSERT_EQ(stages.size(), 1U);
}

// [$sort{a}, $group{_id:"$b"}, $sort{a}]: $group does NOT preserveOrderAndMetadata so the walk
// stops.
TEST_F(SortRuleTest, DoesNotEraseWhenSortDestroyingStageIntervenes) {
    auto pipeline = makePipeline({fromjson("{$sort: {a: 1}}"),
                                  fromjson("{$group: {_id: \"$b\"}}"),
                                  fromjson("{$sort: {a: 1}}")});
    ASSERT_FALSE(isRedundantAt(*pipeline, 2));
}

// [$vectorSearch, $sort{vectorSearchScore}]: vectorSearch's sort pattern covers the sort.
TEST_F(SortRuleTest, ErasesRedundantSortAfterVectorSearch) {
    auto pipeline = makePipeline(
        {fromjson(
             "{$vectorSearch: {queryVector: [1.0,0.0], path: \"v\", numCandidates: 10, limit: 5}}"),
         fromjson("{$sort: {score: {$meta: \"vectorSearchScore\"}}}")});
    ASSERT_TRUE(isRedundantAt(*pipeline, 1));
    eraseAt(*pipeline, 1);
    auto result = pipeline->serializeToBson();
    ASSERT_EQ(result.size(), 1U);
    ASSERT(result[0].hasField("$vectorSearch")) << "Expected $vectorSearch, got: " << result[0];
}

// [$vectorSearch, $sort{vectorSearchScore}] with needsMerge=true: $vectorSearch unconditionally
// sets sort key metadata; the sort also sets it via needsMerge. Both requirements are satisfied
// so the rule fires. This covers the stage->providesSortKeyMetadata() branch in the precondition,
// exercised via $vectorSearch here and via providedMetadataFields on extension stages.
TEST_F(SortRuleTest, ErasesRedundantSortAfterVectorSearchWhenNeedsMerge) {
    getExpCtx()->setNeedsMerge(true);
    auto pipeline = makePipeline(
        {fromjson(
             "{$vectorSearch: {queryVector: [1.0,0.0], path: \"v\", numCandidates: 10, limit: 5}}"),
         fromjson("{$sort: {score: {$meta: \"vectorSearchScore\"}}}")});
    ASSERT_TRUE(isRedundantAt(*pipeline, 1));
    eraseAt(*pipeline, 1);
    auto result = pipeline->serializeToBson();
    ASSERT_EQ(result.size(), 1U);
    ASSERT(result[0].hasField("$vectorSearch")) << "Expected $vectorSearch, got: " << result[0];
}

// [$vectorSearch, $_internalSearchIdLookup, $sort{vectorSearchScore}]: $_internalSearchIdLookup
// preservesOrderAndMetadata=true, so the backward walk continues past it to find $vectorSearch's
// sort pattern.
TEST_F(SortRuleTest, ErasesRedundantSortWhenIdLookupIntervenesBetweenVectorSearchAndSort) {
    auto pipeline = makePipeline(
        {fromjson(
             "{$vectorSearch: {queryVector: [1.0,0.0], path: \"v\", numCandidates: 10, limit: 5}}"),
         fromjson("{$_internalSearchIdLookup: {limit: 67}}"),
         fromjson("{$sort: {score: {$meta: \"vectorSearchScore\"}}}")});
    ASSERT_TRUE(isRedundantAt(*pipeline, 2));
    eraseAt(*pipeline, 2);
    auto result = pipeline->serializeToBson();
    ASSERT_EQ(result.size(), 2U);
    ASSERT(result[0].hasField("$vectorSearch")) << "Expected $vectorSearch, got: " << result[0];
    ASSERT(result[1].hasField("$_internalSearchIdLookup"))
        << "Expected $_internalSearchIdLookup, got: " << result[1];
}

// [$vectorSearch, $replaceRoot{newRoot:"$x"}, $sort{vectorSearchScore}]: newRoot is a non-object
// field path expression, but the sort is entirely metadata-based (no named fieldPath), so
// sortKeyFieldsPreservedBy returns true and the backward scan continues to find $vectorSearch.
TEST_F(SortRuleTest, ErasesRedundantSortWhenReplaceRootIntervenesBetweenVectorSearchAndSort) {
    auto pipeline = makePipeline(
        {fromjson(
             "{$vectorSearch: {queryVector: [1.0,0.0], path: \"v\", numCandidates: 10, limit: 5}}"),
         fromjson("{$replaceRoot: {newRoot: \"$x\"}}"),
         fromjson("{$sort: {score: {$meta: \"vectorSearchScore\"}}}")});
    ASSERT_TRUE(isRedundantAt(*pipeline, 2));
    eraseAt(*pipeline, 2);
    auto result = pipeline->serializeToBson();
    ASSERT_EQ(result.size(), 2U);
    ASSERT(result[0].hasField("$vectorSearch")) << result[0];
    ASSERT(result[1].hasField("$replaceRoot")) << result[1];
}

// [$vectorSearch, $sort{a:1}]: sort pattern {a:1} doesn't match vectorSearch's output.
TEST_F(SortRuleTest, DoesNotEraseNonRedundantSortAfterVectorSearch) {
    auto pipeline = makePipeline(
        {fromjson(
             "{$vectorSearch: {queryVector: [1.0,0.0], path: \"v\", numCandidates: 10, limit: 5}}"),
         fromjson("{$sort: {a: 1}}")});
    ASSERT_FALSE(isRedundantAt(*pipeline, 1));
}

// On a non-merge pipeline, an intervening stage that preserves order is transparent to the backward
// scan, so the rule finds the covering sort behind it and removes the redundant sort.
TEST_F(SortRuleTest, ErasesRedundantSortWhenSplitAwareMockPreservesOrderOnNonMergeSide) {
    auto expCtx = getExpCtx();  // needsMerge=false by default
    DocumentSourceContainer stages;
    stages.push_back(DocumentSourceSort::createFromBson(
        fromjson("{$sort: {a: 1, b: 1}}").firstElement(), expCtx));
    stages.push_back(DocumentSourceSplitStateMock::create(expCtx));
    stages.push_back(
        DocumentSourceSort::createFromBson(fromjson("{$sort: {a: 1}}").firstElement(), expCtx));

    auto sortItr = std::prev(stages.end());
    rbr::PipelineRewriteContext ctx(*getExpCtx(), stages, sortItr);
    ASSERT_TRUE(sortIsRedundantGivenPrecedingStages(ctx));
    rbr::Transforms::eraseCurrent(ctx);
    ASSERT_EQ(stages.size(), 2U);
}

// On the merge side of a sharded pipeline, the same intervening stage no longer preserves order.
// Without passing the correct split state to constraints(), the stage would appear order-preserving
// and the sort would be incorrectly removed.
TEST_F(SortRuleTest, KeepsSortWhenSplitAwareMockBreaksOrderOnMergeSide) {
    auto expCtx = getExpCtx();
    expCtx->setNeedsMerge(true);
    DocumentSourceContainer stages;
    stages.push_back(DocumentSourceSort::createFromBson(
        fromjson("{$sort: {a: 1, b: 1}}").firstElement(), expCtx));
    stages.push_back(DocumentSourceSplitStateMock::create(expCtx));
    stages.push_back(
        DocumentSourceSort::createFromBson(fromjson("{$sort: {a: 1}}").firstElement(), expCtx));

    auto sortItr = std::prev(stages.end());
    rbr::PipelineRewriteContext ctx(*getExpCtx(), stages, sortItr);
    ASSERT_FALSE(sortIsRedundantGivenPrecedingStages(ctx));
    ASSERT_EQ(stages.size(), 3U);
}

// [$sort{a,b}, $replaceRoot{a:"$b",b:"$a"}, $sort{a,b}]: $replaceRoot swaps the values of sort key
// fields 'a' and 'b'. Even though it preservesOrderAndMetadata, the field values change, so the
// upstream sort guarantee does not carry through.
TEST_F(SortRuleTest, DoesNotEraseWhenReplaceRootSwapsSortKeyFields) {
    auto pipeline = makePipeline({fromjson("{$sort: {a: 1, b: 1}}"),
                                  fromjson("{$replaceRoot: {newRoot: {a: \"$b\", b: \"$a\"}}}"),
                                  fromjson("{$sort: {a: 1, b: 1}}")});
    ASSERT_FALSE(isRedundantAt(*pipeline, 2));
}

// [$sort{a}, $addFields{b:1}, $sort{a}]: $addFields does not set preservesOrderAndMetadata, so the
// backward walk stops even though field 'a' is untouched. Guards against SERVER-127594 silently
// enabling the optimization for $addFields without adding field-mapping inspection.
TEST_F(SortRuleTest, DoesNotEraseWhenAddFieldsIntervenesEvenIfSortKeyUntouched) {
    auto pipeline = makePipeline({fromjson("{$sort: {a: 1}}"),
                                  fromjson("{$addFields: {b: 1}}"),
                                  fromjson("{$sort: {a: 1}}")});
    ASSERT_FALSE(isRedundantAt(*pipeline, 2));
}

// sortKeyFieldsPreservedBy path coverage tests --------------------------------------------------

// $replaceRoot with a non-object newRoot (field path expression) and a named sort key.
// sortKeyFieldsPreservedBy can't verify named field mappings and returns false.
TEST_F(SortRuleTest, DoesNotEraseWhenReplaceRootHasFieldPathNewRootAndNamedSortKey) {
    auto pipeline = makePipeline({fromjson("{$sort: {a: 1, b: 1}}"),
                                  fromjson("{$replaceRoot: {newRoot: \"$subdoc\"}}"),
                                  fromjson("{$sort: {a: 1}}")});
    ASSERT_FALSE(isRedundantAt(*pipeline, 2));
}

// $replaceRoot with an object newRoot that omits a sort key field entirely.
// sortKeyFieldsPreservedBy returns false because 'a' is absent from newRoot.
TEST_F(SortRuleTest, DoesNotEraseWhenReplaceRootDropsSortKeyField) {
    auto pipeline = makePipeline({fromjson("{$sort: {a: 1, b: 1}}"),
                                  fromjson("{$replaceRoot: {newRoot: {b: \"$b\"}}}"),
                                  fromjson("{$sort: {a: 1}}")});
    ASSERT_FALSE(isRedundantAt(*pipeline, 2));
}

// $replaceRoot with an object newRoot that identity-maps all sort key fields.
// sortKeyFieldsPreservedBy returns true, backward scan finds the covering sort, and removes it.
TEST_F(SortRuleTest, ErasesRedundantSortWhenReplaceRootIdentityMapsAllSortKeyFields) {
    auto pipeline = makePipeline({fromjson("{$sort: {a: 1, b: 1}}"),
                                  fromjson("{$replaceRoot: {newRoot: {a: \"$a\", b: \"$b\"}}}"),
                                  fromjson("{$sort: {a: 1}}")});
    ASSERT_TRUE(isRedundantAt(*pipeline, 2));
}

}  // namespace
}  // namespace mongo
