// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/unittest/server_parameter_guard.h"

#include <string_view>
#include <utility>

namespace mongo {

/**
 * Test-only RAII guard that sets a query knob (via its server parameter) for its lifetime and
 * invalidates the operation's cached QueryKnobConfiguration, so the new value is observed on the
 * next 'QueryKnobConfiguration::get(opCtx)'. On destruction it restores the previous knob value and
 * invalidates the cache again, so later accesses re-derive from the restored value.
 */
class QueryKnobGuardForTest {
public:
    template <typename T>
    QueryKnobGuardForTest(OperationContext* opCtx, std::string_view knobName, T value)
        : _paramGuard(knobName, value), _opCtx(opCtx) {
        _resetCachedConfiguration();
    }

    ~QueryKnobGuardForTest();

    QueryKnobGuardForTest(const QueryKnobGuardForTest&) = delete;
    QueryKnobGuardForTest& operator=(const QueryKnobGuardForTest&) = delete;

    QueryKnobGuardForTest(QueryKnobGuardForTest&& other) noexcept
        : _paramGuard(std::move(other._paramGuard)), _opCtx(std::exchange(other._opCtx, nullptr)) {}

    QueryKnobGuardForTest& operator=(QueryKnobGuardForTest&& other) noexcept {
        QueryKnobGuardForTest moved(std::move(other));
        using std::swap;
        swap(_paramGuard, moved._paramGuard);
        swap(_opCtx, moved._opCtx);
        return *this;
    }

private:
    void _resetCachedConfiguration();

    unittest::ServerParameterGuard _paramGuard;
    OperationContext* _opCtx;
};

}  // namespace mongo
