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
#include <boost/none.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <iterator>
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
#include "mongo/db/query/index_hint.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_settings/query_settings_cluster_parameter_gen.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/query_settings/query_settings_utils.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/serialization_context.h"

namespace mongo::query_settings {
namespace {
QueryShapeConfiguration makeQueryShapeConfiguration(const QuerySettings& settings,
                                                    QueryInstance query,
                                                    OperationContext* opCtx,
                                                    boost::optional<TenantId> tenantId) {
    auto queryShapeHash = createRepresentativeInfo(query, opCtx, tenantId).queryShapeHash;
    QueryShapeConfiguration result(queryShapeHash, settings);
    result.setRepresentativeQuery(query);
    return result;
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

static auto const kSerializationContext =
    SerializationContext{SerializationContext::Source::Command,
                         SerializationContext::CallerType::Request,
                         SerializationContext::Prefix::ExcludePrefix};

class QuerySettingsManagerTest : public ServiceContextTest {
public:
    static constexpr StringData kCollName = "exampleCol"_sd;
    static constexpr StringData kDbName = "foo"_sd;

    static std::vector<QueryShapeConfiguration> getExampleQueryShapeConfigurations(
        OperationContext* opCtx, TenantId tenantId) {
        OID tenantOid = OID::parse(tenantId.toString()).getValue();
        NamespaceSpec ns;
        ns.setDb(DatabaseNameUtil::deserialize(tenantId, kDbName, kSerializationContext));
        ns.setColl(kCollName);

        QuerySettings settings;
        settings.setQueryFramework(QueryFrameworkControlEnum::kTrySbeEngine);
        settings.setIndexHints({{IndexHintSpec(ns, {IndexHint("a_1")})}});
        QueryInstance queryA = BSON("find" << kCollName << "$db" << kDbName << "filter"
                                           << BSON("a" << 2) << "$tenant" << tenantOid);
        QueryInstance queryB = BSON("find" << kCollName << "$db" << kDbName << "filter"
                                           << BSON("a" << BSONNULL) << "$tenant" << tenantOid);
        return {makeQueryShapeConfiguration(settings, queryA, opCtx, tenantId),
                makeQueryShapeConfiguration(settings, queryB, opCtx, tenantId)};
    }

    void setUp() final {
        QuerySettingsManager::create(getServiceContext(), {});

        _opCtx = cc().makeOperationContext();
        _expCtx = ExpressionContext::makeBlankExpressionContext(opCtx(), {NamespaceString()});
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

    static NamespaceString nss(boost::optional<TenantId> tenantId) {
        static auto const kSerializationContext =
            SerializationContext{SerializationContext::Source::Command,
                                 SerializationContext::CallerType::Request,
                                 SerializationContext::Prefix::ExcludePrefix};

        return NamespaceStringUtil::deserialize(
            tenantId, kDbName, kCollName, kSerializationContext);
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(QuerySettingsManagerTest, QuerySettingsClusterParameterSerialization) {
    // Set query shape configuration.
    QuerySettings settings;
    settings.setQueryFramework(QueryFrameworkControlEnum::kTrySbeEngine);
    QueryInstance query = BSON("find"
                               << "exampleColl"
                               << "$db"
                               << "foo");
    auto config = makeQueryShapeConfiguration(settings, query, opCtx(), /* tenantId */ boost::none);
    LogicalTime clusterParameterTime(Timestamp(113, 59));
    manager().setQueryShapeConfigurations(
        opCtx(), {config}, clusterParameterTime, /* tenantId */ boost::none);

    // Ensure the serialized parameter value contains 'settingsArray' with 'config' as value as well
    // parameter id and clusterParameterTime.
    BSONObjBuilder bob;
    manager().appendQuerySettingsClusterParameterValue(opCtx(), &bob, /* tenantId */ boost::none);
    ASSERT_BSONOBJ_EQ(
        bob.done(),
        BSON("_id" << QuerySettingsManager::kQuerySettingsClusterParameterName
                   << QuerySettingsClusterParameterValue::kSettingsArrayFieldName
                   << BSON_ARRAY(config.toBSON())
                   << QuerySettingsClusterParameterValue::kClusterParameterTimeFieldName
                   << clusterParameterTime.asTimestamp()));
}

TEST_F(QuerySettingsManagerTest, QuerySettingsSetAndReset) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    LogicalTime firstWriteTime(Timestamp(1, 0)), secondWriteTime(Timestamp(2, 0));
    TenantId tenantId(OID::fromTerm(1)), otherTenantId(OID::fromTerm(2));
    auto firstConfig = getExampleQueryShapeConfigurations(opCtx(), tenantId)[0];
    auto secondConfig = getExampleQueryShapeConfigurations(opCtx(), otherTenantId)[1];

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
    using Result = boost::optional<std::pair<QuerySettings, boost::optional<QueryInstance>>>;
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);

    // Helper function for ensuring that two
    // 'QuerySettingsManager::getQuerySettingsForQueryShapeHash()' results are identical.
    auto assertResultsEq = [](Result r0, Result r1) {
        // Ensure that either both results are there, or both are missing.
        ASSERT_EQ(r0.has_value(), r1.has_value());

        // Early exit if any of them are missing.
        if (!r0.has_value() || !r1.has_value()) {
            return;
        }

        // Otherwise, ensure that both pair components are equal.
        ASSERT_BSONOBJ_EQ(r0->first.toBSON(), r1->first.toBSON());
        ASSERT_EQ(r0->second.has_value(), r1->second.has_value());

        // Early exit if query instances are missing.
        if (!r0->second.has_value()) {
            return;
        }
        ASSERT_BSONOBJ_EQ(*r0->second, *r1->second);
    };

    TenantId tenantId(OID::fromTerm(1));
    auto configs = getExampleQueryShapeConfigurations(opCtx(), tenantId);

    manager().setQueryShapeConfigurations(
        opCtx(), std::vector<QueryShapeConfiguration>(configs), LogicalTime(), tenantId);

    // Ensure QuerySettingsManager returns boost::none when QuerySettings are not found.
    assertResultsEq(manager().getQuerySettingsForQueryShapeHash(
                        opCtx(), query_shape::QueryShapeHash(), tenantId),
                    boost::none);

    // Ensure QuerySettingsManager returns a valid (QuerySettings, QueryInstance) pair on lookup.
    assertResultsEq(manager().getQuerySettingsForQueryShapeHash(
                        opCtx(), configs[1].getQueryShapeHash(), tenantId),
                    std::make_pair(configs[1].getSettings(), configs[1].getRepresentativeQuery()));
}

}  // namespace mongo::query_settings
