// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_knobs/query_knob_configuration.h"

#include "mongo/db/query/query_settings/query_settings.h"
#include "mongo/db/query/query_settings/query_settings_context.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using namespace query_settings;
using namespace query_settings::query_settings_details;

// Query settings carrying a single, easily-observable knob override: forcing the classic engine
// maps onto the 'queryFrameworkControl' knob, readable via 'isForceClassicEngineEnabled()'.
QuerySettings makeForceClassicEngineSettings() {
    QuerySettings settings;
    settings.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);
    return settings;
}

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
    ASSERT_EQ(QueryKnobConfiguration::get(opCtx.get()).isForceClassicEngineEnabled(),
              QueryKnobConfiguration{QuerySettings{}}.isForceClassicEngineEnabled());
}

TEST_F(QueryKnobConfigurationTest, ResolvedInstallsQuerySettingsOverrides) {
    auto opCtx = makeOperationContext();
    // Begin an eligible command and resolve settings on the operation.
    getQuerySettingsStateForOp(opCtx.get()) = Pending{};
    getQuerySettingsStateForOp(opCtx.get()) = makeForceClassicEngineSettings();

    // The resolved settings' knob overrides are applied to the configuration.
    ASSERT_TRUE(QueryKnobConfiguration::get(opCtx.get()).isForceClassicEngineEnabled());
}

TEST_F(QueryKnobConfigurationTest, InstallationIsDeferredUntilSettingsResolve) {
    auto opCtx = makeOperationContext();
    getQuerySettingsStateForOp(opCtx.get()) = Pending{};

    // First access happens while settings are still pending, so nothing is installed yet. Reading
    // a PQS-settable knob here is forbidden, so we only trigger the lazy construction.
    QueryKnobConfiguration::get(opCtx.get());

    // Once settings resolve, a later access installs them rather than remaining stuck uninstalled.
    // If the pending access had wrongly marked the configuration installed, this override would be
    // lost and the assertion would fail.
    getQuerySettingsStateForOp(opCtx.get()) = makeForceClassicEngineSettings();
    ASSERT_TRUE(QueryKnobConfiguration::get(opCtx.get()).isForceClassicEngineEnabled());
}

TEST_F(QueryKnobConfigurationTest, PreCommandAccessDoesNotLatchConfiguration) {
    auto opCtx = makeOperationContext();

    // A knob access before any command has begun (e.g. ingress admission control) must not latch
    // the configuration as final: the operation may still turn out to be an eligible command whose
    // settings override knob values.
    QueryKnobConfiguration::get(opCtx.get());

    getQuerySettingsStateForOp(opCtx.get()) = Pending{};
    getQuerySettingsStateForOp(opCtx.get()) = makeForceClassicEngineSettings();
    ASSERT_TRUE(QueryKnobConfiguration::get(opCtx.get()).isForceClassicEngineEnabled());
}

}  // namespace
}  // namespace mongo
