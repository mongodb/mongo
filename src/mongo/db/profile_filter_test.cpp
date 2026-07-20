// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/profile_filter_impl.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class ProfileFilterTest : public ServiceContextTest {
public:
    void setUp() final {
        ServiceContextTest::setUp();

        opCtxPtr = makeOperationContext();
        opCtx = opCtxPtr.get();
        curop = CurOp::get(*opCtx);
        opDebug = &curop->debug();
        expCtx = ExpressionContextBuilder{}.opCtx(opCtx).build();
    }
    ServiceContext::UniqueOperationContext opCtxPtr;
    OperationContext* opCtx;
    CurOp* curop;
    OpDebug* opDebug;
    boost::intrusive_ptr<ExpressionContext> expCtx;
};

TEST_F(ProfileFilterTest, FilterOnAllOpDebugFields) {
    // NOLINTNEXTLINE
    const std::unordered_set<std::string> allowedOpDebugFields = {"ts",
                                                                  "client",
                                                                  "appName",
                                                                  "allUsers",
                                                                  "user",
                                                                  "op",
                                                                  "ns",
                                                                  "command",
                                                                  "originatingCommand",
                                                                  "nShards",
                                                                  "cursorid",
                                                                  "mongot",
                                                                  "exhaust",
                                                                  "keysExamined",
                                                                  "docsExamined",
                                                                  "hasSortStage",
                                                                  "usedDisk",
                                                                  "fromMultiPlanner",
                                                                  "fromPlanCache",
                                                                  "replanned",
                                                                  "replanReason",
                                                                  "nMatched",
                                                                  "nBatches",
                                                                  "nModified",
                                                                  "ninserted",
                                                                  "ndeleted",
                                                                  "nUpserted",
                                                                  "cursorExhausted",
                                                                  "keysInserted",
                                                                  "keysDeleted",
                                                                  "prepareReadConflicts",
                                                                  "writeConflicts",
                                                                  "temporarilyUnavailableErrors",
                                                                  "dataThroughputLastSecond",
                                                                  "dataThroughputAverage",
                                                                  "numYield",
                                                                  "nreturned",
                                                                  "planCacheShapeHash",
                                                                  "planCacheKey",
                                                                  "queryShapeHash",
                                                                  "queryFramework",
                                                                  "planRanker",
                                                                  "locks",
                                                                  "authorization",
                                                                  "LDAPOperations",
                                                                  "flowControl",
                                                                  "writeConcern",
                                                                  "storage",
                                                                  "ok",
                                                                  "errMsg",
                                                                  "errName",
                                                                  "errCode",
                                                                  "responseLength",
                                                                  "protocol",
                                                                  "remoteOpWaitMillis",
                                                                  "cpuNanos",
                                                                  "millis",
                                                                  "durationMillis",
                                                                  "workingMillis",
                                                                  "planSummary",
                                                                  "planningTimeMicros",
                                                                  "estimatedCost",
                                                                  "estimatedCardinality",
                                                                  "totalOplogSlotDurationMicros",
                                                                  "execStats"};

    for (const auto& fieldName : allowedOpDebugFields) {
        auto filterExpr = BSON(fieldName << BSON("$exists" << true));

        ProfileFilterImpl profileFilter{filterExpr, expCtx};
        ASSERT_TRUE(profileFilter.dependsOn(fieldName))
            << "Profile filter failed to report dependency on " << fieldName;

        // Skip filtering on fields that require access to resource statistics - they are not mocked
        // out in this test.
        if (fieldName == "locks" || fieldName == "flowControl") {
            continue;
        }

        ASSERT_DOES_NOT_THROW(profileFilter.matches(opCtx, *opDebug, *curop));
    }
}

TEST_F(ProfileFilterTest, FilterOnLDAPOperationsField) {
    auto filterExpr = BSON("LDAPOperations" << BSON("$exists" << true));
    ProfileFilterImpl profileFilter{filterExpr, expCtx};

    ASSERT_TRUE(profileFilter.dependsOn("LDAPOperations"))
        << "Profile filter failed to report dependency on LDAPOperations";

    ASSERT_FALSE(profileFilter.matches(opCtx, *opDebug, *curop));

    // Record an LDAP bind so the stats report, then the filter should match.
    auto userAcquisitionStats = curop->getUserAcquisitionStats();
    auto* tickSource = opCtx->getServiceContext()->getTickSource();
    {
        UserAcquisitionStatsHandle handle(userAcquisitionStats.get(), tickSource, kBind);
    }
    ASSERT_TRUE(userAcquisitionStats->shouldReportLDAPOperationStats());

    ASSERT_TRUE(profileFilter.matches(opCtx, *opDebug, *curop));
}

TEST_F(ProfileFilterTest, FilterOnNestedField) {
    auto filterExpr = BSON("originatingCommand.pipeline" << BSON("$exists" << true));

    ProfileFilterImpl profileFilter{filterExpr, expCtx};
    ASSERT_TRUE(profileFilter.dependsOn("originatingCommand"))
        << "Profile filter failed to report dependency on originatingCommand";

    ASSERT_FALSE(profileFilter.matches(opCtx, *opDebug, *curop));

    {
        std::lock_guard<Client> lk(*opCtx->getClient());
        curop->setOriginatingCommand(lk, BSON("pipeline" << BSON_ARRAY(BSON("limit" << 1))));
    }

    ASSERT_TRUE(profileFilter.matches(opCtx, *opDebug, *curop));
}

TEST_F(ProfileFilterTest, FilterOnOptionalField) {
    auto filterExpr = BSON("replanReason" << "a good reason");

    ProfileFilterImpl profileFilter{filterExpr, expCtx};

    // Field doesn't exist.
    opDebug->replanReason = boost::none;
    ASSERT_FALSE(profileFilter.matches(opCtx, *opDebug, *curop));

    // Now set the field and make sure it matches.
    opDebug->replanReason = "a good reason";
    ASSERT_TRUE(profileFilter.matches(opCtx, *opDebug, *curop));
}

TEST_F(ProfileFilterTest, FilterDependsOnEnabledFeatureFlag) {
    // '$_testFeatureFlagLatest' is an expression that is permanently enabled in the latest FCV.
    auto filterExpr = fromjson(R"({
        $expr: {
            $gt: ['$nreturned', {'$_testFeatureFlagLatest' : 1}]
        }
    })");

    ProfileFilterImpl profileFilter{filterExpr, expCtx};
    ASSERT_TRUE(profileFilter.dependsOn("nreturned"));

    // '$_testFeatureFlagLatest' will always return 1. If 'nreturned' is 2, the filter should match.
    opDebug->getAdditiveMetrics().nreturned = 2;
    ASSERT_TRUE(profileFilter.matches(opCtx, *opDebug, *curop));

    // '$_testFeatureFlagLatest' will always return 1. If 'nreturned' is 0.1, the filter should not
    // match.
    opDebug->getAdditiveMetrics().nreturned = 0.1;
    ASSERT_FALSE(profileFilter.matches(opCtx, *opDebug, *curop));
}

TEST_F(ProfileFilterTest, FilterOnUnavailableField) {
    auto filterExpr = BSON("notAnOpDebugField" << "some value");
    ASSERT_THROWS_CODE(ProfileFilterImpl(filterExpr, expCtx), DBException, 4910200);
}

TEST_F(ProfileFilterTest, FilterThrowsException) {
    // Filter will fail to apply because of the type mismatch.
    auto filterExpr = fromjson(R"({
        $expr: {
            $eq: [{$add : [ 'hello', '$nreturned' ]}, 0]
        }
    })");

    ProfileFilterImpl profileFilter{filterExpr, expCtx};
    ASSERT_TRUE(profileFilter.dependsOn("nreturned"));

    ASSERT_FALSE(profileFilter.matches(opCtx, *opDebug, *curop));
}

}  // namespace mongo
