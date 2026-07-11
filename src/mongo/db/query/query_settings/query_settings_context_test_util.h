// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_settings/query_settings_context.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"

#include <utility>

namespace mongo {
class OperationContext;

namespace query_settings {

/**
 * Test-only helper that resolves 'settings' onto 'opCtx' as a PQS-supported command would, so that
 * 'forOp(opCtx)' returns them and 'QueryKnobConfiguration::get(opCtx)' installs their knob
 * overrides. Construction swaps the new settings into the operation's query settings state and
 * saves the previous state; destruction restores it, so guards may nest or be used sequentially on
 * an operation that outlives them.
 *
 * Note: any QueryKnobConfiguration already materialized for the operation is not reset. If knob
 * values were read (via 'QueryKnobConfiguration::get') under one guard, a subsequent guard's
 * settings will not be reflected in that cached configuration.
 */
class QuerySettingsGuardForTest {
public:
    QuerySettingsGuardForTest(OperationContext* opCtx, BSONObj settingsObj);
    QuerySettingsGuardForTest(OperationContext* opCtx, const QuerySettings& settings);
    ~QuerySettingsGuardForTest();

    QuerySettingsGuardForTest(const QuerySettingsGuardForTest&) = delete;
    QuerySettingsGuardForTest& operator=(const QuerySettingsGuardForTest&) = delete;

    QuerySettingsGuardForTest(QuerySettingsGuardForTest&& other) noexcept
        : _state(std::exchange(other._state, nullptr)), _previous(std::move(other._previous)) {}

    QuerySettingsGuardForTest& operator=(QuerySettingsGuardForTest&& other) noexcept {
        QuerySettingsGuardForTest moved(std::move(other));
        using std::swap;
        swap(_state, moved._state);
        swap(_previous, moved._previous);
        return *this;
    }

private:
    query_settings_details::QuerySettingsOperationState* _state;
    query_settings_details::QuerySettingsOperationState _previous;
};

}  // namespace query_settings
}  // namespace mongo
