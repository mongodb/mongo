// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/unittest/ensure_fcv.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/version/releases.h"

#include <array>
#include <string_view>
#include <utility>

namespace mongo::query_settings {
namespace {
using namespace std::literals::string_view_literals;

class QuerySettingsValidationTestFixture : public ServiceContextTest {
protected:
    QuerySettingsValidationTestFixture() {
        ShardingState::create(getServiceContext());
        expCtx = make_intrusive<ExpressionContextForTest>();
        query_settings::QuerySettingsService::initializeForTest(getServiceContext());
    }

    QuerySettingsService& service() {
        return QuerySettingsService::get(getServiceContext());
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
};

/*
 * Checks that query settings commands fail the validation with the 'errorCode' code.
 */
void assertInvalidQueryWithAnyQuerySettings(OperationContext* opCtx,
                                            const BSONObj& representativeQuery,
                                            size_t errorCode) {
    auto representativeQueryInfo = createRepresentativeInfo(opCtx, representativeQuery);
    ASSERT_THROWS_CODE(QuerySettingsService::get(opCtx).validateQueryCompatibleWithAnyQuerySettings(
                           representativeQueryInfo),
                       DBException,
                       errorCode);
}

void assertInvalidQueryAndQuerySettingsCombination(OperationContext* opCtx,
                                                   const BSONObj& representativeQuery,
                                                   const QuerySettings& querySettings,
                                                   size_t errorCode) {
    auto representativeQueryInfo = createRepresentativeInfo(opCtx, representativeQuery);
    ASSERT_THROWS_CODE(QuerySettingsService::get(opCtx).validateQueryCompatibleWithQuerySettings(
                           representativeQueryInfo, querySettings),
                       DBException,
                       errorCode);
}

NamespaceSpec makeNamespace(std::string_view dbName, std::string_view collName) {
    NamespaceSpec ns;
    ns.setDb(DatabaseNameUtil::deserialize(
        /* tenantId */ boost::none, dbName, SerializationContext::stateDefault()));
    ns.setColl(collName);
    return ns;
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsCannotBeAppliedOnIdHack) {
    const BSONObj representativeQ =
        fromjson("{find: 'someColl', filter: {_id: 123}, $db: 'testDb'}");
    QuerySettings querySettings;
    assertInvalidQueryWithAnyQuerySettings(expCtx->getOperationContext(), representativeQ, 7746606);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsCannotBeAppliedWithEncryptionInfo) {
    const BSONObj representativeQ = fromjson(R"(
        {
            aggregate: "order",
            $db: "testDB",
            pipeline: [{
                $lookup: {
                from: "inventory",
                localField: "item",
                foreignField: "sku",
                as: "inventory_docs"
                }
            }],
            encryptionInformation: {
                schema: {}
            }
        }
    )");
    assertInvalidQueryWithAnyQuerySettings(expCtx->getOperationContext(), representativeQ, 7746600);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsCannotBeAppliedOnEncryptedColl) {
    const BSONObj representativeQ = fromjson(R"(
        {find: "enxcol_.basic.esc", $db: "testDB"}
    )");
    assertInvalidQueryWithAnyQuerySettings(expCtx->getOperationContext(), representativeQ, 7746601);
}

TEST_F(QuerySettingsValidationTestFixture,
       QuerySettingsWithRejectCannotBeSetOnQueriesWithSystemStageAsFirstStage) {
    QuerySettings rejectionSettings;
    rejectionSettings.setReject(true);

    // $joinPlanCacheStats refuses to parse unless both of these are enabled.
    unittest::ServerParameterGuard joinOptGuard{"internalEnableJoinOptimization", true};
    unittest::ServerParameterGuard joinPlanCacheGuard{"internalEnableJoinPlanCache", true};

    auto collectionlessNss = NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin);

    // Stages that bypass query settings rejection. $listLocalSessions, $listSessions and
    // $listSampledQueries are omitted as they require more complex setup.
    for (auto&& [stage, isCollectionless] : std::array<std::pair<std::string_view, bool>, 9>{{
             {"$querySettings"sv, true},
             {"$planCacheStats"sv, false},
             {"$joinPlanCacheStats"sv, true},
             {"$collStats"sv, false},
             {"$indexStats"sv, false},
             {"$queryStats"sv, true},
             {"$currentOp"sv, true},
             {"$listCatalog"sv, true},
             {"$listSearchIndexes"sv, false},
         }}) {
        auto aggCmdBSON = [&]() {
            if (isCollectionless) {
                return BSON("aggregate" << collectionlessNss.coll() << "$db"
                                        << collectionlessNss.db_forTest() << "pipeline"
                                        << BSON_ARRAY(BSON(stage << BSONObj())));
            }

            return BSON("aggregate" << "testColl"
                                    << "$db"
                                    << "testColl"
                                    << "pipeline" << BSON_ARRAY(BSON(stage << BSONObj())));
        }();

        assertInvalidQueryAndQuerySettingsCombination(
            expCtx->getOperationContext(), aggCmdBSON, rejectionSettings, 8705200);
    }
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsCannotUseUuidAsNs) {
    auto s1 = "00000000-0000-4000-8000-000000000000";
    auto uuid1Res = UUID::parse(s1);
    ASSERT_OK(uuid1Res);
    ASSERT(UUID::isUUIDString(s1));
    const BSONObj representativeQ = BSON("find" << uuid1Res.getValue() << "$db"
                                                << "testDB"
                                                << "filter" << BSON("a" << BSONNULL));
    ASSERT_THROWS_CODE(createRepresentativeInfo(expCtx->getOperationContext(), representativeQ),
                       DBException,
                       7746605);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsIndicesCannotReferToSameColl) {
    const BSONObj representativeQ = fromjson(R"(
        {find: "testColl", $db: "testDB"}
    )");
    auto ns = makeNamespace("testDB", "testColl");
    QuerySettings querySettings;
    auto indexSpecA = IndexHintSpec(ns, {IndexHint("sku")});
    auto indexSpecB = IndexHintSpec(ns, {IndexHint("uks")});
    querySettings.setIndexHints({{indexSpecA, indexSpecB}});
    service().simplifyQuerySettings(querySettings);
    ASSERT_THROWS_CODE(service().validateQuerySettings(querySettings), DBException, 7746608);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsCannotBeEmpty) {
    QuerySettings querySettings;
    service().simplifyQuerySettings(querySettings);
    ASSERT_THROWS_CODE(service().validateQuerySettings(querySettings), DBException, 7746604);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsCannotHaveDefaultValues) {
    QuerySettings querySettings;
    querySettings.setReject(false);
    service().simplifyQuerySettings(querySettings);
    ASSERT_THROWS_CODE(service().validateQuerySettings(querySettings), DBException, 7746604);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsMaxTimeMSZeroIsNormalizedToUnset) {
    QuerySettings querySettings;
    querySettings.setMaxTimeMS(0);
    service().simplifyQuerySettings(querySettings);
    ASSERT_EQUALS(querySettings.getMaxTimeMS(), boost::none);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsIndexHintsWithNoDbSpecified) {
    QuerySettings querySettings;
    NamespaceSpec ns;
    ns.setColl("collName"sv);
    querySettings.setIndexHints({{IndexHintSpec(ns, {IndexHint("a")})}});
    service().simplifyQuerySettings(querySettings);
    ASSERT_THROWS_CODE(service().validateQuerySettings(querySettings), DBException, 8727500);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsIndexHintsWithNoCollSpecified) {
    QuerySettings querySettings;
    NamespaceSpec ns;
    ns.setDb(DatabaseNameUtil::deserialize(
        /* tenantId */ boost::none, "dbName"sv, SerializationContext::stateDefault()));
    querySettings.setIndexHints({{IndexHintSpec(ns, {IndexHint("a")})}});
    service().simplifyQuerySettings(querySettings);
    ASSERT_THROWS_CODE(service().validateQuerySettings(querySettings), DBException, 8727501);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsIndexHintsWithEmptyAllowedIndexes) {
    QuerySettings querySettings;
    auto ns = makeNamespace("testDB", "testColl");
    querySettings.setIndexHints({{IndexHintSpec(ns, {})}});
    service().simplifyQuerySettings(querySettings);
    ASSERT_EQUALS(querySettings.getIndexHints(), boost::none);
    ASSERT_THROWS_CODE(service().validateQuerySettings(querySettings), DBException, 7746604);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsIndexHintsWithAllEmptyAllowedIndexes) {
    QuerySettings querySettings;
    querySettings.setIndexHints({{
        IndexHintSpec(makeNamespace("testDB", "testColl1"), {}),
        IndexHintSpec(makeNamespace("testDB", "testColl2"), {}),
        IndexHintSpec(makeNamespace("testDB", "testColl3"), {}),
    }});
    service().simplifyQuerySettings(querySettings);
    ASSERT_EQUALS(querySettings.getIndexHints(), boost::none);
    ASSERT_THROWS_CODE(service().validateQuerySettings(querySettings), DBException, 7746604);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsIndexHintsWithSomeEmptyAllowedIndexes) {
    QuerySettings querySettings;
    querySettings.setIndexHints({{
        IndexHintSpec(makeNamespace("testDB", "testColl1"), {}),
        IndexHintSpec(makeNamespace("testDB", "testColl2"), {IndexHint("a")}),
    }});
    const auto expectedIndexHintSpec =
        IndexHintSpec(makeNamespace("testDB", "testColl2"), {IndexHint("a")});
    service().simplifyQuerySettings(querySettings);
    const auto simplifiedIndexHints = querySettings.getIndexHints();

    ASSERT_NE(simplifiedIndexHints, boost::none);
    const auto& indexHintsList = *simplifiedIndexHints;
    ASSERT_EQ(indexHintsList.size(), 1);
    const auto& actualIndexHintSpec = indexHintsList[0];
    ASSERT_BSONOBJ_EQ(expectedIndexHintSpec.toBSON(), actualIndexHintSpec.toBSON());
    ASSERT_DOES_NOT_THROW(service().validateQuerySettings(querySettings));
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsIndexHintsWithEmptyKeyPattern) {
    QuerySettings querySettings;
    querySettings.setIndexHints({{
        IndexHintSpec(makeNamespace("testDB", "testColl"), {IndexHint(BSONObj{})}),
    }});
    service().simplifyQuerySettings(querySettings);
    ASSERT_THROWS_CODE(service().validateQuerySettings(querySettings), DBException, 9646000);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsIndexHintsWithInvalidKeyPattern) {
    QuerySettings querySettings;
    querySettings.setIndexHints({{
        IndexHintSpec(makeNamespace("testDB", "testColl"),
                      {IndexHint(BSON("a" << 1 << "b"
                                          << "some-string"))}),
    }});
    service().simplifyQuerySettings(querySettings);
    ASSERT_THROWS_CODE(service().validateQuerySettings(querySettings), DBException, 9646001);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsIndexHintsWithInvalidNaturalHint) {
    QuerySettings querySettings;
    querySettings.setIndexHints({{
        IndexHintSpec(makeNamespace("testDB", "testColl"),
                      {IndexHint(BSON("$natural" << 1 << "b" << 2))}),
    }});
    service().simplifyQuerySettings(querySettings);
    ASSERT_THROWS_CODE(service().validateQuerySettings(querySettings), DBException, 9646001);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsIndexHintsWithInvalidNaturalHintInverse) {
    QuerySettings querySettings;
    querySettings.setIndexHints({{
        IndexHintSpec(makeNamespace("testDB", "testColl"),
                      {IndexHint(BSON("b" << 2 << "$natural" << 1))}),
    }});
    service().simplifyQuerySettings(querySettings);
    ASSERT_THROWS_CODE(service().validateQuerySettings(querySettings), DBException, 9646001);
}

TEST_F(QuerySettingsValidationTestFixture,
       QueryShapeConfigurationsValidationFailsOnBSONObjectTooLarge) {
    QueryShapeConfigurationsWithTimestamp config;
    QuerySettings querySettings;
    // Two ~10MB hint keys (a single BSON object cannot exceed BSONObjMaxUserSize = 16MB, so the
    // overflow must come from two separate objects) push the serialized configuration past the
    // 16MB limit.
    std::string largeString1(10 * 1024 * 1024, 'a');
    std::string largeString2(10 * 1024 * 1024, 'b');
    querySettings.setIndexHints({{
        IndexHintSpec(makeNamespace("testDB", "testColl"),
                      {IndexHint(BSON(largeString1 << 1)), IndexHint(BSON(largeString2 << 1))}),
    }});
    QueryShapeConfiguration queryShapeConfiguration(query_shape::QueryShapeHash(), querySettings);
    config.queryShapeConfigurations = {queryShapeConfiguration};
    ASSERT_THROWS_CODE(service().validateQueryShapeConfigurations(config),
                       DBException,
                       ErrorCodes::BSONObjectTooLarge);
}

TEST_F(QuerySettingsValidationTestFixture, SimplifyQuerySettingsClearsEmptyKnobs) {
    QuerySettings settings;
    settings.setQueryKnobs(QuerySettingsKnobOverrides::fromBSON(BSONObj{}));
    ASSERT_TRUE(settings.getQueryKnobs().has_value());
    ASSERT_TRUE(settings.getQueryKnobs()->empty());
    service().simplifyQuerySettings(settings);
    ASSERT_FALSE(settings.getQueryKnobs().has_value());
}

TEST_F(QuerySettingsValidationTestFixture, SimplifyQuerySettingsStripsKnobDeletions) {
    QuerySettings settings;
    settings.setQueryKnobs(QuerySettingsKnobOverrides::fromBSON(
        BSON("testIntKnobWire" << 7 << "testBoolKnobWire" << BSONNULL)));
    service().simplifyQuerySettings(settings);
    // The removal sentinel is stripped; the real knob survives.
    ASSERT_TRUE(settings.getQueryKnobs().has_value());
    ASSERT_BSONOBJ_EQ(settings.getQueryKnobs()->toBSON(), BSON("testIntKnobWire" << 7));
}

TEST_F(QuerySettingsValidationTestFixture, SimplifyQuerySettingsClearsKnobsThatAreAllDeletions) {
    QuerySettings settings;
    settings.setQueryKnobs(
        QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << BSONNULL)));
    service().simplifyQuerySettings(settings);
    ASSERT_FALSE(settings.getQueryKnobs().has_value());
}

TEST_F(QuerySettingsValidationTestFixture, ValidateRejectsDuplicateKnobs) {
    QuerySettings settings;
    settings.setQueryKnobs(QuerySettingsKnobOverrides::fromBSON(
        BSON("testIntKnobWire" << 5 << "testIntKnobWire" << 10)));
    ASSERT_THROWS_CODE(service().validateQuerySettings(settings), DBException, 12366201);
}

// FCV validation of user-provided knob overrides: knobs whose minFcv exceeds the current FCV are
// rejected (12955401).

TEST_F(QuerySettingsValidationTestFixture, ValidateQueryKnobsAcceptsSupportedKnobsOnLatestFcv) {
    QueryFCVEnvironmentForTest::setUp();
    // (Generic FCV reference): FCV-gated query knob validation test.
    unittest::EnsureFCV fcv(multiversion::GenericFCV::kLatest);

    QuerySettings settings;
    settings.setQueryKnobs(QuerySettingsKnobOverrides::fromBSON(
        BSON("testIntKnobWire" << 5 << "testLowFcvKnobWire" << 5)));
    service().validateQueryKnobs(settings);
}

// A user write with a knob that is being downgraded away must be rejected during the whole FCV
// transition; otherwise it can be persisted behind the downgrade migration's stripping pass and
// survive at the downgraded FCV.
TEST_F(QuerySettingsValidationTestFixture, ValidateQueryKnobsRejectsKnobsMidDowngrade) {
    GTEST_SKIP() << "Test doesn't support FCV 9.1 (TODO: SERVER-133009)";

    QueryFCVEnvironmentForTest::setUp();
    // (Generic FCV reference): FCV-gated query knob validation test.
    unittest::EnsureFCV fcv(multiversion::GenericFCV::kDowngradingFromLatestToLastLTS);

    QuerySettings settings;
    settings.setQueryKnobs(QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 5)));
    ASSERT_THROWS(service().validateQueryKnobs(settings), DBException);
}

TEST_F(QuerySettingsValidationTestFixture, ValidateQueryKnobsRejectsKnobsOnLowerStableFcv) {
    GTEST_SKIP() << "Test doesn't support FCV 9.1 (TODO: SERVER-133009)";

    QueryFCVEnvironmentForTest::setUp();
    // (Generic FCV reference): FCV-gated query knob validation test.
    unittest::EnsureFCV fcv(multiversion::GenericFCV::kLastLTS);

    QuerySettings settings;
    settings.setQueryKnobs(QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 5)));
    ASSERT_THROWS(service().validateQueryKnobs(settings), DBException);
}

TEST_F(QuerySettingsValidationTestFixture, ValidateRejectsKnobOverrideParseErrors) {
    QuerySettings settings;
    settings.setQueryKnobs(QuerySettingsKnobOverrides::fromBSON(BSON("totallyUnknownKnob" << 1)));
    ASSERT_THROWS_CODE(service().validateQuerySettings(settings), DBException, 12194501);
}

}  // namespace
}  // namespace mongo::query_settings
