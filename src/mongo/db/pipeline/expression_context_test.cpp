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
#include "mongo/db/feature_flag_test_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
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
        const auto expCtx = ExpressionContext{
            opCtx.get(),
            {},     // explain
            false,  // fromMongos
            false,  // needsMerge
            false,  // allowDiskUse
            false,  // bypassDocumentValidation
            false,  // isMapReduce
            NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd),
            LegacyRuntimeConstants{Date_t::now(), {}},
            {},  // collator
            std::make_shared<StubMongoProcessInterface>(),
            {},  // resolvedNamespaces
            {},  // collUUID
            {},  // let
            false};
        // LegacyRuntimeConstants is passed to the constructor of ExpressionContext and should make
        // $$NOW and $$CLUSTER_TIME available to be referenced.
        ASSERT_DOES_NOT_THROW(static_cast<void>(expCtx.variables.getValue(Variables::kNowId)));
        ASSERT_DOES_NOT_THROW(
            static_cast<void>(expCtx.variables.getValue(Variables::kClusterTimeId)));
    }
    {
        const auto expCtx = ExpressionContext{
            opCtx.get(),
            {},     // explain
            false,  // fromMongos
            false,  // needsMerge
            false,  // allowDiskUse
            false,  // bypassDocumentValidation
            false,  // isMapReduce
            NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd),
            LegacyRuntimeConstants{{}, Timestamp(1, 0)},
            {},  // collator
            std::make_shared<StubMongoProcessInterface>(),
            {},  // resolvedNamespaces
            {},  // collUUID
            {},  // let
            false};
        // LegacyRuntimeConstants is passed to the constructor of ExpressionContext and should make
        // $$NOW and $$CLUSTER_TIME available to be referenced.
        ASSERT_DOES_NOT_THROW(static_cast<void>(expCtx.variables.getValue(Variables::kNowId)));
        ASSERT_DOES_NOT_THROW(
            static_cast<void>(expCtx.variables.getValue(Variables::kClusterTimeId)));
    }
}

TEST_F(ExpressionContextTest, ParametersCanContainExpressionsWhichAreFolded) {
    auto opCtx = makeOperationContext();
    const auto expCtx =
        ExpressionContext{opCtx.get(),
                          {},     // explain
                          false,  // fromMongos
                          false,  // needsMerge
                          false,  // allowDiskUse
                          false,  // bypassDocumentValidation
                          false,  // isMapReduce
                          NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd),
                          {},  // runtime constants
                          {},  // collator
                          std::make_shared<StubMongoProcessInterface>(),
                          {},  // resolvedNamespaces
                          {},  // collUUID
                          BSON("atan2" << BSON("$atan2" << BSON_ARRAY(0 << 1))),
                          false};
    ASSERT_EQUALS(
        0.0,
        expCtx.variables.getValue(expCtx.variablesParseState.getVariable("atan2")).getDouble());
}

TEST_F(ExpressionContextTest, ParametersCanReferToAlreadyDefinedParameters) {
    auto opCtx = makeOperationContext();
    const auto expCtx =
        ExpressionContext{opCtx.get(),
                          {},     // explain
                          false,  // fromMongos
                          false,  // needsMerge
                          false,  // allowDiskUse
                          false,  // bypassDocumentValidation
                          false,  // isMapReduce
                          NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd),
                          {},  // runtime constants
                          {},  // collator
                          std::make_shared<StubMongoProcessInterface>(),
                          {},  // resolvedNamespaces
                          {},  // collUUID
                          BSON("a" << 12 << "b"
                                   << "$$a"
                                   << "c"
                                   << "$$b"),
                          false};
    ASSERT_EQUALS(
        12.0, expCtx.variables.getValue(expCtx.variablesParseState.getVariable("c")).getDouble());
}

TEST_F(ExpressionContextTest, ParametersCanOverwriteInLeftToRightOrder) {
    auto opCtx = makeOperationContext();
    const auto expCtx =
        ExpressionContext{opCtx.get(),
                          {},     // explain
                          false,  // fromMongos
                          false,  // needsMerge
                          false,  // allowDiskUse
                          false,  // bypassDocumentValidation
                          false,  // isMapReduce
                          NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd),
                          {},  // runtime constants
                          {},  // collator
                          std::make_shared<StubMongoProcessInterface>(),
                          {},  // resolvedNamespaces
                          {},  // collUUID
                          BSON("x" << 12 << "b" << 10 << "x" << 20),
                          false};
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
                                            NamespaceString::createNamespaceString_forTest(
                                                "test"_sd, "namespace"_sd),
                                            {},  // runtime constants
                                            {},  // collator
                                            std::make_shared<StubMongoProcessInterface>(),
                                            {},  // resolvedNamespaces
                                            {},  // collUUID
                                            BSON("a"
                                                 << "$b"),
                                            false}),
        DBException,
        4890500);
}

TEST_F(ExpressionContextTest, ParametersCauseGracefulFailuresIfUppercase) {
    auto opCtx = makeOperationContext();
    ASSERT_THROWS_CODE(
        static_cast<void>(ExpressionContext{
            opCtx.get(),
            {},     // explain
            false,  // fromMongos
            false,  // needsMerge
            false,  // allowDiskUse
            false,  // bypassDocumentValidation
            false,  // isMapReduce
            NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd),
            {},  // runtime constants
            {},  // collator
            std::make_shared<StubMongoProcessInterface>(),
            {},  // resolvedNamespaces
            {},  // collUUID
            BSON("A" << 12),
            false}),
        DBException,
        ErrorCodes::FailedToParse);
}

TEST_F(ExpressionContextTest, DontInitializeUnreferencedVariables) {
    auto opCtx = makeOperationContext();
    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << BSON("a" << 1)));
    AggregateCommandRequest acr({} /*nss*/, pipeline);
    ResolvedNamespaceMap sm;
    auto expCtx = make_intrusive<ExpressionContext>(opCtx.get(),
                                                    acr,
                                                    nullptr /*collator*/,
                                                    nullptr /*mongoProcessInterface*/,
                                                    sm,
                                                    boost::none /*collUUID*/);
    Pipeline::parse(pipeline, expCtx);
    expCtx->initializeReferencedSystemVariables();
    ASSERT_FALSE(expCtx->variables.hasValue(Variables::kNowId));
    ASSERT_FALSE(expCtx->variables.hasValue(Variables::kClusterTimeId));
    ASSERT_FALSE(expCtx->variables.hasValue(Variables::kUserRolesId));
}

TEST_F(ExpressionContextTest, UseLastLTSFeatureFlagWhenFCVUninitialized) {
    auto opCtx = makeOperationContext();
    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << BSON("a" << 1)));
    AggregateCommandRequest acr({} /*nss*/, pipeline);
    ResolvedNamespaceMap sm;
    auto expCtx = make_intrusive<ExpressionContext>(opCtx.get(),
                                                    acr,
                                                    nullptr /*collator*/,
                                                    nullptr /*mongoProcessInterface*/,
                                                    sm,
                                                    boost::none /*collUUID*/);

    const auto kOriginalFCV =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ON_BLOCK_EXIT([&] { serverGlobalParams.mutableFCV.setVersion(kOriginalFCV); });
    // (Generic FCV reference): for testing only. This comment is required by linter.
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::FeatureCompatibilityVersion::kUnsetDefaultLastLTSBehavior);

    // If the FCV is uninitialized, we should check the feature flag is enabled on the lastLTS
    // version.
    // 'featureFlagSpoon' should be enabled, since it is a test feature flag that is always enabled
    // on the lastLTS version.
    ASSERT_DOES_NOT_THROW(
        expCtx->throwIfFeatureFlagIsNotEnabledOnFCV("foo", feature_flags::gFeatureFlagSpoon));

    // 'featureFlagBlender' should not be enabled, since it is a test feature flag that is always
    // enabled on the latest version.
    ASSERT_THROWS_CODE(
        expCtx->throwIfFeatureFlagIsNotEnabledOnFCV("foo", feature_flags::gFeatureFlagBlender),
        DBException,
        ErrorCodes::QueryFeatureNotAllowed);
}

TEST_F(ExpressionContextTest, UseLatestFeatureFlagWhenFCVInitialized) {
    auto opCtx = makeOperationContext();
    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << BSON("a" << 1)));
    AggregateCommandRequest acr({} /*nss*/, pipeline);
    ResolvedNamespaceMap sm;
    auto expCtx = make_intrusive<ExpressionContext>(opCtx.get(),
                                                    acr,
                                                    nullptr /*collator*/,
                                                    nullptr /*mongoProcessInterface*/,
                                                    sm,
                                                    boost::none /*collUUID*/);

    const auto& fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    ASSERT_TRUE(fcvSnapshot.isVersionInitialized());
    // (Generic FCV reference): for testing only. This comment is required by linter.
    ASSERT_TRUE(fcvSnapshot.isGreaterThanOrEqualTo(multiversion::GenericFCV::kLatest));

    // Since the FCV is initialized and is the latest version, 'featureFlagSpoon', enabled on the
    // lastLTS, and 'featureFlagBlender', enabled on the latest version, should both be enabled.
    ASSERT_DOES_NOT_THROW(
        expCtx->throwIfFeatureFlagIsNotEnabledOnFCV("foo", feature_flags::gFeatureFlagSpoon));
    ASSERT_DOES_NOT_THROW(
        expCtx->throwIfFeatureFlagIsNotEnabledOnFCV("foo", feature_flags::gFeatureFlagBlender));
}

}  // namespace
}  // namespace mongo
