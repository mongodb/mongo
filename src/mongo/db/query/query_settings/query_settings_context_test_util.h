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
