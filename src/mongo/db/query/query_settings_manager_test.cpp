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

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <iterator>
#include <memory>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/index_hint.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_settings_cluster_parameter_gen.h"
#include "mongo/db/query/query_settings_gen.h"
#include "mongo/db/query/query_settings_manager.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/serialization_context.h"

namespace mongo::query_settings {
namespace {
QueryShapeConfiguration makeQueryShapeConfiguration(
    const QuerySettings& settings,
    QueryInstance query,
    boost::intrusive_ptr<ExpressionContext> expCtx) {
    auto findCommandRequest = std::make_unique<FindCommandRequest>(
        FindCommandRequest::parse(IDLParserContext("findCommandRequest"), query));
    auto parsedFindCommandResult =
        parsed_find_command::parse(expCtx, std::move(findCommandRequest));
    ASSERT_OK(parsedFindCommandResult);
    return QueryShapeConfiguration(
        std::make_unique<query_shape::FindCmdShape>(*parsedFindCommandResult.getValue(), expCtx)
            ->sha256Hash(expCtx->opCtx),
        settings,
        query);
}

// QueryShapeConfiguration is not comparable, therefore comparing the corresponding
// BSONObj encoding.
void assertQueryShapeConfigurationsEquals(
    const std::vector<QueryShapeConfiguration>& expectedQueryShapeConfigurations,
    const std::vector<QueryShapeConfiguration>& actualQueryShapeConfigurations) {
    std::vector<BSONObj> lhs, rhs;
    std::transform(expectedQueryShapeConfigurations.begin(),
                   expectedQueryShapeConfigurations.end(),
                   std::back_inserter(lhs),
                   [](auto x) { return x.toBSON(); });
    std::transform(actualQueryShapeConfigurations.begin(),
                   actualQueryShapeConfigurations.end(),
                   std::back_inserter(rhs),
                   [](auto x) { return x.toBSON(); });
    std::sort(lhs.begin(), lhs.end(), SimpleBSONObjComparator::kInstance.makeLessThan());
    std::sort(rhs.begin(), rhs.end(), SimpleBSONObjComparator::kInstance.makeLessThan());
    ASSERT(std::equal(
        lhs.begin(), lhs.end(), rhs.begin(), SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

}  // namespace

class QuerySettingsManagerTest : public ServiceContextTest {
public:
    static std::vector<QueryShapeConfiguration> getExampleQueryShapeConfigurations(
        boost::intrusive_ptr<ExpressionContext> expCtx) {
        QuerySettings settings;
        settings.setQueryEngineVersion(QueryEngineVersionEnum::kV2);
        settings.setIndexHints({{IndexHintSpec({IndexHint("a_1")})}});
        QueryInstance queryA = BSON("find"
                                    << "exampleColl"
                                    << "$db"
                                    << "foo"
                                    << "filter" << BSON("a" << 2));
        QueryInstance queryB = BSON("find"
                                    << "exampleColl"
                                    << "$db"
                                    << "foo"
                                    << "filter" << BSON("a" << BSONNULL));
        return {makeQueryShapeConfiguration(settings, queryA, expCtx),
                makeQueryShapeConfiguration(settings, queryB, expCtx)};
    }

    void setUp() final {
        QuerySettingsManager::create(getServiceContext());

        _opCtx = cc().makeOperationContext();
        _expCtx = boost::intrusive_ptr{new ExpressionContextForTest(opCtx())};
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    boost::intrusive_ptr<ExpressionContext> expCtx() {
        return _expCtx;
    }

    QuerySettingsManager& manager() {
        return QuerySettingsManager::get(opCtx());
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(QuerySettingsManagerTest, QuerySettingsClusterParameterSerialization) {
    // Set query shape configuration.
    QuerySettings settings;
    settings.setQueryEngineVersion(QueryEngineVersionEnum::kV2);
    QueryInstance query = BSON("find"
                               << "exampleColl"
                               << "$db"
                               << "foo");
    auto config = makeQueryShapeConfiguration(settings, query, expCtx());
    LogicalTime clusterParameterTime(Timestamp(113, 59));
    TenantId tenantId(OID::gen());
    manager().setQueryShapeConfigurations(opCtx(), {config}, clusterParameterTime, tenantId);

    // Ensure the serialized parameter value contains 'settingsArray' with 'config' as value as well
    // parameter id and clusterParameterTime.
    BSONObjBuilder bob;
    manager().appendQuerySettingsClusterParameterValue(opCtx(), &bob, tenantId);
    ASSERT_BSONOBJ_EQ(
        bob.done(),
        BSON("_id" << QuerySettingsManager::kQuerySettingsClusterParameterName
                   << QuerySettingsClusterParameterValue::kSettingsArrayFieldName
                   << BSON_ARRAY(config.toBSON())
                   << QuerySettingsClusterParameterValue::kClusterParameterTimeFieldName
                   << clusterParameterTime.asTimestamp()));
}

TEST_F(QuerySettingsManagerTest, QuerySettingsSetAndReset) {
    auto configs = getExampleQueryShapeConfigurations(expCtx());
    auto firstConfig = configs[0], secondConfig = configs[1];
    LogicalTime firstWriteTime(Timestamp(1, 0)), secondWriteTime(Timestamp(2, 0));
    TenantId tenantId(OID::fromTerm(1)), otherTenantId(OID::fromTerm(2));

    // Ensure that the maintained in-memory query shape configurations equal to the
    // configurations specified in the parameter for both tenants.
    manager().setQueryShapeConfigurations(opCtx(), {firstConfig}, firstWriteTime, tenantId);
    manager().setQueryShapeConfigurations(opCtx(), {firstConfig}, firstWriteTime, otherTenantId);
    assertQueryShapeConfigurationsEquals(
        {firstConfig}, manager().getAllQueryShapeConfigurations(opCtx(), tenantId));
    assertQueryShapeConfigurationsEquals(
        {firstConfig}, manager().getAllQueryShapeConfigurations(opCtx(), otherTenantId));
    ASSERT_EQ(manager().getClusterParameterTime(opCtx(), tenantId), firstWriteTime);
    ASSERT_EQ(manager().getClusterParameterTime(opCtx(), otherTenantId), firstWriteTime);

    // Update query settings for tenant with 'tenantId'. Ensure its query shape configurations and
    // parameter cluster time are updated accordingly.
    manager().setQueryShapeConfigurations(opCtx(), {secondConfig}, secondWriteTime, tenantId);
    assertQueryShapeConfigurationsEquals(
        {secondConfig}, manager().getAllQueryShapeConfigurations(opCtx(), tenantId));
    assertQueryShapeConfigurationsEquals(
        {firstConfig}, manager().getAllQueryShapeConfigurations(opCtx(), otherTenantId));
    ASSERT_EQ(manager().getClusterParameterTime(opCtx(), tenantId), secondWriteTime);
    ASSERT_EQ(manager().getClusterParameterTime(opCtx(), otherTenantId), firstWriteTime);

    // Reset the parameter value and ensure that the in-memory storage is cleared for tenant with
    // 'tenantId'. QueryShapeConfigurations for tenant with 'otherTenantId' must not be affected..
    manager().removeAllQueryShapeConfigurations(opCtx(), tenantId);
    assertQueryShapeConfigurationsEquals(
        {}, manager().getAllQueryShapeConfigurations(opCtx(), tenantId));
    assertQueryShapeConfigurationsEquals(
        {firstConfig}, manager().getAllQueryShapeConfigurations(opCtx(), otherTenantId));
    ASSERT_EQ(manager().getClusterParameterTime(opCtx(), tenantId), LogicalTime::kUninitialized);
    ASSERT_EQ(manager().getClusterParameterTime(opCtx(), otherTenantId), firstWriteTime);
}

TEST_F(QuerySettingsManagerTest, QuerySettingsLookup) {
    auto configs = getExampleQueryShapeConfigurations(expCtx());
    TenantId tenantId(OID::fromTerm(1)), otherTenantId(OID::fromTerm(2));
    manager().setQueryShapeConfigurations(
        opCtx(), std::vector<QueryShapeConfiguration>(configs), LogicalTime(), tenantId);

    // Ensure QuerySettingsManager returns boost::none when QuerySettings are not found.
    ASSERT_FALSE(manager().getQuerySettingsForQueryShapeHash(
        opCtx(), query_shape::QueryShapeHash(), tenantId));

    // Ensure QuerySettingsManager returns a valid (QuerySettings, QueryInstance) pair on lookup.
    auto querySettingsPair = manager().getQuerySettingsForQueryShapeHash(
        opCtx(), configs[1].getQueryShapeHash(), tenantId);
    ASSERT(querySettingsPair.has_value());
    auto [settings, queryInstance] = *querySettingsPair;
    ASSERT_BSONOBJ_EQ(settings.toBSON(), configs[1].getSettings().toBSON());
    ASSERT_BSONOBJ_EQ(queryInstance, configs[1].getRepresentativeQuery());

    // Ensure QuerySettingsManager returns boost::none when no QuerySettings are set for the given
    // tenant, however, exists for other tenant.
    ASSERT_FALSE(manager()
                     .getQuerySettingsForQueryShapeHash(
                         opCtx(), query_shape::QueryShapeHash(), otherTenantId)
                     .has_value());
}

TEST(QuerySettingsClusterParameter, ParameterValidation) {
    // Ensure validation fails for invalid input.
    TenantId tenantId(OID::gen());
    QuerySettingsClusterParameter querySettingsParameter(
        QuerySettingsManager::kQuerySettingsClusterParameterName,
        ServerParameterType::kClusterWide);
    ASSERT_NOT_OK(querySettingsParameter.validate(BSON("" << BSON("a"
                                                                  << "b"))
                                                      .firstElement(),
                                                  tenantId));

    // Ensure validation passes for valid input.
    QuerySettingsClusterParameterValue parameterValue({}, {});
    ASSERT_OK(querySettingsParameter.validate(BSON("" << parameterValue.toBSON()).firstElement(),
                                              tenantId));
}
}  // namespace mongo::query_settings
