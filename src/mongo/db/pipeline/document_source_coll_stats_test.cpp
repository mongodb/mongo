// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_coll_stats.h"

#include "mongo/db/metrics_policy_manager.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_coll_stats_gen.h"
#include "mongo/db/pipeline/storage_stats_spec_gen.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <cstdint>

#include <gmock/gmock.h>

namespace mongo {
namespace {
using DocumentSourceCollStatsTest = AggregationContextFixture;

auto representativeShape(const DocumentSourceCollStats& collStatsStage) {
    query_shape::SerializationOptions opts{
        .literalPolicy = query_shape::LiteralSerializationPolicy::kToRepresentativeParseableValue};
    return collStatsStage.serialize(opts).getDocument().toBson();
}

TEST_F(DocumentSourceCollStatsTest, QueryShape) {
    auto spec = DocumentSourceCollStatsSpec();

    auto stage = make_intrusive<DocumentSourceCollStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$collStats":{}})",
        redact(*stage));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$collStats":{}})",
        representativeShape(*stage));

    spec.setCount(BSONObj());
    spec.setQueryExecStats(BSONObj());
    stage = make_intrusive<DocumentSourceCollStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$collStats":{"count":{},"queryExecStats":{}}})",
        redact(*stage));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$collStats":{"count":{},"queryExecStats":{}}})",
        representativeShape(*stage));

    auto latencyStats = LatencyStatsSpec();
    latencyStats.setHistograms(true);
    spec.setLatencyStats(latencyStats);
    stage = make_intrusive<DocumentSourceCollStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$collStats": {
                "latencyStats": {
                    "histograms": true
                },
                "count": {},
                "queryExecStats": {}
            }
        })",
        redact(*stage));

    auto storageStats = StorageStatsSpec();
    storageStats.setScale(2);
    storageStats.setVerbose(true);
    spec.setStorageStats(storageStats);
    stage = make_intrusive<DocumentSourceCollStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$collStats": {
                "latencyStats": {
                    "histograms": true
                },
                "storageStats": {
                    "scale": "?number",
                    "verbose": true,
                    "waitForLock": true,
                    "numericOnly": false
                },
                "count": {},
                "queryExecStats": {}
            }
        })",
        redact(*stage));

    storageStats.setWaitForLock(false);
    storageStats.setNumericOnly(false);
    spec.setStorageStats(storageStats);
    stage = make_intrusive<DocumentSourceCollStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$collStats": {
                "latencyStats": {
                    "histograms": true
                },
                "storageStats": {
                    "scale": "?number",
                    "verbose": true,
                    "waitForLock": false,
                    "numericOnly": false
                },
                "count": {},
                "queryExecStats": {}
            }
        })",
        redact(*stage));
}

using ForceFilteredTestParams = std::tuple<boost::optional<bool>,  // forceFiltered
                                           bool,                   // fromRouter
                                           bool                    // featureFlagEnabled
                                           >;

class MetricsPolicyManagerMock : public MetricsPolicyManager {
public:
    MOCK_METHOD(bool,
                requiresFiltering,
                (OperationContext*, MetricsCategoryEnum, bool),
                (const, override));

    MOCK_METHOD(const std::vector<std::string>&,
                getAllowlistPaths,
                (MetricsCategoryEnum),
                (const, override));

    MOCK_METHOD(const PathMatcherNode&,
                getAllowlistMatcher,
                (MetricsCategoryEnum),
                (const, override));
};

class DocumentSourceCollStatsForceFilteredTest
    : public DocumentSourceCollStatsTest,
      public ::testing::WithParamInterface<ForceFilteredTestParams> {};

std::string buildForceFilteredTestName(
    const ::testing::TestParamInfo<ForceFilteredTestParams>& info) {
    auto [forceFiltered, fromRouter, featureFlagEnabled] = info.param;

    std::string forceFilteredDesc = forceFiltered ? (*forceFiltered ? "true" : "false") : "notSet";
    return forceFilteredDesc + "_" + (fromRouter ? "fromRouter" : "notFromRouter") + "_" +
        (featureFlagEnabled ? "flagEnabled" : "flagDisabled");
}

INSTANTIATE_TEST_SUITE_P(
    ForceFiltered,
    DocumentSourceCollStatsForceFilteredTest,
    ::testing::Combine(::testing::Values(true, false, boost::none),  // forceFiltered
                       ::testing::Values(true, false),               // fromRouter
                       ::testing::Values(true, false)                // featureFlagEnabled
                       ),
    buildForceFilteredTestName);

TEST_P(DocumentSourceCollStatsForceFilteredTest, ForceFiltered) {
    auto [forceFiltered, fromRouter, featureFlagEnabled] = GetParam();
    bool inRouter = !fromRouter;

    unittest::ServerParameterGuard featureFlag{"featureFlagCollStatsMetricsFiltering",
                                               featureFlagEnabled};

    if (fromRouter) {
        getExpCtx()->setFromRouter(true);
    }

    if (inRouter) {
        getExpCtx()->setInRouter(true);
    }

    // Make 'requiresFiltering' return the same value as 'featureFlagEnabled'.
    auto mockManager = std::make_unique<MetricsPolicyManagerMock>();
    EXPECT_CALL(*mockManager, requiresFiltering)
        .WillRepeatedly(::testing::Return(featureFlagEnabled));
    MetricsPolicyManager::set(getExpCtx()->getServiceContext(), std::move(mockManager));

    BSONObjBuilder builder;
    builder.append("count", BSONObj());
    if (forceFiltered) {
        builder.append("forceFiltered", *forceFiltered);
    }

    BSONObj cmdObj = BSON("$collStats" << builder.obj());
    BSONElement elem = cmdObj.getField("$collStats");

    // Validation fails only when forceFiltered=true and fromRouter=true and
    // featureFlagEnabled=false.
    bool shouldSucceed = !(forceFiltered && *forceFiltered && fromRouter && !featureFlagEnabled);

    if (shouldSucceed) {
        auto stage = DocumentSourceCollStats::createFromBson(elem, getExpCtx());
        ASSERT(stage);

        auto collStatsStage = dynamic_cast<DocumentSourceCollStats*>(stage.get());
        ASSERT(collStatsStage);

        auto stageObj = representativeShape(*collStatsStage);
        auto collStatsObj = stageObj["$collStats"].embeddedObject();

        if (inRouter) {
            // On a router, 'forceFiltered' is modified based on the metrics policy.
            // - requiresFiltering=true (featureFlagEnabled=true). It gets set to true.
            // - requiresFiltering=false (featureFlagEnabled=false). It gets unset.
            if (featureFlagEnabled) {
                ASSERT(collStatsObj.hasField("forceFiltered"));
                ASSERT_EQ(collStatsObj["forceFiltered"].Bool(), true);
            } else {
                ASSERT(!collStatsObj.hasField("forceFiltered"));
            }
        } else {
            // On a shard, 'forceFiltered' is kept as-is regardless of the metrics policy.
            if (forceFiltered) {
                ASSERT(collStatsObj.hasField("forceFiltered"));
                ASSERT_EQ(collStatsObj["forceFiltered"].Bool(), *forceFiltered);
            } else {
                ASSERT(!collStatsObj.hasField("forceFiltered"));
            }
        }
    } else {
        ASSERT_THROWS_CODE(DocumentSourceCollStats::createFromBson(elem, getExpCtx()),
                           AssertionException,
                           ErrorCodes::IllegalOperation);
    }
}
}  // namespace
}  // namespace mongo
