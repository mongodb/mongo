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
