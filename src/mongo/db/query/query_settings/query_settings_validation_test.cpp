/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/serialization_context.h"

namespace mongo::query_settings {
namespace {

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
    auto representativeQueryInfo =
        createRepresentativeInfo(opCtx, representativeQuery, boost::none);
    ASSERT_THROWS_CODE(QuerySettingsService::get(opCtx).validateQueryCompatibleWithAnyQuerySettings(
                           representativeQueryInfo),
                       DBException,
                       errorCode);
}

void assertInvalidQueryAndQuerySettingsCombination(OperationContext* opCtx,
                                                   const BSONObj& representativeQuery,
                                                   const QuerySettings& querySettings,
                                                   size_t errorCode) {
    auto representativeQueryInfo =
        createRepresentativeInfo(opCtx, representativeQuery, boost::none);
    ASSERT_THROWS_CODE(QuerySettingsService::get(opCtx).validateQueryCompatibleWithQuerySettings(
                           representativeQueryInfo, querySettings),
                       DBException,
                       errorCode);
}

NamespaceSpec makeNamespace(StringData dbName, StringData collName) {
    NamespaceSpec ns;
    ns.setDb(
        DatabaseNameUtil::deserialize(boost::none, dbName, SerializationContext::stateDefault()));
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

    auto collectionlessNss = NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin);
    const stdx::unordered_set<StringData, StringMapHasher>
        collectionLessRejectionIncompatibleStages = {
            "$querySettings"_sd,
            "$listSessions"_sd,
            "$listSampledQueries"_sd,
            "$queryStats"_sd,
            "$currentOp"_sd,
            "$listCatalog"_sd,
            "$listLocalSessions"_sd,
        };

    for (auto&& stage : QuerySettingsService::getRejectionIncompatibleStages()) {
        // Avoid testing these stages, as they require more complex setup.
        if (stage == "$listLocalSessions" || stage == "$listSessions" ||
            stage == "$listSampledQueries") {
            continue;
        }

        auto aggCmdBSON = [&]() {
            if (collectionLessRejectionIncompatibleStages.contains(stage)) {
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
    ASSERT_THROWS_CODE(
        createRepresentativeInfo(expCtx->getOperationContext(), representativeQ, boost::none),
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

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsIndexHintsWithNoDbSpecified) {
    QuerySettings querySettings;
    NamespaceSpec ns;
    ns.setColl("collName"_sd);
    querySettings.setIndexHints({{IndexHintSpec(ns, {IndexHint("a")})}});
    service().simplifyQuerySettings(querySettings);
    ASSERT_THROWS_CODE(service().validateQuerySettings(querySettings), DBException, 8727500);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsIndexHintsWithNoCollSpecified) {
    QuerySettings querySettings;
    NamespaceSpec ns;
    ns.setDb(DatabaseNameUtil::deserialize(
        boost::none /* tenantId */, "dbName"_sd, SerializationContext::stateDefault()));
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
    std::string largeString(10 * 1024 * 1024, 'a');
    const BSONObj query = BSON("find" << "testColl" << "$db" << "testDB" << "filter"
                                      << BSON("$gt" << BSON(largeString << 1)));
    querySettings.setIndexHints({{
        IndexHintSpec(makeNamespace("testDB", "testColl"), {IndexHint(BSON(largeString << 1))}),
    }});
    QueryShapeConfiguration queryShapeConfiguration(query_shape::QueryShapeHash(), querySettings);
    queryShapeConfiguration.setRepresentativeQuery(query);
    config.queryShapeConfigurations = {queryShapeConfiguration};
    ASSERT_THROWS_CODE(service().validateQueryShapeConfigurations(config),
                       DBException,
                       ErrorCodes::BSONObjectTooLarge);
}

}  // namespace
}  // namespace mongo::query_settings
