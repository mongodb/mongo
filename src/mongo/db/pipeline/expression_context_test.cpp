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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

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

}  // namespace
}  // namespace mongo
