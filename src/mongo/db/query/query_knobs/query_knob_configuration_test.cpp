/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
