// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression_context.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <utility>


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using ExpressionContextTest = ServiceContextTest;

TEST_F(ExpressionContextTest, ExpressionContextSummonsMissingTimeValues) {
    auto opCtx = makeOperationContext();
    auto t1 = VectorClockMutable::get(opCtx->getServiceContext())->tickClusterTime(1);
    t1.addTicks(100);
    VectorClockMutable::get(opCtx->getServiceContext())->tickClusterTimeTo(t1);
    {
        const auto expCtx =
            ExpressionContextBuilder{}
                .opCtx(opCtx.get())
                .ns(NamespaceString::createNamespaceString_forTest("test"sv, "namespace"sv))
                .runtimeConstants(LegacyRuntimeConstants{Date_t::now(), {}})
                .build();
        // LegacyRuntimeConstants is passed to the constructor of ExpressionContext and should make
        // $$NOW and $$CLUSTER_TIME available to be referenced.
        ASSERT_DOES_NOT_THROW(static_cast<void>(expCtx->variables.getValue(Variables::kNowId)));
        ASSERT_DOES_NOT_THROW(
            static_cast<void>(expCtx->variables.getValue(Variables::kClusterTimeId)));
    }
    {
        auto expCtx =
            ExpressionContextBuilder{}
                .opCtx(opCtx.get())
                .ns(NamespaceString::createNamespaceString_forTest("test"sv, "namespace"sv))
                .runtimeConstants(LegacyRuntimeConstants{{}, Timestamp(1, 0)})
                .build();
        // LegacyRuntimeConstants is passed to the constructor of ExpressionContext and should
        // make
        // $$NOW and $$CLUSTER_TIME available to be referenced.
        ASSERT_DOES_NOT_THROW(static_cast<void>(expCtx->variables.getValue(Variables::kNowId)));
        ASSERT_DOES_NOT_THROW(
            static_cast<void>(expCtx->variables.getValue(Variables::kClusterTimeId)));
    }
}

TEST_F(ExpressionContextTest, ParametersCanContainExpressionsWhichAreFolded) {
    auto opCtx = makeOperationContext();
    const auto expCtx =
        ExpressionContextBuilder{}
            .opCtx(opCtx.get())
            .ns(NamespaceString::createNamespaceString_forTest("test"sv, "namespace"sv))
            .letParameters(BSON("atan2" << BSON("$atan2" << BSON_ARRAY(0 << 1))))
            .build();
    ASSERT_EQUALS(
        0.0,
        expCtx->variables.getValue(expCtx->variablesParseState.getVariable("atan2")).getDouble());
}

TEST_F(ExpressionContextTest, ParametersCanReferToAlreadyDefinedParameters) {
    auto opCtx = makeOperationContext();
    const auto expCtx =
        mongo::ExpressionContextBuilder{}
            .opCtx(opCtx.get())
            .ns(mongo::NamespaceString::createNamespaceString_forTest("test"sv, "namespace"sv))
            .letParameters(BSON("a" << 12 << "b"
                                    << "$$a"
                                    << "c"
                                    << "$$b"))
            .build();
    ASSERT_EQUALS(
        12.0, expCtx->variables.getValue(expCtx->variablesParseState.getVariable("c")).getDouble());
}

TEST_F(ExpressionContextTest, ParametersCanOverwriteInLeftToRightOrder) {
    auto opCtx = makeOperationContext();
    const auto expCtx =
        mongo::ExpressionContextBuilder{}
            .opCtx(opCtx.get())
            .ns(mongo::NamespaceString::createNamespaceString_forTest("test_sd", "namespace_sd"))
            .mayDbProfile(false)
            .letParameters(BSON("x" << 12 << "b" << 10 << "x" << 20))
            .build();
    ASSERT_EQUALS(
        20, expCtx->variables.getValue(expCtx->variablesParseState.getVariable("x")).getDouble());
}

TEST_F(ExpressionContextTest, ParametersCauseGracefulFailuresIfNonConstant) {
    auto opCtx = makeOperationContext();
    ASSERT_THROWS_CODE(
        static_cast<void>(
            mongo::ExpressionContextBuilder{}
                .opCtx(opCtx.get())
                .ns(mongo::NamespaceString::createNamespaceString_forTest("test"sv, "namespace"sv))
                .letParameters(BSON("a" << "$b"))
                .build()),
        mongo::DBException,
        4890500);
}

TEST_F(ExpressionContextTest, ParametersCauseGracefulFailuresIfUppercase) {
    auto opCtx = makeOperationContext();
    ASSERT_THROWS_CODE(
        static_cast<void>(
            mongo::ExpressionContextBuilder{}
                .opCtx(opCtx.get())
                .ns(mongo::NamespaceString::createNamespaceString_forTest("test"sv, "namespace"sv))
                .letParameters(BSON("A" << 12))
                .build()),
        mongo::DBException,
        ErrorCodes::FailedToParse);
}

TEST_F(ExpressionContextTest, DontInitializeUnreferencedVariables) {
    auto opCtx = makeOperationContext();
    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << BSON("a" << 1)));
    AggregateCommandRequest acr({} /*nss*/, pipeline);
    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), acr).build();
    pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal);
    expCtx->initializeReferencedSystemVariables();
    ASSERT_FALSE(expCtx->variables.hasValue(Variables::kNowId));
    ASSERT_FALSE(expCtx->variables.hasValue(Variables::kClusterTimeId));
    ASSERT_FALSE(expCtx->variables.hasValue(Variables::kUserRolesId));
}

TEST_F(ExpressionContextTest, ErrorsIfClusterTimeUsedInStandalone) {
    auto opCtx = makeOperationContext();
    repl::ReplicationCoordinator::set(opCtx->getServiceContext(),
                                      std::make_unique<repl::ReplicationCoordinatorMock>(
                                          opCtx->getServiceContext(), repl::ReplSettings()));
    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$project" << BSON("a" << "$$CLUSTER_TIME")));
    AggregateCommandRequest acr({} /*nss*/, pipeline);
    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), acr).build();
    ASSERT_THROWS_CODE(
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal),
        AssertionException,
        10071200);
}

TEST_F(ExpressionContextTest, CanBuildWithoutView) {
    auto opCtx = makeOperationContext();

    auto expCtxWithoutView =
        mongo::ExpressionContextBuilder{}
            .opCtx(opCtx.get())
            .ns(NamespaceString::createNamespaceString_forTest("test"sv, "namespace"sv))
            .build();

    ASSERT_FALSE(expCtxWithoutView->getView().has_value());
}

TEST_F(ExpressionContextTest, CanBuildWithView) {
    auto opCtx = makeOperationContext();

    auto viewNss = NamespaceString::createNamespaceString_forTest("test"sv, "view"sv);
    auto collNss = NamespaceString::createNamespaceString_forTest("test"sv, "coll"sv);
    std::vector<BSONObj> viewPipeline = {BSON("$project" << BSON("_id" << 0))};

    auto view =
        boost::make_optional(ResolvedNamespace::makeForView(viewNss, collNss, viewPipeline));
    auto expCtxWithView = mongo::ExpressionContextBuilder{}
                              .opCtx(opCtx.get())
                              .ns(collNss)
                              .view(std::move(view))
                              .build();

    // expCtx namespace isn't affected by the view namespace.
    ASSERT_EQUALS(expCtxWithView->getNamespaceString(),
                  NamespaceString::createNamespaceString_forTest("test"sv, "coll"sv));

    ASSERT_TRUE(expCtxWithView->getView().has_value());
    ASSERT_EQUALS(expCtxWithView->getView()->getNamespace(), viewNss);

    auto expCtxViewPipe = expCtxWithView->getView()->getViewPipeline();
    const auto& expCtxViewPipeStages = expCtxViewPipe.getStages();
    ASSERT_EQUALS(expCtxViewPipeStages.size(), viewPipeline.size());
    ASSERT(expCtxViewPipeStages[0] != nullptr);
    ASSERT_BSONOBJ_EQ(expCtxViewPipeStages[0]->getOriginalBson().wrap(), viewPipeline[0]);
}

TEST_F(ExpressionContextTest, CopyWithDoesNotInitializeViewByDefault) {
    auto opCtx = makeOperationContext();

    auto viewNss = NamespaceString::createNamespaceString_forTest("test"sv, "view"sv);
    auto coll1Nss = NamespaceString::createNamespaceString_forTest("test"sv, "coll1"sv);
    std::vector<BSONObj> viewPipeline = {BSON("$project" << BSON("_id" << 0))};

    auto view =
        boost::make_optional(ResolvedNamespace::makeForView(viewNss, coll1Nss, viewPipeline));
    auto expCtxOriginal = mongo::ExpressionContextBuilder{}
                              .opCtx(opCtx.get())
                              .ns(coll1Nss)
                              .view(std::move(view))
                              .build();

    auto namespaceCopy = NamespaceString::createNamespaceString_forTest("test"sv, "coll2"sv);
    auto expCtxCopy = makeCopyFromExpressionContext(expCtxOriginal, namespaceCopy);

    // expCtxCopy doesn't have a view initialized.
    ASSERT_FALSE(expCtxCopy->getView().has_value());

    // expCtxOriginal isn't affected by the copy.
    ASSERT_TRUE(expCtxOriginal->getView().has_value());
    ASSERT_EQUALS(expCtxOriginal->getView()->getNamespace(), viewNss);

    auto expCtxOriginalViewPipe = expCtxOriginal->getView()->getViewPipeline();
    const auto& expCtxOriginalViewPipeStages = expCtxOriginalViewPipe.getStages();
    ASSERT_EQUALS(expCtxOriginalViewPipeStages.size(), viewPipeline.size());
    ASSERT(expCtxOriginalViewPipeStages[0] != nullptr);
    ASSERT_BSONOBJ_EQ(expCtxOriginalViewPipeStages[0]->getOriginalBson().wrap(), viewPipeline[0]);
}

TEST_F(ExpressionContextTest, CopyWithInitializesViewWhenSpecified) {
    auto opCtx = makeOperationContext();

    auto viewNss = NamespaceString::createNamespaceString_forTest("test"sv, "view"sv);
    auto coll1Nss = NamespaceString::createNamespaceString_forTest("test"sv, "coll1"sv);
    std::vector<BSONObj> viewPipeline = {BSON("$project" << BSON("_id" << 0))};

    auto view =
        boost::make_optional(ResolvedNamespace::makeForView(viewNss, coll1Nss, viewPipeline));
    auto expCtxOriginal = mongo::ExpressionContextBuilder{}
                              .opCtx(opCtx.get())
                              .ns(coll1Nss)
                              .view(std::move(view))
                              .build();

    auto namespaceCopy = NamespaceString::createNamespaceString_forTest("test"sv, "coll2"sv);
    auto viewForCopy =
        boost::make_optional(ResolvedNamespace::makeForView(viewNss, coll1Nss, viewPipeline));
    auto expCtxCopy = makeCopyFromExpressionContext(
        expCtxOriginal, namespaceCopy, boost::none, boost::none, std::move(viewForCopy));

    // expCtxCopy has a view.
    ASSERT_TRUE(expCtxCopy->getView().has_value());
    ASSERT_EQUALS(expCtxCopy->getView()->getNamespace(), viewNss);

    auto expCtxCopyViewPipe = expCtxCopy->getView()->getViewPipeline();
    const auto& expCtxCopyViewPipeStages = expCtxCopyViewPipe.getStages();
    ASSERT_EQUALS(expCtxCopyViewPipeStages.size(), viewPipeline.size());
    ASSERT(expCtxCopyViewPipeStages[0] != nullptr);
    ASSERT_BSONOBJ_EQ(expCtxCopyViewPipeStages[0]->getOriginalBson().wrap(), viewPipeline[0]);
}

struct AddCmdTestCase {
    OptionalBool needsMerge;
    OptionalBool needsSortedMerge;

    boost::intrusive_ptr<ExpressionContext> makeExpCtx(OperationContext* opCtx) const {
        AggregateCommandRequest request(NamespaceString{});
        if (needsMerge.has_value()) {
            request.setNeedsMerge(needsMerge);
        }
        if (needsSortedMerge.has_value()) {
            request.setNeedsSortedMerge(needsSortedMerge);
        }
        return ExpressionContextBuilder{}.fromRequest(opCtx, request).build();
    }
};

TEST_F(ExpressionContextTest, MergeType) {
    auto opCtxHolder = makeOperationContext();
    auto opCtx = opCtxHolder.get();
    {
        const auto expCtx =
            AddCmdTestCase{.needsMerge = true, .needsSortedMerge = true}.makeExpCtx(opCtx);
        ASSERT_TRUE(expCtx->getNeedsMerge());
        ASSERT_FALSE(expCtx->needsUnsortedMerge());
        ASSERT_TRUE(expCtx->needsSortedMerge());
    }
    {
        const auto expCtx =
            AddCmdTestCase{.needsMerge = true, .needsSortedMerge = false}.makeExpCtx(opCtx);
        ASSERT_TRUE(expCtx->getNeedsMerge());
        ASSERT_TRUE(expCtx->needsUnsortedMerge());
        ASSERT_FALSE(expCtx->needsSortedMerge());
    }
    {
        const auto expCtx = AddCmdTestCase{.needsMerge = true}.makeExpCtx(opCtx);
        ASSERT_TRUE(expCtx->getNeedsMerge());
        ASSERT_TRUE(expCtx->needsUnsortedMerge());
        ASSERT_FALSE(expCtx->needsSortedMerge());
    }
    {
        const auto expCtx = AddCmdTestCase{}.makeExpCtx(opCtx);
        ASSERT_FALSE(expCtx->getNeedsMerge());
        ASSERT_FALSE(expCtx->needsUnsortedMerge());
        ASSERT_FALSE(expCtx->needsSortedMerge());
    }
    {
        const auto expCtx =
            AddCmdTestCase{.needsMerge = false, .needsSortedMerge = false}.makeExpCtx(opCtx);
        ASSERT_FALSE(expCtx->getNeedsMerge());
        ASSERT_FALSE(expCtx->needsUnsortedMerge());
        ASSERT_FALSE(expCtx->needsSortedMerge());
    }
    {
        const auto expCtx = AddCmdTestCase{.needsSortedMerge = false}.makeExpCtx(opCtx);
        ASSERT_FALSE(expCtx->getNeedsMerge());
        ASSERT_FALSE(expCtx->needsUnsortedMerge());
        ASSERT_FALSE(expCtx->needsSortedMerge());
    }
}

// This should tassert to detect this malformed AggregateCommandRequest.
// The 'needsSortedMerge' bit implies 'needsMerge'.
using ExpressionContextTestDeathTest = ExpressionContextTest;
DEATH_TEST_F(ExpressionContextTestDeathTest, IllegalNeedsMergeCombo, "10372401") {
    auto opCtx = makeOperationContext();
    AddCmdTestCase{.needsMerge = false, .needsSortedMerge = true}.makeExpCtx(opCtx.get());
}

DEATH_TEST_F(ExpressionContextTestDeathTest, IllegalNeedsMergeComboNeedsMergeEmpty, "10372401") {
    auto opCtx = makeOperationContext();
    AddCmdTestCase{.needsSortedMerge = true}.makeExpCtx(opCtx.get());
}

TEST_F(ExpressionContextTest, IfrContextIsSharedWithSubPipeline) {
    auto opCtx = makeOperationContext();

    auto ifrContext = std::make_shared<IncrementalFeatureRolloutContext>();
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx.get())
                      .ns(NamespaceString::createNamespaceString_forTest("test"sv, "coll"sv))
                      .ifrContext(ifrContext)
                      .build();

    ASSERT_EQ(ifrContext.get(), expCtx->getIfrContext().get());

    auto subExpCtx = makeCopyForSubPipelineFromExpressionContext(
        expCtx, NamespaceString::createNamespaceString_forTest("test"sv, "subColl"sv));

    // Verify that 'expCtx' and 'subExpCtx' share the same IFRContext.
    ASSERT_EQ(ifrContext.get(), subExpCtx->getIfrContext().get());
    ASSERT_EQ(expCtx->getIfrContext().get(), subExpCtx->getIfrContext().get());
}

TEST_F(ExpressionContextTest, AllowPartialResultsIsNotInheritedBySubPipeline) {
    auto opCtx = makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx.get())
                      .ns(NamespaceString::createNamespaceString_forTest("test"sv, "coll"sv))
                      .allowPartialResults(true)
                      .build();

    auto subExpCtx = makeCopyForSubPipelineFromExpressionContext(
        expCtx, NamespaceString::createNamespaceString_forTest("test"sv, "subColl"sv));

    ASSERT_TRUE(expCtx->getAllowPartialResults());
    ASSERT_FALSE(subExpCtx->getAllowPartialResults());
}

// Tests for ExpressionContextBuilder::fromRequest(FindCommandRequest) IDHACK eligibility.
// The key behavior: when there is no explicit request collation, isIdHackQuery is set based
// purely on query structure, regardless of whether the collection has a default collator.
// Before SERVER-123100 this was only done when the collection had no collator, which was a bug
// because an inherited (no-request) collation always matches the collection's default.

TEST_F(ExpressionContextTest, FindOnIdWithNoCollationSetsIsIdHackQuery) {
    auto opCtx = makeOperationContext();
    auto request = FindCommandRequest(NamespaceString::createNamespaceString_forTest("test.coll"));
    request.setFilter(fromjson("{_id: 1}"));

    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), request, nullptr).build();
    ASSERT_TRUE(expCtx->isIdHackQuery());
}

TEST_F(ExpressionContextTest, FindOnIdWithCollectionCollatorSetsIsIdHackQuery) {
    // Regression test for SERVER-123100: before this fix, a find on _id against a collection with
    // a custom collator (but no explicit request collation) would not set isIdHackQuery=true,
    // causing the IDHACK/express fast path to be skipped.
    auto opCtx = makeOperationContext();
    auto request = FindCommandRequest(NamespaceString::createNamespaceString_forTest("test.coll"));
    request.setFilter(fromjson("{_id: 1}"));

    CollatorInterfaceMock collectionCollator(CollatorInterfaceMock::MockType::kReverseString);
    auto expCtx =
        ExpressionContextBuilder{}.fromRequest(opCtx.get(), request, &collectionCollator).build();
    ASSERT_TRUE(expCtx->isIdHackQuery());
}

TEST_F(ExpressionContextTest, FindOnNonIdFieldDoesNotSetIsIdHackQuery) {
    auto opCtx = makeOperationContext();
    auto request = FindCommandRequest(NamespaceString::createNamespaceString_forTest("test.coll"));
    request.setFilter(fromjson("{a: 1}"));

    CollatorInterfaceMock collectionCollator(CollatorInterfaceMock::MockType::kReverseString);
    auto expCtx =
        ExpressionContextBuilder{}.fromRequest(opCtx.get(), request, &collectionCollator).build();
    ASSERT_FALSE(expCtx->isIdHackQuery());
}

TEST_F(ExpressionContextTest, FindOnIdWithMismatchedCollationDoesNotSetIsIdHackQuery) {
    // When the request carries an explicit collation that does not match the collection's default
    // collator, the collatorsMatch() check returns false and isIdHackQuery must remain false.
    // This exercises the 'else if (haveMatchingCollators)' branch in fromRequest.
    auto opCtx = makeOperationContext();
    CollatorFactoryInterface::set(opCtx->getServiceContext(),
                                  std::make_unique<CollatorFactoryMock>());

    auto request = FindCommandRequest(NamespaceString::createNamespaceString_forTest("test.coll"));
    request.setFilter(fromjson("{_id: 1}"));
    // Any non-simple spec; CollatorFactoryMock always parses non-simple specs as kReverseString.
    request.setCollation(BSON("locale" << "mock_always_equal"));

    // Collection collator is kAlwaysEqual — its spec differs from kReverseString, so
    // collatorsMatch returns false and isIdHackQuery must not be set.
    CollatorInterfaceMock collectionCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto expCtx =
        ExpressionContextBuilder{}.fromRequest(opCtx.get(), request, &collectionCollator).build();
    ASSERT_FALSE(expCtx->isIdHackQuery());
}

TEST_F(ExpressionContextTest, FindOnIdWithMatchingExplicitCollationSetsIsIdHackQuery) {
    // When the request carries an explicit collation that matches the collection's default
    // collator, haveMatchingCollators is true and isIdHackQuery must be set based on the filter.
    // CollatorFactoryMock always parses any non-simple spec as kReverseString, so both the
    // request collator and the collection collator are kReverseString and their specs match.
    auto opCtx = makeOperationContext();
    CollatorFactoryInterface::set(opCtx->getServiceContext(),
                                  std::make_unique<CollatorFactoryMock>());

    auto request = FindCommandRequest(NamespaceString::createNamespaceString_forTest("test.coll"));
    request.setFilter(fromjson("{_id: 1}"));
    request.setCollation(BSON("locale" << "mock_reverse"));

    CollatorInterfaceMock collectionCollator(CollatorInterfaceMock::MockType::kReverseString);
    auto expCtx =
        ExpressionContextBuilder{}.fromRequest(opCtx.get(), request, &collectionCollator).build();
    ASSERT_TRUE(expCtx->isIdHackQuery());
}

TEST_F(ExpressionContextTest, FindOnIdWithHintDoesNotSetIsIdHackQuery) {
    // A hint disqualifies IDHACK: isIdHackEligibleQueryWithoutCollator() returns false whenever
    // the hint is non-empty, regardless of the filter shape.
    auto opCtx = makeOperationContext();
    auto request = FindCommandRequest(NamespaceString::createNamespaceString_forTest("test.coll"));
    request.setFilter(fromjson("{_id: 1}"));
    request.setHint(fromjson("{_id: 1}"));

    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), request, nullptr).build();
    ASSERT_FALSE(expCtx->isIdHackQuery());
}

TEST_F(ExpressionContextTest, SetIsIdHackQueryIsIdempotent) {
    // setIsIdHackQuery(true) on an already-true flag must be a no-op, not a tassert failure.
    // This exercises the monotone-upgrade invariant: false→true is allowed, true→true is
    // also allowed, and true→false would fire the tassert.
    auto opCtx = makeOperationContext();
    auto request = FindCommandRequest(NamespaceString::createNamespaceString_forTest("test.coll"));
    request.setFilter(fromjson("{_id: 1}"));

    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), request, nullptr).build();
    ASSERT_TRUE(expCtx->isIdHackQuery());
    // Calling setIsIdHackQuery(true) again must not throw or tassert.
    expCtx->setIsIdHackQuery(true);
    ASSERT_TRUE(expCtx->isIdHackQuery());
}

// The expression fallback tracker rolls up into the per-query OperationMemoryUsageTracker (so the
// per-query limit is enforced) only when both memory-tracking feature flags are enabled.
TEST_F(ExpressionContextTest,
       ExpressionFallbackTrackerRollsUpOnlyWhenBothMemoryTrackingFlagsEnabled) {
    struct Case {
        bool queryTracking;
        bool expressionTracking;
        bool expectPerQueryLimitEnforced;
    };
    const Case cases[] = {
        {true, true, true},    // Both on: fallback is a child of the operation tracker.
        {true, false, false},  // Guarded specifically by the expression-tracking flag check.
        {false, true, false},
        {false, false, false},
    };

    for (const auto& c : cases) {
        unittest::ServerParameterGuard queryMemTracking{"featureFlagQueryMemoryTracking",
                                                        c.queryTracking};
        unittest::ServerParameterGuard exprMemTracking{"featureFlagExpressionMemoryTracking",
                                                       c.expressionTracking};
        // Generous per-expression cap, tiny per-query limit: only a rolled-up fallback can exceed
        // the per-query limit, since it is checked via the operation-tracker base chain.
        unittest::ServerParameterGuard exprCap{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                               10 * 1024 * 1024};
        unittest::ServerParameterGuard perQueryLimit{"internalQueryMaxMemoryUsageBytesPerOperation",
                                                     4};

        auto opCtx = makeOperationContext();
        auto expCtx = ExpressionContextBuilder{}
                          .opCtx(opCtx.get())
                          .ns(NamespaceString::createNamespaceString_forTest("test"sv, "coll"sv))
                          .build();

        auto& tracker = expCtx->getExpressionFallbackTracker();
        tracker.add(100);  // Exceeds the per-query limit but stays under the per-expression cap.
        ASSERT_EQ(!tracker.withinMemoryLimit(opCtx.get()), c.expectPerQueryLimitEnforced)
            << "queryTracking=" << c.queryTracking
            << " expressionTracking=" << c.expressionTracking;
        tracker.add(-100);  // Release so the tracker is left balanced.
    }
}

// When both memory-tracking flags are enabled and an OperationContext is available, the fallback
// tracker is bounded solely by the per-query limit.
TEST_F(ExpressionContextTest, ExpressionFallbackTrackerIgnoresPerExpressionCapWhenRolledUp) {
    unittest::ServerParameterGuard queryMemTracking{"featureFlagQueryMemoryTracking", true};
    unittest::ServerParameterGuard exprMemTracking{"featureFlagExpressionMemoryTracking", true};
    // Tiny per-expression cap, generous per-query limit: if the cap were still enforced on the
    // rolled-up path the tracker would report over-limit; it must not.
    unittest::ServerParameterGuard exprCap{"internalQueryMaxSingleExpressionMemoryUsageBytes", 4};
    unittest::ServerParameterGuard perQueryLimit{"internalQueryMaxMemoryUsageBytesPerOperation",
                                                 10 * 1024 * 1024};

    auto opCtx = makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx.get())
                      .ns(NamespaceString::createNamespaceString_forTest("test"sv, "coll"sv))
                      .build();

    auto& tracker = expCtx->getExpressionFallbackTracker();
    tracker.add(100);  // Far exceeds the per-expression cap but stays under the per-query limit.
    ASSERT_TRUE(tracker.withinMemoryLimit(opCtx.get()));
    tracker.add(-100);  // Release so the tracker is left balanced.
}

// Changing the OperationContext must drop the cached fallback tracker, so it never retains a base
// pointer into a previous operation's (possibly destroyed) tracker.
TEST_F(ExpressionContextTest, ExpressionFallbackTrackerRebuiltWhenOperationContextChanges) {
    unittest::ServerParameterGuard queryMemTracking{"featureFlagQueryMemoryTracking", true};
    unittest::ServerParameterGuard exprMemTracking{"featureFlagExpressionMemoryTracking", true};
    unittest::ServerParameterGuard exprCap{"internalQueryMaxSingleExpressionMemoryUsageBytes", 4};
    unittest::ServerParameterGuard perQueryLimit{"internalQueryMaxMemoryUsageBytesPerOperation",
                                                 10 * 1024 * 1024};

    auto opCtx = makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx.get())
                      .ns(NamespaceString::createNamespaceString_forTest("test"sv, "coll"sv))
                      .build();

    // With an OperationContext, the fallback rolls up to the operation tracker and is bounded by
    // the generous per-query limit, so it ignores the tiny per-expression cap.
    {
        auto& tracker = expCtx->getExpressionFallbackTracker();
        tracker.add(100);
        ASSERT_TRUE(tracker.withinMemoryLimit(opCtx.get()));
        tracker.add(-100);  // Release so the operation tracker is left balanced.
    }

    // Detaching the OperationContext drops the cached fallback; it is rebuilt as a standalone
    // tracker bounded by the per-expression cap. If the stale cached tracker were returned instead,
    // it would still be bounded by the generous per-query limit and stay within limit.
    expCtx->setOperationContext(nullptr);

    auto& rebuilt = expCtx->getExpressionFallbackTracker();
    rebuilt.add(100);
    ASSERT_FALSE(rebuilt.withinMemoryLimit(opCtx.get()));  // 100 > per-expression cap of 4.
    rebuilt.add(-100);
}

// A context that opts out of operation-wide memory tracking gets a standalone fallback tracker
// even when an OperationContext is available and both memory-tracking flags are enabled: the
// per-expression cap is still enforced, but nothing counts toward the per-operation limit.
TEST_F(ExpressionContextTest,
       ExpressionFallbackTrackerIsStandaloneWhenOperationMemoryTrackingExcluded) {
    unittest::ServerParameterGuard queryMemTracking{"featureFlagQueryMemoryTracking", true};
    unittest::ServerParameterGuard exprMemTracking{"featureFlagExpressionMemoryTracking", true};

    // Tiny per-operation limit, generous per-expression cap: an opted-out fallback must not
    // enforce the per-operation limit.
    {
        unittest::ServerParameterGuard exprCap{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                               10 * 1024 * 1024};
        unittest::ServerParameterGuard perOpLimit{"internalQueryMaxMemoryUsageBytesPerOperation",
                                                  4};

        auto opCtx = makeOperationContext();
        auto expCtx = ExpressionContextBuilder{}
                          .opCtx(opCtx.get())
                          .ns(NamespaceString::createNamespaceString_forTest("test"sv, "coll"sv))
                          .excludeOperationMemoryTracking(true)
                          .build();

        auto& tracker = expCtx->getExpressionFallbackTracker();
        tracker.add(100);  // Exceeds the per-operation limit; a rolled-up tracker would fail.
        ASSERT_TRUE(tracker.withinMemoryLimit(opCtx.get()));
        tracker.add(-100);
    }

    // Tiny per-expression cap, generous per-operation limit: the standalone per-expression cap
    // must still be enforced.
    {
        unittest::ServerParameterGuard exprCap{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                               4};
        unittest::ServerParameterGuard perOpLimit{"internalQueryMaxMemoryUsageBytesPerOperation",
                                                  10 * 1024 * 1024};

        auto opCtx = makeOperationContext();
        auto expCtx = ExpressionContextBuilder{}
                          .opCtx(opCtx.get())
                          .ns(NamespaceString::createNamespaceString_forTest("test"sv, "coll"sv))
                          .excludeOperationMemoryTracking(true)
                          .build();

        auto& tracker = expCtx->getExpressionFallbackTracker();
        tracker.add(100);  // 100 > per-expression cap of 4.
        ASSERT_FALSE(tracker.withinMemoryLimit(opCtx.get()));
        tracker.add(-100);
    }
}

TEST_F(ExpressionContextTest, ExcludeOperationMemoryTrackingIsPropagatedToCopies) {
    auto opCtx = makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx.get())
                      .ns(NamespaceString::createNamespaceString_forTest("test"sv, "coll"sv))
                      .excludeOperationMemoryTracking(true)
                      .build();

    auto copy = makeCopyFromExpressionContext(
        expCtx, NamespaceString::createNamespaceString_forTest("test"sv, "other"sv));
    ASSERT_TRUE(copy->getExcludeOperationMemoryTracking());
}

TEST_F(
    ExpressionContextTest,
    ExpressionFallbackTrackerStandaloneWhenExpressionFallbackExcludedFromOperationMemoryTracking) {
    unittest::ServerParameterGuard queryMemTracking{"featureFlagQueryMemoryTracking", true};
    unittest::ServerParameterGuard exprMemTracking{"featureFlagExpressionMemoryTracking", true};
    // Tiny per-expression cap, generous per-query limit. If the fallback rolled up to the operation
    // tracker it would be bounded by the generous per-query limit and stay within limit; a
    // standalone fallback is bounded by the tiny per-expression cap and exceeds it.
    unittest::ServerParameterGuard exprCap{"internalQueryMaxSingleExpressionMemoryUsageBytes", 4};
    unittest::ServerParameterGuard perQueryLimit{"internalQueryMaxMemoryUsageBytesPerOperation",
                                                 10 * 1024 * 1024};

    auto opCtx = makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx.get())
                      .ns(NamespaceString::createNamespaceString_forTest("test"sv, "coll"sv))
                      .excludeExpressionFallbackFromOperationMemoryTracking(true)
                      .build();

    auto& tracker = expCtx->getExpressionFallbackTracker();
    tracker.add(100);  // Exceeds the per-expression cap; only a rolled-up tracker would be within.
    ASSERT_FALSE(tracker.withinMemoryLimit(opCtx.get()));
    tracker.add(-100);
}

TEST_F(ExpressionContextTest, StageMemoryTrackerUnaffectedByExpressionFallbackExclusion) {
    unittest::ServerParameterGuard queryMemTracking{"featureFlagQueryMemoryTracking", true};
    unittest::ServerParameterGuard exprMemTracking{"featureFlagExpressionMemoryTracking", true};
    // Tiny per-query limit; the stage tracker's own cap is left at its generous default. If the
    // stage tracker rolls up (as it must), it is bounded by the tiny per-query limit and exceeds
    // it.
    unittest::ServerParameterGuard perQueryLimit{"internalQueryMaxMemoryUsageBytesPerOperation", 4};

    auto opCtx = makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx.get())
                      .ns(NamespaceString::createNamespaceString_forTest("test"sv, "coll"sv))
                      .excludeExpressionFallbackFromOperationMemoryTracking(true)
                      .build();

    auto stageTracker =
        OperationMemoryUsageTracker::createChunkedSimpleMemoryUsageTrackerForStage(*expCtx);
    stageTracker.add(100);  // Exceeds the per-query limit via the operation-tracker base chain.
    ASSERT_FALSE(stageTracker.withinMemoryLimit(opCtx.get()));
    stageTracker.add(-100);
}

}  // namespace
}  // namespace mongo
