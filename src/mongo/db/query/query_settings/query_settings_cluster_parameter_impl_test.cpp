/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_settings/query_settings_cluster_parameter_gen.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/serialization_context.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::query_settings {
static bool operator==(const QueryShapeConfiguration& lhs, const QueryShapeConfiguration& rhs) {
    return SimpleBSONObjComparator::kInstance.compare(lhs.toBSON(), rhs.toBSON()) == 0;
}

static bool operator==(const QueryShapeConfigurationsWithTimestamp& lhs,
                       const QueryShapeConfigurationsWithTimestamp& rhs) {
    return lhs.clusterParameterTime == rhs.clusterParameterTime &&
        lhs.queryShapeConfigurations == rhs.queryShapeConfigurations;
}

namespace {
static auto const kSerializationContext =
    SerializationContext{SerializationContext::Source::Command,
                         SerializationContext::CallerType::Request,
                         SerializationContext::Prefix::ExcludePrefix};

auto makeDbName(StringData dbName) {
    return DatabaseNameUtil::deserialize(
        boost::none /*tenantId=*/, dbName, SerializationContext::stateDefault());
}

NamespaceSpec makeNsSpec(StringData collName) {
    NamespaceSpec ns;
    ns.setDb(makeDbName("testDbA"));
    ns.setColl(collName);
    return ns;
}

QuerySettings makeQuerySettings(const IndexHintSpecs& indexHints, bool setFramework = false) {
    QuerySettings settings;
    if (!indexHints.empty()) {
        settings.setIndexHints(indexHints);
    }
    if (setFramework) {
        settings.setQueryFramework(mongo::QueryFrameworkControlEnum::kTrySbeEngine);
    }
    return settings;
}

class QuerySettingsClusterParameterTest : public ServiceContextTest {
public:
    void setUp() final {
        _opCtx = cc().makeOperationContext();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    QuerySettingsService& service() {
        return QuerySettingsService::get(opCtx());
    }

    QueryShapeConfiguration makeQueryShapeConfiguration(
        const BSONObj& cmdBSON,
        const QuerySettings& querySettings,
        boost::optional<TenantId> tenantId = boost::none) {
        auto queryShapeHash = createRepresentativeInfo(opCtx(), cmdBSON, tenantId).queryShapeHash;
        QueryShapeConfiguration config(queryShapeHash, querySettings);
        config.setRepresentativeQuery(cmdBSON);
        return config;
    }

    BSONObj makeQuerySettingsClusterParameter(const QueryShapeConfigurationsWithTimestamp& config) {
        BSONArrayBuilder bob;
        for (const auto& c : config.queryShapeConfigurations) {
            bob.append(c.toBSON());
        }

        return BSON("_id" << QuerySettingsService::getQuerySettingsClusterParameterName()
                          << QuerySettingsClusterParameterValue::kSettingsArrayFieldName
                          << bob.arr()
                          << QuerySettingsClusterParameterValue::kClusterParameterTimeFieldName
                          << config.clusterParameterTime.asTimestamp());
    }

    LogicalTime nextClusterParameterTime() {
        auto clusterParameterTime = service().getClusterParameterTime(/* tenantId */ boost::none);
        clusterParameterTime.addTicks(1);
        return clusterParameterTime;
    }

    void assertTransformInvalidQuerySettings(
        const std::list<std::pair<const IndexHintSpecs, const IndexHintSpecs>>&
            listOfIndexHintSpecs) {
        auto sp = std::make_unique<QuerySettingsClusterParameter>(
            QuerySettingsService::getQuerySettingsClusterParameterName(),
            ServerParameterType::kClusterWide);
        std::vector<QueryShapeConfiguration> expectedQueryShapeConfigurations;
        size_t collIndex = 0;

        // Create the 'querySettingsClusterParamValue' as BSON.
        QueryShapeConfigurationsWithTimestamp configsWithTs;
        configsWithTs.clusterParameterTime = nextClusterParameterTime();
        for (const auto& [initialIndexHintSpecs, expectedIndexHintSpecs] : listOfIndexHintSpecs) {
            QueryInstance representativeQuery =
                BSON("find" << "coll_" + std::to_string(collIndex) << "$db"
                            << "foo");
            configsWithTs.queryShapeConfigurations.emplace_back(makeQueryShapeConfiguration(
                representativeQuery, makeQuerySettings(initialIndexHintSpecs)));

            if (!expectedIndexHintSpecs.empty()) {
                expectedQueryShapeConfigurations.emplace_back(makeQueryShapeConfiguration(
                    representativeQuery, makeQuerySettings(expectedIndexHintSpecs)));
            }
            collIndex++;
        }

        // Set the new 'clusterParamValue' for "querySettings" cluster parameter.
        const auto clusterParamValue = makeQuerySettingsClusterParameter(configsWithTs);
        ASSERT_OK(
            sp->set(BSON("" << clusterParamValue).firstElement(), boost::none /* tenantId */));

        // Assert that parsing after transforming invalid settings (if any) works.
        ASSERT_EQ(expectedQueryShapeConfigurations,
                  service()
                      .getAllQueryShapeConfigurations(/* tenantId */ boost::none)
                      .queryShapeConfigurations);
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

/**
 * Tests that set/reset methods over the QuerySettings cluster parameter correctly populate the data
 * in QuerySettingsService, as well as that QuerySettings data is correctly populated when
 * serializing cluster parameter.
 */
TEST_F(QuerySettingsClusterParameterTest, QuerySettingsClusterParameterSetReset) {
    boost::optional<TenantId> tenantId;
    auto sp = std::make_unique<QuerySettingsClusterParameter>(
        QuerySettingsService::getQuerySettingsClusterParameterName(),
        ServerParameterType::kClusterWide);

    // Ensure no clusterParameterTime is specified for "querySettings" cluster parameter, if no
    // settings are specified.
    ASSERT_EQ(sp->getClusterParameterTime(tenantId), LogicalTime());
    ASSERT_EQ(service().getClusterParameterTime(tenantId), LogicalTime());
    {
        BSONObjBuilder bob;
        sp->append(
            opCtx(), &bob, QuerySettingsService::getQuerySettingsClusterParameterName(), tenantId);
        ASSERT_BSONOBJ_EQ(
            bob.done(), makeQuerySettingsClusterParameter(QueryShapeConfigurationsWithTimestamp()));
    }

    // Set query shape configuration.
    QuerySettings settings;
    settings.setQueryFramework(QueryFrameworkControlEnum::kTrySbeEngine);
    QueryInstance findCmdBSON = BSON("find" << "exampleColl"
                                            << "$db"
                                            << "foo");
    auto config = makeQueryShapeConfiguration(findCmdBSON, settings);
    LogicalTime clusterParameterTime(Timestamp(1, 2));
    QueryShapeConfigurationsWithTimestamp configsWithTs{{config}, clusterParameterTime};

    // Ensure that after parameter is set, the query shape configurations are present in the
    // QuerySettingsService.
    const auto clusterParamValue = makeQuerySettingsClusterParameter(configsWithTs);
    ASSERT_OK(sp->set(BSON("" << clusterParamValue).firstElement(), tenantId));
    ASSERT_EQ(service().getAllQueryShapeConfigurations(tenantId), configsWithTs);
    ASSERT_EQ(sp->getClusterParameterTime(tenantId), clusterParameterTime);
    ASSERT_EQ(service().getClusterParameterTime(tenantId), clusterParameterTime);

    // Ensure the serialized parameter value contains 'settingsArray' with 'config' as value as well
    // parameter id and clusterParameterTime.
    {
        BSONObjBuilder bob;
        sp->append(
            opCtx(), &bob, QuerySettingsService::getQuerySettingsClusterParameterName(), tenantId);
        ASSERT_BSONOBJ_EQ(bob.done(), clusterParamValue);
    }

    // Ensure that after parameter is reset, no query shape configurations are present in the
    // QuerySettingsService and clusterParameterTime is reset.
    ASSERT_OK(sp->reset(tenantId));
    ASSERT(service().getAllQueryShapeConfigurations(tenantId).queryShapeConfigurations.empty());
    ASSERT_EQ(sp->getClusterParameterTime(tenantId), LogicalTime());
    ASSERT_EQ(service().getClusterParameterTime(tenantId), LogicalTime());
    {
        BSONObjBuilder bob;
        sp->append(
            opCtx(), &bob, QuerySettingsService::getQuerySettingsClusterParameterName(), tenantId);
        ASSERT_BSONOBJ_EQ(
            bob.done(), makeQuerySettingsClusterParameter(QueryShapeConfigurationsWithTimestamp()));
    }
}

/**
 * Tests that query settings with invalid key-pattern index hints leads to no query settings after
 * sanitization.
 */
TEST_F(QuerySettingsClusterParameterTest,
       QuerySettingsClusterParameterSetInvalidQSWithSanitization) {
    boost::optional<TenantId> tenantId;
    auto sp = std::make_unique<QuerySettingsClusterParameter>(
        QuerySettingsService::getQuerySettingsClusterParameterName(),
        ServerParameterType::kClusterWide);
    IndexHintSpecs initialIndexHintSpec{
        IndexHintSpec(makeNsSpec("testCollC"_sd),
                      {IndexHint(BSON("a" << 1 << "$natural" << 1)), IndexHint(BSONObj{})}),
        IndexHintSpec(makeNsSpec("testCollD"_sd),
                      {IndexHint(BSON("b" << "some-string"
                                          << "a" << 1))})};

    QuerySettings querySettings;
    querySettings.setIndexHints(initialIndexHintSpec);
    ASSERT_THROWS_CODE(service().validateQuerySettings(querySettings), DBException, 9646001);

    // Create the 'querySettingsClusterParamValue' as BSON.
    const auto findCmdBSON = BSON("find" << "bar"
                                         << "$db"
                                         << "foo");
    const auto config = makeQueryShapeConfiguration(findCmdBSON, querySettings);

    // Assert that parsing after transforming invalid settings (if any) works.
    const auto clusterParamValue =
        makeQuerySettingsClusterParameter({{config}, LogicalTime(Timestamp(3, 4))});
    ASSERT_OK(sp->set(BSON("" << clusterParamValue).firstElement(), tenantId));
    ASSERT(service().getAllQueryShapeConfigurations(tenantId).queryShapeConfigurations.empty());
}

/**
 * Sets the value of QuerySettingsClusterParameter and asserts the resulting index hint spec is
 * sanitized.
 */
TEST_F(QuerySettingsClusterParameterTest, SetValidClusterParameterAndAssertResultIsSanitized) {
    IndexHintSpecs indexHintSpec{IndexHintSpec(
        makeNsSpec("testCollA"), {IndexHint(BSON("a" << 1)), IndexHint(BSON("b" << -1.0))})};
    assertTransformInvalidQuerySettings({{indexHintSpec, indexHintSpec}});
}

/**
 * Same as above test, but assert that the validation would fail with invalid key-pattern indexes.
 */
TEST_F(QuerySettingsClusterParameterTest, SetClusterParameterAndAssertResultIsSanitized) {
    IndexHintSpecs initialIndexHintSpec{
        IndexHintSpec(makeNsSpec("testCollA"_sd),
                      {IndexHint(BSON("a" << 1.0)), IndexHint(BSON("b" << "-1.0"))}),
        IndexHintSpec(makeNsSpec("testCollB"_sd),
                      {IndexHint(BSON("a" << 2)), IndexHint(BSONObj{})})};
    IndexHintSpecs expectedIndexHintSpec{
        IndexHintSpec(makeNsSpec("testCollA"_sd), {IndexHint(BSON("a" << 1))}),
        IndexHintSpec(makeNsSpec("testCollB"_sd), {IndexHint(BSON("a" << 2))})};

    ASSERT_THROWS_CODE(service().validateQuerySettings(makeQuerySettings(initialIndexHintSpec)),
                       DBException,
                       9646001);

    assertTransformInvalidQuerySettings({{initialIndexHintSpec, expectedIndexHintSpec}});
}

/**
 * Same as above test, but with multiple QuerySettings set.
 */
TEST_F(QuerySettingsClusterParameterTest, SetClusterParameterAndAssertResultIsSanitizedMultipleQS) {
    // First pair of index hints.
    IndexHintSpecs initialIndexHintSpec1{
        IndexHintSpec(makeNsSpec("testCollA"_sd),
                      {IndexHint(BSON("a" << 2)), IndexHint(BSONObj{})}),
        IndexHintSpec(makeNsSpec("testCollB"_sd),
                      {IndexHint(BSON("a" << 1.0)),
                       IndexHint(BSON("b" << 3)),
                       IndexHint("a_1"),
                       IndexHint(NaturalOrderHint(NaturalOrderHint::Direction::kForward))})};
    IndexHintSpecs expectedIndexHintSpec1{
        IndexHintSpec(makeNsSpec("testCollA"_sd), {IndexHint(BSON("a" << 2.0))}),
        IndexHintSpec(makeNsSpec("testCollB"_sd),
                      {IndexHint(BSON("a" << 1.0)),
                       IndexHint(BSON("b" << 3)),
                       IndexHint("a_1"),
                       IndexHint(NaturalOrderHint(NaturalOrderHint::Direction::kForward))})};
    // Second pair of index hints.
    IndexHintSpecs initialIndexHintSpec2{
        IndexHintSpec(makeNsSpec("testCollC"_sd),
                      {IndexHint(BSON("a" << 1 << "$natural" << 1)), IndexHint(BSONObj{})}),
        IndexHintSpec(makeNsSpec("testCollD"_sd),
                      {IndexHint(BSON("b" << "some-string"
                                          << "a" << 1)),
                       IndexHint(BSONObj{})})};

    ASSERT_THROWS_CODE(service().validateQuerySettings(makeQuerySettings(initialIndexHintSpec1)),
                       DBException,
                       9646000);

    ASSERT_THROWS_CODE(service().validateQuerySettings(makeQuerySettings(initialIndexHintSpec2)),
                       DBException,
                       9646001);

    assertTransformInvalidQuerySettings(
        {{initialIndexHintSpec1, expectedIndexHintSpec1}, {initialIndexHintSpec2, {}}});
}
}  // namespace
}  // namespace mongo::query_settings
