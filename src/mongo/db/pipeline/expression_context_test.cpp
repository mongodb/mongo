/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/pipeline/expression_context.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

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
                .ns(NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd))
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
                .ns(NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd))
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
            .ns(NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd))
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
            .ns(mongo::NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd))
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
        static_cast<void>(mongo::ExpressionContextBuilder{}
                              .opCtx(opCtx.get())
                              .ns(mongo::NamespaceString::createNamespaceString_forTest(
                                  "test"_sd, "namespace"_sd))
                              .letParameters(BSON("a" << "$b"))
                              .build()),
        mongo::DBException,
        4890500);
}

TEST_F(ExpressionContextTest, ParametersCauseGracefulFailuresIfUppercase) {
    auto opCtx = makeOperationContext();
    ASSERT_THROWS_CODE(
        static_cast<void>(mongo::ExpressionContextBuilder{}
                              .opCtx(opCtx.get())
                              .ns(mongo::NamespaceString::createNamespaceString_forTest(
                                  "test"_sd, "namespace"_sd))
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
    Pipeline::parse(pipeline, expCtx);
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
    Pipeline::parse(pipeline, expCtx);
    ASSERT_THROWS_CODE(expCtx->initializeReferencedSystemVariables(), AssertionException, 10071200);
}

TEST_F(ExpressionContextTest, CanBuildWithoutView) {
    auto opCtx = makeOperationContext();

    auto expCtxWithoutView =
        mongo::ExpressionContextBuilder{}
            .opCtx(opCtx.get())
            .ns(NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd))
            .build();

    ASSERT_FALSE(expCtxWithoutView->getView().has_value());
}

TEST_F(ExpressionContextTest, CanBuildWithView) {
    auto opCtx = makeOperationContext();

    auto viewNss = NamespaceString::createNamespaceString_forTest("test"_sd, "view"_sd);
    std::vector<BSONObj> viewPipeline = {BSON("$project" << BSON("_id" << 0))};

    auto view = boost::make_optional(std::make_pair(viewNss, viewPipeline));
    auto expCtxWithView =
        mongo::ExpressionContextBuilder{}
            .opCtx(opCtx.get())
            .ns(NamespaceString::createNamespaceString_forTest("test"_sd, "coll"_sd))
            .view(view)
            .build();

    // expCtx namespace isn't affected by the view namespace.
    ASSERT_EQUALS(expCtxWithView->getNamespaceString(),
                  NamespaceString::createNamespaceString_forTest("test"_sd, "coll"_sd));

    ASSERT_TRUE(expCtxWithView->getView().has_value());
    ASSERT_EQUALS(expCtxWithView->getView()->first, viewNss);
    ASSERT_EQUALS(expCtxWithView->getView()->second.size(), viewPipeline.size());
    ASSERT_BSONOBJ_EQ(expCtxWithView->getView()->second[0], viewPipeline[0]);
}

TEST_F(ExpressionContextTest, CopyWithDoesNotInitializeViewByDefault) {
    auto opCtx = makeOperationContext();

    auto viewNss = NamespaceString::createNamespaceString_forTest("test"_sd, "view"_sd);
    std::vector<BSONObj> viewPipeline = {BSON("$project" << BSON("_id" << 0))};

    auto view = boost::make_optional(std::make_pair(viewNss, viewPipeline));
    auto expCtxOriginal =
        mongo::ExpressionContextBuilder{}
            .opCtx(opCtx.get())
            .ns(NamespaceString::createNamespaceString_forTest("test"_sd, "coll1"_sd))
            .view(view)
            .build();

    auto namespaceCopy = NamespaceString::createNamespaceString_forTest("test"_sd, "coll2"_sd);
    auto expCtxCopy = makeCopyFromExpressionContext(expCtxOriginal, namespaceCopy);

    // expCtxCopy doesn't have a view initialized.
    ASSERT_FALSE(expCtxCopy->getView().has_value());

    // expCtxOriginal isn't affected by the copy.
    ASSERT_TRUE(expCtxOriginal->getView().has_value());
    ASSERT_EQUALS(expCtxOriginal->getView()->first, viewNss);
    ASSERT_EQUALS(expCtxOriginal->getView()->second.size(), viewPipeline.size());
    ASSERT_BSONOBJ_EQ(expCtxOriginal->getView()->second[0], viewPipeline[0]);
}

TEST_F(ExpressionContextTest, CopyWithInitializesViewWhenSpecified) {
    auto opCtx = makeOperationContext();

    auto viewNss = NamespaceString::createNamespaceString_forTest("test"_sd, "view"_sd);
    std::vector<BSONObj> viewPipeline = {BSON("$project" << BSON("_id" << 0))};

    auto view = boost::make_optional(std::make_pair(viewNss, viewPipeline));
    auto expCtxOriginal =
        mongo::ExpressionContextBuilder{}
            .opCtx(opCtx.get())
            .ns(NamespaceString::createNamespaceString_forTest("test"_sd, "coll1"_sd))
            .view(view)
            .build();

    auto namespaceCopy = NamespaceString::createNamespaceString_forTest("test"_sd, "coll2"_sd);
    auto expCtxCopy = makeCopyFromExpressionContext(
        expCtxOriginal, namespaceCopy, boost::none, boost::none, view);

    // expCtxCopy has a view.
    ASSERT_TRUE(expCtxCopy->getView().has_value());
    ASSERT_EQUALS(expCtxCopy->getView()->first, viewNss);
    ASSERT_EQUALS(expCtxCopy->getView()->second.size(), viewPipeline.size());
    ASSERT_BSONOBJ_EQ(expCtxCopy->getView()->second[0], viewPipeline[0]);
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
DEATH_TEST_F(ExpressionContextTest, IllegalNeedsMergeCombo, "10372401") {
    auto opCtx = makeOperationContext();
    AddCmdTestCase{.needsMerge = false, .needsSortedMerge = true}.makeExpCtx(opCtx.get());
}

DEATH_TEST_F(ExpressionContextTest, IllegalNeedsMergeComboNeedsMergeEmpty, "10372401") {
    auto opCtx = makeOperationContext();
    AddCmdTestCase{.needsSortedMerge = true}.makeExpCtx(opCtx.get());
}

}  // namespace
}  // namespace mongo
