// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_settings_manager.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/serialization_context.h"

#include <string_view>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::query_settings {
using namespace std::literals::string_view_literals;
namespace {

bool operator==(const QuerySettings& lhs, const QuerySettings& rhs) {
    return SimpleBSONObjComparator::kInstance.compare(lhs.toBSON(), rhs.toBSON()) == 0;
}

QuerySettings makeQuerySettings(const IndexHintSpecs& indexHints) {
    QuerySettings settings;
    if (!indexHints.empty()) {
        settings.setIndexHints(indexHints);
    }
    settings.setQueryFramework(mongo::QueryFrameworkControlEnum::kTrySbeEngine);
    return settings;
}
}  // namespace

static auto const kSerializationContext = SerializationContext{
    SerializationContext::Source::Command, SerializationContext::CallerType::Request};

class QuerySettingsManagerTest : public ServiceContextTest {
public:
    static constexpr std::string_view kCollName = "exampleCol"sv;
    static constexpr std::string_view kDbName = "foo"sv;

    std::vector<QueryShapeConfiguration> getExampleQueryShapeConfigurations() {
        NamespaceSpec ns;
        ns.setDb(DatabaseNameUtil::deserialize(
            /* tenantId */ boost::none, kDbName, kSerializationContext));
        ns.setColl(kCollName);

        const QuerySettings settings = makeQuerySettings({IndexHintSpec(ns, {IndexHint("a_1")})});
        QueryInstance queryA =
            BSON("find" << kCollName << "$db" << kDbName << "filter" << BSON("a" << 2));
        QueryInstance queryB =
            BSON("find" << kCollName << "$db" << kDbName << "filter" << BSON("a" << BSONNULL));
        return {makeQueryShapeConfiguration(settings, queryA),
                makeQueryShapeConfiguration(settings, queryB)};
    }

    QueryShapeConfiguration makeQueryShapeConfiguration(const QuerySettings& settings,
                                                        QueryInstance query) {
        auto queryShapeHash = createRepresentativeInfo(opCtx(), query).queryShapeHash;
        return QueryShapeConfiguration(queryShapeHash, settings);
    }

    void setUp() final {
        ServiceContextTest::setUp();
        _opCtx = cc().makeOperationContext();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    QuerySettingsManager& manager() {
        return _manager;
    }

    static NamespaceString nss() {
        static auto const kSerializationContext = SerializationContext{
            SerializationContext::Source::Command, SerializationContext::CallerType::Request};

        return NamespaceStringUtil::deserialize(
            /* tenantId */ boost::none, kDbName, kCollName, kSerializationContext);
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    QuerySettingsManager _manager;
};

TEST_F(QuerySettingsManagerTest, QuerySettingsLookup) {
    auto configs = getExampleQueryShapeConfigurations();
    manager().setAllQueryShapeConfigurations({{configs}, LogicalTime()});

    // Ensure QuerySettingsManager returns boost::none when QuerySettings are not found.
    ASSERT_EQ(manager().getQuerySettingsForQueryShapeHash(query_shape::QueryShapeHash()),
              boost::none);

    // Ensure QuerySettingsManager returns a valid QuerySettings on lookup.
    ASSERT_EQ(
        manager().getQuerySettingsForQueryShapeHash(configs[1].getQueryShapeHash())->querySettings,
        configs[1].getSettings());
}

TEST_F(QuerySettingsManagerTest, QuerySettingsMarkBackfilled) {
    const auto configs = getExampleQueryShapeConfigurations();
    const auto& hash0 = configs[0].getQueryShapeHash();
    const auto& hash1 = configs[1].getQueryShapeHash();

    // Ensure that the 'hasRepresentativeQuery' flag is initially set to false for both queries.
    LogicalTime time;
    manager().setAllQueryShapeConfigurations({{configs}, time});
    ASSERT_FALSE(manager().getQuerySettingsForQueryShapeHash(hash0)->hasRepresentativeQuery);
    ASSERT_FALSE(manager().getQuerySettingsForQueryShapeHash(hash1)->hasRepresentativeQuery);

    // Mark the first query as backfilled and ensure that the 'hasRepresentativeQuery' flag is now
    // set to true for the first one, and false for the second one.
    manager().markBackfilledRepresentativeQueries({hash0}, time);
    ASSERT_TRUE(manager().getQuerySettingsForQueryShapeHash(hash0)->hasRepresentativeQuery);
    ASSERT_FALSE(manager().getQuerySettingsForQueryShapeHash(hash1)->hasRepresentativeQuery);

    // Mark the second query as backfilled and ensure that both flags are now set to true.
    manager().markBackfilledRepresentativeQueries({hash1}, time);
    ASSERT_TRUE(manager().getQuerySettingsForQueryShapeHash(hash0)->hasRepresentativeQuery);
    ASSERT_TRUE(manager().getQuerySettingsForQueryShapeHash(hash1)->hasRepresentativeQuery);

    // Set a new configuration with an advanced timestamp and assert that both flags are now false.
    LogicalTime nextTime = time;
    nextTime.addTicks(1);
    manager().setAllQueryShapeConfigurations({{configs}, nextTime});
    ASSERT_FALSE(manager().getQuerySettingsForQueryShapeHash(hash0)->hasRepresentativeQuery);
    ASSERT_FALSE(manager().getQuerySettingsForQueryShapeHash(hash1)->hasRepresentativeQuery);

    // Ensure that calling markBackfilledRepresentativeQueries() with a stale time throws a
    // "ConflictingOperationsInProgress" error.
    ASSERT_THROWS_CODE(manager().markBackfilledRepresentativeQueries({hash0}, time),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);

    // Ensure that it's possible to mark both queries with the new time.
    manager().markBackfilledRepresentativeQueries({hash0, hash1}, nextTime);
    ASSERT_TRUE(manager().getQuerySettingsForQueryShapeHash(hash0)->hasRepresentativeQuery);
    ASSERT_TRUE(manager().getQuerySettingsForQueryShapeHash(hash1)->hasRepresentativeQuery);
}
}  // namespace mongo::query_settings
