// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_knobs/query_knob_configuration.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/query_settings/query_knob_overrides.h"
#include "mongo/db/query/query_settings/query_settings.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class QueryKnobConfigurationTest : public ServiceContextTest {};

TEST_F(QueryKnobConfigurationTest, GetReturnsStableInstancePerOperation) {
    auto opCtx = makeOperationContext();
    auto& first = QueryKnobConfiguration::get(opCtx.get());
    auto& second = QueryKnobConfiguration::get(opCtx.get());
    // The configuration is resolved once and cached on the operation.
    ASSERT_EQ(&first, &second);
}

TEST_F(QueryKnobConfigurationTest, NotStartedUsesGlobalKnobValues) {
    auto opCtx = makeOperationContext();
    // No eligible command has begun on the operation, so its configuration reflects the global
    // knob values, matching a configuration built from empty settings.
    ASSERT_EQ(
        QueryKnobConfiguration::get(opCtx.get()).isForceClassicEngineEnabled(),
        QueryKnobConfiguration{query_settings::QuerySettings{}}.isForceClassicEngineEnabled());
}

QueryKnobConfiguration makeConfig() {
    return QueryKnobConfiguration{query_settings::QuerySettings{}};
}

// ---------------------------------------------------------------------------
// getPlanRanker()
// ---------------------------------------------------------------------------

// The IDL default for internalQueryPlanRanker is kMixed; with the feature flag
// on (its own default), getPlanRanker() must return that default.
TEST_F(QueryKnobConfigurationTest, DefaultPlanRankerIsMixed) {
    ASSERT_EQ(makeConfig().getPlanRanker(), QueryPlanRankerEnum::kMixed);
}

TEST_F(QueryKnobConfigurationTest, PlanRankerReturnsCostBased) {
    unittest::ServerParameterGuard knobGuard{"internalQueryPlanRanker", "costBased"};
    ASSERT_EQ(makeConfig().getPlanRanker(), QueryPlanRankerEnum::kCostBased);
}

TEST_F(QueryKnobConfigurationTest, PlanRankerReturnsMixed) {
    unittest::ServerParameterGuard knobGuard{"internalQueryPlanRanker", "mixed"};
    ASSERT_EQ(makeConfig().getPlanRanker(), QueryPlanRankerEnum::kMixed);
}

TEST_F(QueryKnobConfigurationTest, PlanRankerReturnsMultiPlanner) {
    unittest::ServerParameterGuard knobGuard{"internalQueryPlanRanker", "multiPlanning"};
    ASSERT_EQ(makeConfig().getPlanRanker(), QueryPlanRankerEnum::kMultiPlanner);
}

// ---------------------------------------------------------------------------
// Query Knobs <<  QuerySettings
// ---------------------------------------------------------------------------

TEST_F(QueryKnobConfigurationTest, QuerySettingsPlanRankerToCostBased) {
    query_settings::QuerySettings qs;
    qs.setQueryKnobs(
        query_settings::QuerySettingsKnobOverrides::fromBSON(BSON("planRanker" << "costBased")));
    ASSERT_EQ(QueryKnobConfiguration{qs}.getPlanRanker(), QueryPlanRankerEnum::kCostBased);
}

TEST_F(QueryKnobConfigurationTest, QueryKnobOverrideByQuerySettings) {
    unittest::ServerParameterGuard paramGuard{"internalQueryPlanRanker", "costBased"};
    query_settings::QuerySettings qs;
    qs.setQueryKnobs(
        query_settings::QuerySettingsKnobOverrides::fromBSON(BSON("planRanker" << "mixed")));
    ASSERT_EQ(QueryKnobConfiguration{qs}.getPlanRanker(), QueryPlanRankerEnum::kMixed);
}

// ---------------------------------------------------------------------------
// serializeForExplain() — source attribution
// ---------------------------------------------------------------------------

TEST_F(QueryKnobConfigurationTest, SerializeForExplainIsEmptyForAllDefaults) {
    ASSERT_TRUE(makeConfig().serializeForExplain().isEmpty());
}

TEST_F(QueryKnobConfigurationTest, SerializeForExplainShowsSetParameterSource) {
    unittest::ServerParameterGuard paramGuard{"internalQueryPlanRanker", "costBased"};
    auto explain = makeConfig().serializeForExplain();
    ASSERT_EQ(explain["planRanker"].Obj()["source"].String(), "setParameter");
}

TEST_F(QueryKnobConfigurationTest, SerializeForExplainShowsQuerySettingsSource) {
    query_settings::QuerySettings qs;
    qs.setQueryKnobs(
        query_settings::QuerySettingsKnobOverrides::fromBSON(BSON("planRanker" << "costBased")));
    auto explain = QueryKnobConfiguration{qs}.serializeForExplain();
    ASSERT_EQ(explain["planRanker"].Obj()["source"].String(), "querySettings");
}

}  // namespace
}  // namespace mongo
