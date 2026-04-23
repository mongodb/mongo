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

#include "mongo/db/pipeline/pipeline_d.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/shard_role_transaction_resources_stasher_for_pipeline.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.coll");

class PipelineDTest : public CatalogTestFixture {
protected:
    void setUp() final {
        CatalogTestFixture::setUp();
        // Register a mock collator factory so collections with mock-locale collations can be
        // created without requiring the full ICU library.
        CollatorFactoryInterface::set(getServiceContext(), std::make_unique<CollatorFactoryMock>());
        expCtx = make_intrusive<ExpressionContextForTest>(operationContext(), kNss);
    }

    void tearDown() final {
        expCtx.reset();
        CatalogTestFixture::tearDown();
    }

    // Creates the collection at kNss with the given options. Each test that needs a collection
    // calls this directly so it can tailor the options (e.g. custom collator).
    void createCollection(CollectionOptions opts = {}) {
        ASSERT_OK(storageInterface()->createCollection(operationContext(), kNss, opts));
    }

    void buildAndAttachPipeline(AggregateCommandRequest aggRequest) {
        DocumentSourceContainer sources;
        for (const auto& s : aggRequest.getPipeline()) {
            for (auto&& ds : DocumentSource::parse(expCtx, s)) {
                sources.push_back(std::move(ds));
            }
        }
        auto pipeline = Pipeline::create(sources, expCtx);

        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), kNss, AcquisitionPrerequisites::OperationType::kRead),
            MODE_IS);
        auto colls = MultipleCollectionAccessor(
            std::move(coll), {}, false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */);

        auto transactionResourcesStasher =
            make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
        PipelineD::buildAndAttachInnerQueryExecutorAndBindCatalogInfoToPipeline(
            colls, kNss, &aggRequest, pipeline.get(), transactionResourcesStasher);

        stashTransactionResourcesFromOperationContext(operationContext(),
                                                      transactionResourcesStasher.get());
    }

    void buildAndAttachPipeline(std::vector<BSONObj> stages) {
        buildAndAttachPipeline(AggregateCommandRequest(kNss, stages));
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
};

// An aggregation pipeline starting with {$match: {_id: X}} should have isIdHackQuery set to true
// after prepareExecutor runs. This enables the express/IDHACK fast path for aggregations that are
// equivalent to a point lookup on _id (e.g. db.coll.aggregate([{$match: {_id: X}}])).
TEST_F(PipelineDTest, AggPipelineMatchOnIdSetsIsIdHackQuery) {
    createCollection();
    ASSERT_FALSE(expCtx->isIdHackQuery());
    buildAndAttachPipeline({fromjson("{$match: {_id: 1}}")});
    ASSERT_TRUE(expCtx->isIdHackQuery());
}

// A pipeline that does not match on _id should not set isIdHackQuery.
TEST_F(PipelineDTest, AggPipelineMatchOnNonIdDoesNotSetIsIdHackQuery) {
    createCollection();
    ASSERT_FALSE(expCtx->isIdHackQuery());
    buildAndAttachPipeline({fromjson("{$match: {a: 1}}")});
    ASSERT_FALSE(expCtx->isIdHackQuery());
}

// When the collection has a non-null default collator and the query's effective collator matches
// it, isIdHackQuery must be set to true. This exercises the CollatorInterface::collatorsMatch
// branch in prepareExecutor with a non-null collection collator.
TEST_F(PipelineDTest, AggPipelineMatchOnIdWithMatchingCollatorSetsIsIdHackQuery) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    CollectionOptions opts;
    opts.collation = collator.getSpec().toBSON();
    createCollection(opts);
    // Set the same collator on expCtx so cq->getCollator() matches the collection's default.
    expCtx->setCollator(collator.cloneShared());

    ASSERT_FALSE(expCtx->isIdHackQuery());
    buildAndAttachPipeline({fromjson("{$match: {_id: 1}}")});
    ASSERT_TRUE(expCtx->isIdHackQuery());
}

// A pipeline starting with {$match: {_id: X}} followed by $lookup should still set isIdHackQuery
// to true. The $match is pushed down to the cursor layer as a canonical query on _id, while
// $lookup remains in the aggregation pipeline above the cursor and does not affect the canonical
// query's filter. If a future optimization pushes $lookup into the cursor layer and changes the
// canonical query shape, this test will need revisiting.
TEST_F(PipelineDTest, AggPipelineMatchOnIdFollowedByLookupSetsIsIdHackQuery) {
    createCollection();
    // Register the $lookup 'from' namespace so DocumentSource::parse() can resolve it.
    const auto otherNss = NamespaceString::createNamespaceString_forTest("test.other");
    expCtx->addResolvedNamespace(otherNss, ResolvedNamespace{otherNss, {}});
    ASSERT_FALSE(expCtx->isIdHackQuery());
    buildAndAttachPipeline(
        {fromjson("{$match: {_id: 1}}"),
         fromjson("{$lookup: {from: 'other', localField: 'x', foreignField: 'y', as: 'z'}}")});
    ASSERT_TRUE(expCtx->isIdHackQuery());
}

// A $lookup whose *subpipeline* starts with {$match: {_id: X}} does NOT set isIdHackQuery on the
// outer ExpressionContext. The outer pipeline has no _id filter of its own; the subpipeline's
// eligibility applies only to the inner CQ built when the $lookup is executed, not to the outer
// cursor. This is the complement of AggPipelineMatchOnIdFollowedByLookupSetsIsIdHackQuery.
TEST_F(PipelineDTest, AggPipelineLookupSubpipelineMatchOnIdDoesNotSetIsIdHackQuery) {
    createCollection();
    const auto otherNss = NamespaceString::createNamespaceString_forTest("test.other");
    expCtx->addResolvedNamespace(otherNss, ResolvedNamespace{otherNss, {}});
    ASSERT_FALSE(expCtx->isIdHackQuery());
    buildAndAttachPipeline(
        {fromjson("{$lookup: {from: 'other', pipeline: [{$match: {_id: 1}}], as: 'x'}}")});
    ASSERT_FALSE(expCtx->isIdHackQuery());
}

// White-box test of the collatorsMatch() guard in prepareExecutor(): when the CQ's collator does
// not match the collection's default collator, isIdHackQuery must remain false. In a real
// aggregation without an explicit collation, expCtx would inherit the collection's collator and
// the collators would match; here we leave expCtx with a null collator to exercise the guard.
TEST_F(PipelineDTest, AggPipelineMatchOnIdWithMismatchedCollatorDoesNotSetIsIdHackQuery) {
    CollatorInterfaceMock collectionCollator(CollatorInterfaceMock::MockType::kReverseString);
    CollectionOptions opts;
    opts.collation = collectionCollator.getSpec().toBSON();
    createCollection(opts);
    // expCtx intentionally keeps a null collator (mismatch) to exercise the guard.

    ASSERT_FALSE(expCtx->isIdHackQuery());
    buildAndAttachPipeline({fromjson("{$match: {_id: 1}}")});
    ASSERT_FALSE(expCtx->isIdHackQuery());
}

// A single-element $in on _id normalizes to an $eq during MatchExpression parsing. The code in
// prepareExecutor() passes the parsed MatchExpression to isIdHackEligibleQueryWithoutCollator(),
// so this normalized form must be recognized and must set isIdHackQuery to true.
TEST_F(PipelineDTest, AggPipelineMatchOnIdWithSingleElemInSetsIsIdHackQuery) {
    createCollection();
    ASSERT_FALSE(expCtx->isIdHackQuery());
    buildAndAttachPipeline({fromjson("{$match: {_id: {$in: [1]}}}")});
    ASSERT_TRUE(expCtx->isIdHackQuery());
}

// A pipeline starting with {$match: {_id: {$in: [X]}}} on an aggregate command with an explicit
// hint must NOT set isIdHackQuery to true. A hint disqualifies IDHACK, and the late-upgrade path
// in prepareExecutor() (maybeUpgradeIdHackFlag) must check all preconditions via
// isIdHackEligibleQueryWithoutCollator(), not just the filter shape via isMatchIdHackEligible().
// TODO SERVER-87213: a hint on the _id index itself should not disqualify IDHACK/Express.
TEST_F(PipelineDTest, AggPipelineMatchOnIdInWithHintDoesNotSetIsIdHackQuery) {
    createCollection();
    ASSERT_FALSE(expCtx->isIdHackQuery());
    auto aggRequest = AggregateCommandRequest(kNss, {fromjson("{$match: {_id: {$in: [1]}}}")});
    aggRequest.setHint(fromjson("{_id: 1}"));
    buildAndAttachPipeline(std::move(aggRequest));
    ASSERT_FALSE(expCtx->isIdHackQuery());
}

// A multi-element $in on _id does NOT normalize to a simple $eq and must not set isIdHackQuery.
// This is the boundary between the single-element case (which does normalize and qualifies) and the
// general case (which stays as an IN MatchExpression and is rejected by isMatchIdHackEligible()).
TEST_F(PipelineDTest, AggPipelineMatchOnIdWithMultiElemInDoesNotSetIsIdHackQuery) {
    createCollection();
    ASSERT_FALSE(expCtx->isIdHackQuery());
    buildAndAttachPipeline({fromjson("{$match: {_id: {$in: [1, 2]}}}")});
    ASSERT_FALSE(expCtx->isIdHackQuery());
}

// A compound $match that includes _id alongside other fields is not a pure _id point query.
// Neither isSimpleIdQuery() (raw BSON has multiple fields) nor isMatchIdHackEligible() (ME is an
// AND node, not an EQ node) qualifies it, so isIdHackQuery must remain false.
TEST_F(PipelineDTest, AggPipelineCompoundMatchIncludingIdDoesNotSetIsIdHackQuery) {
    createCollection();
    ASSERT_FALSE(expCtx->isIdHackQuery());
    buildAndAttachPipeline({fromjson("{$match: {_id: 1, other: 2}}")});
    ASSERT_FALSE(expCtx->isIdHackQuery());
}

// A pipeline with no $match stage produces a canonical query with a trivially true filter.
// Neither isSimpleIdQuery() nor isMatchIdHackEligible() qualifies an always-true filter, so
// isIdHackQuery must remain false.
TEST_F(PipelineDTest, AggPipelineWithNoMatchDoesNotSetIsIdHackQuery) {
    createCollection();
    ASSERT_FALSE(expCtx->isIdHackQuery());
    buildAndAttachPipeline({fromjson("{$project: {a: 1}}")});
    ASSERT_FALSE(expCtx->isIdHackQuery());
}

// Regression test for SERVER-123100: when a sub-query (e.g. generated internally by
// $graphLookup/$lookup/$dbref at runtime) inherits an outer ExpressionContext that has
// isIdHackQuery=true, but the sub-query's actual match expression is not a simple _id
// equality, isExpressEligible() must return Ineligible. Without the fix, it trusted the
// stale flag and returned IdPointQueryEligible, causing makeExpressExecutorForFindById()
// to tassert when it tried to cast the match expression to ComparisonMatchExpressionBase.
TEST_F(PipelineDTest, ExpressIneligibleWhenIsIdHackQueryFlagStaleForInQuery) {
    createCollection();

    // Build a CQ for {_id: {$in: [1, 2]}} — a multi-element $in that stays as an IN
    // MatchExpression (NOT a ComparisonMatchExpressionBase) after parsing.
    auto findCommand = std::make_unique<FindCommandRequest>(kNss);
    findCommand->setFilter(fromjson("{_id: {$in: [1, 2]}}"));
    auto testExpCtx =
        ExpressionContextBuilder{}.fromRequest(operationContext(), *findCommand).build();

    // Sanity-check: the flag is correctly false for a multi-element $in.
    ASSERT_FALSE(testExpCtx->isIdHackQuery());

    // Force the flag to true to simulate a sub-query that has inherited the outer pipeline's
    // ExpressionContext (which had isIdHackQuery=true because the outer query was {_id: X}).
    testExpCtx->setIsIdHackQuery(true);

    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = testExpCtx,
        .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)}});

    auto coll = acquireCollection(
        operationContext(),
        CollectionAcquisitionRequest::fromOpCtx(
            operationContext(), kNss, AcquisitionPrerequisites::OperationType::kRead),
        MODE_IS);

    // Without the fix: returns IdPointQueryEligible (trusting the stale flag), and the
    // subsequent call to makeExpressExecutorForFindById() would tassert because the IN match
    // expression cannot be cast to ComparisonMatchExpressionBase. With the fix:
    // isIdHackEligibleQueryWithoutCollator() rejects the IN expression and returns Ineligible.
    ASSERT_EQ(isExpressEligible(operationContext(), coll.getCollectionPtr(), *cq),
              ExpressEligibility::Ineligible);
}

}  // namespace
}  // namespace mongo
