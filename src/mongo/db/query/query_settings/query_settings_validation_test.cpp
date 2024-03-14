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
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/query_settings/query_settings_utils.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/s/sharding_state.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/serialization_context.h"

namespace mongo::query_settings {
namespace {

class QuerySettingsValidationTestFixture : public ServiceContextTest {
protected:
    QuerySettingsValidationTestFixture() {
        ShardingState::create(getServiceContext());
        expCtx = make_intrusive<ExpressionContextForTest>();
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
};


/*
 * Checks that query settings commands fail the validation with the 'errorCode' code.
 */
void assertInvalidQueryInstance(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                const BSONObj& representativeQuery,
                                size_t errorCode) {
    auto representativeQueryInfo =
        createRepresentativeInfo(representativeQuery, expCtx->opCtx, boost::none);
    ASSERT_THROWS_CODE(
        utils::validateRepresentativeQuery(representativeQueryInfo), DBException, errorCode);
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
    assertInvalidQueryInstance(expCtx, representativeQ, 7746606);
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
    assertInvalidQueryInstance(expCtx, representativeQ, 7746600);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsCannotBeAppliedOnEncryptedColl) {
    const BSONObj representativeQ = fromjson(R"(
        {find: "enxcol_.basic.esc", $db: "testDB"}
    )");
    assertInvalidQueryInstance(expCtx, representativeQ, 7746601);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsCannotUseUuidAsNs) {
    auto s1 = "00000000-0000-4000-8000-000000000000";
    auto uuid1Res = UUID::parse(s1);
    ASSERT_OK(uuid1Res);
    ASSERT(UUID::isUUIDString(s1));
    const BSONObj representativeQ = BSON("find" << uuid1Res.getValue() << "$db"
                                                << "testDB"
                                                << "filter" << BSON("a" << BSONNULL));
    ASSERT_THROWS_CODE(createRepresentativeInfo(representativeQ, expCtx->opCtx, boost::none),
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
    querySettings.setIndexHints({{std::vector{indexSpecA, indexSpecB}}});
    ASSERT_THROWS_CODE(utils::validateQuerySettings(querySettings), DBException, 7746608);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsCannotBeEmpty) {
    QuerySettings querySettings;
    ASSERT_THROWS_CODE(utils::validateQuerySettings(querySettings), DBException, 7746604);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsCannotHaveDefaultValues) {
    QuerySettings querySettings;
    querySettings.setReject(false);
    ASSERT_THROWS_CODE(utils::validateQuerySettings(querySettings), DBException, 7746604);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsIndexHintsWithNoDbSpecified) {
    QuerySettings querySettings;
    NamespaceSpec ns;
    ns.setColl("collName"_sd);
    querySettings.setIndexHints({{IndexHintSpec(ns, {IndexHint("a")})}});
    ASSERT_THROWS_CODE(utils::validateQuerySettings(querySettings), DBException, 8727500);
}

TEST_F(QuerySettingsValidationTestFixture, QuerySettingsIndexHintsWithNoCollSpecified) {
    QuerySettings querySettings;
    NamespaceSpec ns;
    ns.setDb(DatabaseNameUtil::deserialize(
        boost::none /* tenantId */, "dbName"_sd, SerializationContext::stateDefault()));
    querySettings.setIndexHints({{IndexHintSpec(ns, {IndexHint("a")})}});
    ASSERT_THROWS_CODE(utils::validateQuerySettings(querySettings), DBException, 8727501);
}
}  // namespace
}  // namespace mongo::query_settings
