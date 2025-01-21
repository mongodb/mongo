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
#include "mongo/bson/json.h"
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
    auto queryShapeHash = createRepresentativeInfo(opCtx, query, tenantId).queryShapeHash;
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

BSONObj makeSettingsClusterParameter(const BSONArray& settings) {
    LogicalTime clusterParameterTime(Timestamp(113, 59));
    return BSON("_id" << QuerySettingsManager::kQuerySettingsClusterParameterName
                      << QuerySettingsClusterParameterValue::kSettingsArrayFieldName << settings
                      << QuerySettingsClusterParameterValue::kClusterParameterTimeFieldName
                      << clusterParameterTime.asTimestamp());
}

QuerySettings makeQuerySettings(const IndexHintSpecs& indexHints, bool setFramework = true) {
    QuerySettings settings;
    if (!indexHints.empty()) {
        settings.setIndexHints(indexHints);
    }
    if (setFramework) {
        settings.setQueryFramework(mongo::QueryFrameworkControlEnum::kTrySbeEngine);
    }
    return settings;
}

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
        NamespaceSpec ns;
        ns.setDb(DatabaseNameUtil::deserialize(tenantId, kDbName, kSerializationContext));
        ns.setColl(kCollName);

        const QuerySettings settings = makeQuerySettings({IndexHintSpec(ns, {IndexHint("a_1")})});
        QueryInstance queryA =
            BSON("find" << kCollName << "$db" << kDbName << "filter" << BSON("a" << 2));
        QueryInstance queryB =
            BSON("find" << kCollName << "$db" << kDbName << "filter" << BSON("a" << BSONNULL));
        return {makeQueryShapeConfiguration(settings, queryA, opCtx, tenantId),
                makeQueryShapeConfiguration(settings, queryB, opCtx, tenantId)};
    }

    void setUp() final {
        QuerySettingsManager::create(
            getServiceContext(), {}, query_settings::utils::sanitizeQuerySettingsHints);

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

    void assertTransformInvalidQuerySettings(
        const std::list<std::pair<const IndexHintSpecs, const IndexHintSpecs>>&
            listOfIndexHintSpecs) {
        auto sp = std::make_unique<QuerySettingsClusterParameter>(
            "querySettings", ServerParameterType::kClusterWide);
        BSONArrayBuilder bob;
        std::vector<QueryShapeConfiguration> expectedQueryShapeConfigurations;
        size_t collIndex = 0;
        // Create the 'querySettingsClusterParamValue' as BSON.
        for (const auto& [initialIndexHintSpecs, expectedIndexHintSpecs] : listOfIndexHintSpecs) {
            QueryInstance representativeQuery = BSON("find"
                                                     << "coll_" + std::to_string(collIndex) << "$db"
                                                     << "foo");
            bob.append(makeQueryShapeConfiguration(makeQuerySettings(initialIndexHintSpecs),
                                                   representativeQuery,
                                                   opCtx(),
                                                   /* tenantId */ boost::none)
                           .toBSON());
            expectedQueryShapeConfigurations.emplace_back(
                makeQueryShapeConfiguration(makeQuerySettings(expectedIndexHintSpecs),
                                            representativeQuery,
                                            opCtx(),
                                            /* tenantId */ boost::none));
            collIndex++;
        }
        // Set the cluster param value.
        const auto clusterParamValues = BSON_ARRAY(makeSettingsClusterParameter(bob.arr()));
        // Assert that parsing after transforming invalid settings (if any) works.
        ASSERT_OK(sp->set(clusterParamValues.firstElement(), boost::none));
        assertQueryShapeConfigurationsEquals(
            expectedQueryShapeConfigurations,
            manager()
                .getAllQueryShapeConfigurations(opCtx(), /* tenantId */ boost::none)
                .queryShapeConfigurations);
    }

    void assertSanitizeInvalidIndexHints(const IndexHintSpecs& initialSpec,
                                         const IndexHintSpecs& expectedSpec) {
        QueryInstance query = BSON("find"
                                   << "exampleColl"
                                   << "$db"
                                   << "foo");
        auto initSettings = makeQueryShapeConfiguration(makeQuerySettings(initialSpec),
                                                        query,
                                                        opCtx(),
                                                        /* tenantId */ boost::none);
        std::vector<QueryShapeConfiguration> queryShapeConfigs{initSettings};
        const auto expectedSettings = makeQuerySettings(expectedSpec);
        ASSERT_DOES_NOT_THROW(utils::sanitizeQuerySettingsHints(queryShapeConfigs));
        ASSERT_BSONOBJ_EQ(queryShapeConfigs[0].getSettings().toBSON(), expectedSettings.toBSON());
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
        {firstConfig},
        manager().getAllQueryShapeConfigurations(opCtx(), tenantId).queryShapeConfigurations);
    assertQueryShapeConfigurationsEquals(
        {firstConfig},
        manager().getAllQueryShapeConfigurations(opCtx(), otherTenantId).queryShapeConfigurations);
    ASSERT_EQ(manager().getClusterParameterTime(opCtx(), tenantId), firstWriteTime);
    ASSERT_EQ(manager().getClusterParameterTime(opCtx(), otherTenantId), firstWriteTime);

    // Update query settings for tenant with 'tenantId'. Ensure its query shape configurations and
    // parameter cluster time are updated accordingly.
    manager().setQueryShapeConfigurations(opCtx(), {secondConfig}, secondWriteTime, tenantId);
    assertQueryShapeConfigurationsEquals(
        {secondConfig},
        manager().getAllQueryShapeConfigurations(opCtx(), tenantId).queryShapeConfigurations);
    assertQueryShapeConfigurationsEquals(
        {firstConfig},
        manager().getAllQueryShapeConfigurations(opCtx(), otherTenantId).queryShapeConfigurations);
    ASSERT_EQ(manager().getClusterParameterTime(opCtx(), tenantId), secondWriteTime);
    ASSERT_EQ(manager().getClusterParameterTime(opCtx(), otherTenantId), firstWriteTime);

    // Reset the parameter value and ensure that the in-memory storage is cleared for tenant with
    // 'tenantId'.
    manager().removeAllQueryShapeConfigurations(opCtx(), tenantId);
    assertQueryShapeConfigurationsEquals(
        {}, manager().getAllQueryShapeConfigurations(opCtx(), tenantId).queryShapeConfigurations);

    // Attempt to remove QueryShapeConfigurations for a tenant that does not have any.
    manager().removeAllQueryShapeConfigurations(opCtx(), tenantId);
    assertQueryShapeConfigurationsEquals(
        {}, manager().getAllQueryShapeConfigurations(opCtx(), tenantId).queryShapeConfigurations);

    // Verify that QueryShapeConfigurations for tenant with 'otherTenantId' were not be affected.
    assertQueryShapeConfigurationsEquals(
        {firstConfig},
        manager().getAllQueryShapeConfigurations(opCtx(), otherTenantId).queryShapeConfigurations);
    ASSERT_EQ(manager().getClusterParameterTime(opCtx(), tenantId), LogicalTime::kUninitialized);
    ASSERT_EQ(manager().getClusterParameterTime(opCtx(), otherTenantId), firstWriteTime);
}

TEST_F(QuerySettingsManagerTest, QuerySettingsLookup) {
    using Result = boost::optional<QuerySettings>;
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
        ASSERT_BSONOBJ_EQ(r0->toBSON(), r1->toBSON());
    };

    TenantId tenantId(OID::fromTerm(1));
    auto configs = getExampleQueryShapeConfigurations(opCtx(), tenantId);

    manager().setQueryShapeConfigurations(
        opCtx(), std::vector<QueryShapeConfiguration>(configs), LogicalTime(), tenantId);

    // Ensure QuerySettingsManager returns boost::none when QuerySettings are not found.
    assertResultsEq(manager().getQuerySettingsForQueryShapeHash(
                        opCtx(), query_shape::QueryShapeHash(), tenantId),
                    boost::none);

    // Ensure QuerySettingsManager returns a valid QuerySettings on lookup.
    assertResultsEq(manager().getQuerySettingsForQueryShapeHash(
                        opCtx(), configs[1].getQueryShapeHash(), tenantId),
                    configs[1].getSettings());
}

/**
 * Tests that valid index hint specs are the same before and after index hint sanitization.
 */
TEST_F(QuerySettingsManagerTest, ValidIndexHintsAreTheSameBeforeAndAfterSanitization) {
    IndexHintSpecs indexHintSpec{IndexHintSpec(
        makeNsSpec("testCollA"_sd), {IndexHint(BSON("a" << 1)), IndexHint(BSON("b" << -1.0))})};
    assertSanitizeInvalidIndexHints(indexHintSpec, indexHintSpec);
}

/**
 * Tests that invalid key-pattern are removed after sanitization.
 */
TEST_F(QuerySettingsManagerTest, InvalidKeyPatternIndexesAreRemovedAfterSanitization) {
    IndexHintSpecs indexHintSpec{IndexHintSpec(makeNsSpec("testCollA"_sd),
                                               {IndexHint(BSON("a" << 1 << "c"
                                                                   << "invalid")),
                                                IndexHint(BSON("b" << -1.0))})};
    IndexHintSpecs expectedHintSpec{
        IndexHintSpec(makeNsSpec("testCollA"_sd), {IndexHint(BSON("b" << -1.0))})};
    assertSanitizeInvalidIndexHints(indexHintSpec, expectedHintSpec);
}

/**
 * Same as the above test but with more complex examples.
 */
TEST_F(QuerySettingsManagerTest, InvalidKeyPatternIndexesAreRemovedAfterSanitizationComplex) {
    IndexHintSpecs indexHintSpec{IndexHintSpec(makeNsSpec("testCollA"_sd),
                                               {
                                                   IndexHint(BSON("a" << 1 << "c"
                                                                      << "invalid")),
                                                   IndexHint(BSON("b" << -1.0)),
                                                   IndexHint(BSON("c" << -2.0 << "b" << 4)),
                                                   IndexHint("index_name"),
                                                   IndexHint(BSON("$natural" << 1)),
                                                   IndexHint(BSON("$natural" << -1 << "a" << 2)),
                                                   IndexHint(BSON("a" << -1 << "$natural" << 1)),
                                               })};
    IndexHintSpecs expectedHintSpec{IndexHintSpec(makeNsSpec("testCollA"_sd),
                                                  {IndexHint(BSON("b" << -1.0)),
                                                   IndexHint(BSON("c" << -2.0 << "b" << 4)),
                                                   IndexHint("index_name")})};
    assertSanitizeInvalidIndexHints(indexHintSpec, expectedHintSpec);
}


/**
 * Tests that invalid key-pattern are removed after sanitization and the resulted index hints are
 * empty.
 */
TEST_F(QuerySettingsManagerTest, InvalidKeyPatternIndexesAreRemovedAfterSanitizationEmptyHints) {
    IndexHintSpecs indexHintSpec{IndexHintSpec(makeNsSpec("testCollA"_sd),
                                               {
                                                   IndexHint(BSON("a" << 1 << "c"
                                                                      << "invalid")),
                                               })};
    IndexHintSpecs expectedHintSpec;
    assertSanitizeInvalidIndexHints(indexHintSpec, expectedHintSpec);
}

/**
 * Sets the value of QuerySettingsClusterParameter and asserts the resulting index hint spec is
 * sanitized.
 */
TEST_F(QuerySettingsManagerTest, SetValidClusterParameterAndAssertResultIsSanitized) {
    IndexHintSpecs indexHintSpec{IndexHintSpec(
        makeNsSpec("testCollA"), {IndexHint(BSON("a" << 1)), IndexHint(BSON("b" << -1.0))})};
    assertTransformInvalidQuerySettings({{indexHintSpec, indexHintSpec}});
}

/**
 * Same as above test, but assert that the validation would fail with invalid key-pattern indexes.
 */
TEST_F(QuerySettingsManagerTest, SetClusterParameterAndAssertResultIsSanitized) {
    IndexHintSpecs initialIndexHintSpec{
        IndexHintSpec(makeNsSpec("testCollA"_sd),
                      {IndexHint(BSON("a" << 1.0)),
                       IndexHint(BSON("b"
                                      << "-1.0"))}),
        IndexHintSpec(makeNsSpec("testCollB"_sd),
                      {IndexHint(BSON("a" << 2)), IndexHint(BSONObj{})})};
    IndexHintSpecs expectedIndexHintSpec{
        IndexHintSpec(makeNsSpec("testCollA"_sd), {IndexHint(BSON("a" << 1))}),
        IndexHintSpec(makeNsSpec("testCollB"_sd), {IndexHint(BSON("a" << 2))})};

    ASSERT_THROWS_CODE(utils::validateQuerySettings(makeQuerySettings(initialIndexHintSpec)),
                       DBException,
                       9646001);

    assertTransformInvalidQuerySettings({{initialIndexHintSpec, expectedIndexHintSpec}});
}

/**
 * Same as above test, but with multiple QuerySettings set.
 */
TEST_F(QuerySettingsManagerTest, SetClusterParameterAndAssertResultIsSanitizedMultipleQS) {
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
                      {IndexHint(BSON("b"
                                      << "some-string"
                                      << "a" << 1)),
                       IndexHint(BSONObj{})})};

    ASSERT_THROWS_CODE(utils::validateQuerySettings(makeQuerySettings(initialIndexHintSpec1)),
                       DBException,
                       9646000);

    ASSERT_THROWS_CODE(utils::validateQuerySettings(makeQuerySettings(initialIndexHintSpec2)),
                       DBException,
                       9646001);

    assertTransformInvalidQuerySettings(
        {{initialIndexHintSpec1, expectedIndexHintSpec1}, {initialIndexHintSpec2, {}}});
}

/**
 * Tests that query settings with invalid key-pattern index hints leads to no query settings after
 * sanitization.
 */
TEST_F(QuerySettingsManagerTest, SetClusterParameterInvalidQSSanitization) {
    auto sp = std::make_unique<QuerySettingsClusterParameter>("querySettings",
                                                              ServerParameterType::kClusterWide);
    IndexHintSpecs initialIndexHintSpec{
        IndexHintSpec(makeNsSpec("testCollC"_sd),
                      {IndexHint(BSON("a" << 1 << "$natural" << 1)), IndexHint(BSONObj{})}),
        IndexHintSpec(makeNsSpec("testCollD"_sd),
                      {IndexHint(BSON("b"
                                      << "some-string"
                                      << "a" << 1))})};
    const auto querySettings = makeQuerySettings(initialIndexHintSpec, false);
    ASSERT_THROWS_CODE(utils::validateQuerySettings(querySettings), DBException, 9646001);

    // Create the 'querySettingsClusterParamValue' as BSON.
    const auto config = makeQueryShapeConfiguration(querySettings,
                                                    BSON("find"
                                                         << "bar"
                                                         << "$db"
                                                         << "foo"),
                                                    opCtx(),
                                                    /* tenantId */ boost::none);
    // Assert that parsing after transforming invalid settings (if any) works.
    const auto clusterParamValues =
        BSON_ARRAY(makeSettingsClusterParameter(BSON_ARRAY(config.toBSON())));
    ASSERT_OK(sp->set(clusterParamValues.firstElement(), boost::none));
    auto res = manager().getQuerySettingsForQueryShapeHash(opCtx(),
                                                           config.getQueryShapeHash(),
                                                           /* tenantId */ boost::none);
    ASSERT(!res.has_value());
    ASSERT(manager()
               .getAllQueryShapeConfigurations(opCtx(), /* tenantId */ boost::none)
               .queryShapeConfigurations.empty());
}

}  // namespace mongo::query_settings
