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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#define ASSERT_DOES_NOT_THROW(EXPRESSION)                                          \
    try {                                                                          \
        EXPRESSION;                                                                \
    } catch (const AssertionException& e) {                                        \
        str::stream err;                                                           \
        err << "Threw an exception incorrectly: " << e.toString();                 \
        ::mongo::unittest::TestAssertionFailure(__FILE__, __LINE__, err).stream(); \
    }

namespace mongo {
namespace {

using ExpressionContextTest = ServiceContextTest;

TEST_F(ExpressionContextTest, ExpressionContextSummonsMissingTimeValues) {
    auto opCtx = makeOperationContext();
    auto logicalClock = std::make_unique<LogicalClock>(opCtx->getServiceContext());
    auto t1 = logicalClock->reserveTicks(1);
    t1.addTicks(100);
    ASSERT_OK(logicalClock->advanceClusterTime(t1));
    LogicalClock::set(opCtx->getServiceContext(), std::move(logicalClock));
    {
        const auto expCtx = ExpressionContext{
            opCtx.get(),
            {},     // explain
            false,  // fromMongos
            false,  // needsMerge
            false,  // allowDiskUse
            false,  // bypassDocumentValidation
            false,  // isMapReduce
            NamespaceString{"test"_sd, "namespace"_sd},
            {},  // runtime constants
            {},  // collator
            std::make_shared<StubMongoProcessInterface>(),
            {},  // resolvedNamespaces
            {},  // collUUID
            false,
        };
        ASSERT_DOES_NOT_THROW(static_cast<void>(expCtx.variables.getValue(Variables::kNowId)));
        ASSERT_DOES_NOT_THROW(
            static_cast<void>(expCtx.variables.getValue(Variables::kClusterTimeId)));
    }
    {
        const auto expCtx = ExpressionContext{opCtx.get(),
                                              {},     // explain
                                              false,  // fromMongos
                                              false,  // needsMerge
                                              false,  // allowDiskUse
                                              false,  // bypassDocumentValidation
                                              false,  // isMapReduce
                                              NamespaceString{"test"_sd, "namespace"_sd},
                                              RuntimeConstants{Date_t::now(), {}},
                                              {},  // collator
                                              std::make_shared<StubMongoProcessInterface>(),
                                              {},  // resolvedNamespaces
                                              {},  // collUUID
                                              false};
        ASSERT_DOES_NOT_THROW(static_cast<void>(expCtx.variables.getValue(Variables::kNowId)));
        ASSERT_DOES_NOT_THROW(
            static_cast<void>(expCtx.variables.getValue(Variables::kClusterTimeId)));
    }
    {
        const auto expCtx = ExpressionContext{opCtx.get(),
                                              {},     // explain
                                              false,  // fromMongos
                                              false,  // needsMerge
                                              false,  // allowDiskUse
                                              false,  // bypassDocumentValidation
                                              false,  // isMapReduce
                                              NamespaceString{"test"_sd, "namespace"_sd},
                                              RuntimeConstants{{}, Timestamp(1, 0)},
                                              {},  // collator
                                              std::make_shared<StubMongoProcessInterface>(),
                                              {},  // resolvedNamespaces
                                              {},  // collUUID
                                              false};
        ASSERT_DOES_NOT_THROW(static_cast<void>(expCtx.variables.getValue(Variables::kNowId)));
        ASSERT_DOES_NOT_THROW(
            static_cast<void>(expCtx.variables.getValue(Variables::kClusterTimeId)));
    }
}

TEST_F(ExpressionContextTest, ParametersCanContainExpressionsWhichAreFolded) {
    auto opCtx = makeOperationContext();
    const auto expCtx = ExpressionContext{opCtx.get(),
                                          {},     // explain
                                          false,  // fromMongos
                                          false,  // needsMerge
                                          false,  // allowDiskUse
                                          false,  // bypassDocumentValidation
                                          false,  // isMapReduce
                                          NamespaceString{"test"_sd, "namespace"_sd},
                                          {},  // runtime constants
                                          {},  // collator
                                          std::make_shared<StubMongoProcessInterface>(),
                                          {},  // resolvedNamespaces
                                          {},  // collUUID
                                          false,
                                          BSON("atan2" << BSON("$atan2" << BSON_ARRAY(0 << 1)))};
    ASSERT_EQUALS(
        0.0,
        expCtx.variables.getValue(expCtx.variablesParseState.getVariable("atan2")).getDouble());
}

TEST_F(ExpressionContextTest, ParametersCanReferToAlreadyDefinedParameters) {
    auto opCtx = makeOperationContext();
    const auto expCtx = ExpressionContext{opCtx.get(),
                                          {},     // explain
                                          false,  // fromMongos
                                          false,  // needsMerge
                                          false,  // allowDiskUse
                                          false,  // bypassDocumentValidation
                                          false,  // isMapReduce
                                          NamespaceString{"test"_sd, "namespace"_sd},
                                          {},  // runtime constants
                                          {},  // collator
                                          std::make_shared<StubMongoProcessInterface>(),
                                          {},  // resolvedNamespaces
                                          {},  // collUUID
                                          false,
                                          BSON("a" << 12 << "b"
                                                   << "$$a"
                                                   << "c"
                                                   << "$$b")};
    ASSERT_EQUALS(
        12.0, expCtx.variables.getValue(expCtx.variablesParseState.getVariable("c")).getDouble());
}

TEST_F(ExpressionContextTest, ParametersCanOverwriteInLeftToRightOrder) {
    auto opCtx = makeOperationContext();
    const auto expCtx = ExpressionContext{opCtx.get(),
                                          {},     // explain
                                          false,  // fromMongos
                                          false,  // needsMerge
                                          false,  // allowDiskUse
                                          false,  // bypassDocumentValidation
                                          false,  // isMapReduce
                                          NamespaceString{"test"_sd, "namespace"_sd},
                                          {},  // runtime constants
                                          {},  // collator
                                          std::make_shared<StubMongoProcessInterface>(),
                                          {},  // resolvedNamespaces
                                          {},  // collUUID
                                          false,
                                          BSON("x" << 12 << "b" << 10 << "x" << 20)};
    ASSERT_EQUALS(
        20, expCtx.variables.getValue(expCtx.variablesParseState.getVariable("x")).getDouble());
}

TEST_F(ExpressionContextTest, ParametersCauseGracefulFailuresIfNonConstant) {
    auto opCtx = makeOperationContext();
    ASSERT_THROWS_CODE(
        static_cast<void>(ExpressionContext{opCtx.get(),
                                            {},     // explain
                                            false,  // fromMongos
                                            false,  // needsMerge
                                            false,  // allowDiskUse
                                            false,  // bypassDocumentValidation
                                            false,  // isMapReduce
                                            NamespaceString{"test"_sd, "namespace"_sd},
                                            {},  // runtime constants
                                            {},  // collator
                                            std::make_shared<StubMongoProcessInterface>(),
                                            {},  // resolvedNamespaces
                                            {},  // collUUID
                                            false,
                                            BSON("a"
                                                 << "$b")}),
        DBException,
        31474);
}

TEST_F(ExpressionContextTest, ParametersCauseGracefulFailuresIfUppercase) {
    auto opCtx = makeOperationContext();
    ASSERT_THROWS_CODE(
        static_cast<void>(ExpressionContext{opCtx.get(),
                                            {},     // explain
                                            false,  // fromMongos
                                            false,  // needsMerge
                                            false,  // allowDiskUse
                                            false,  // bypassDocumentValidation
                                            false,  // isMapReduce
                                            NamespaceString{"test"_sd, "namespace"_sd},
                                            {},  // runtime constants
                                            {},  // collator
                                            std::make_shared<StubMongoProcessInterface>(),
                                            {},  // resolvedNamespaces
                                            {},  // collUUID
                                            false,
                                            BSON("A" << 12)}),
        DBException,
        16867);
}
}  // namespace
}  // namespace mongo
