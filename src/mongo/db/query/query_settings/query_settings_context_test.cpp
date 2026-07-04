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

#include "mongo/db/query/query_settings/query_settings_context.h"

#include "mongo/db/client.h"
#include "mongo/db/query/query_lifespan.h"
#include "mongo/db/query/query_settings/query_settings.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <variant>

namespace mongo::query_settings {
namespace {

using namespace query_settings_details;

// A non-default QuerySettings used to verify that the resolved value round-trips.
QuerySettings makeRejectingSettings() {
    QuerySettings settings;
    settings.setReject(true);
    return settings;
}

class QuerySettingsContextTest : public ServiceContextTest {
protected:
    // Creates an OperationContext on its own Client so two operations can be live at once (a Client
    // owns at most one OperationContext at a time). Keeps both Client and OperationContext alive.
    struct ScopedOperationContext {
        ServiceContext::UniqueClient client;
        ServiceContext::UniqueOperationContext opCtx;

        OperationContext* get() const {
            return opCtx.get();
        }
    };

    ScopedOperationContext makeScopedOperationContext(std::string_view name) {
        auto client = getServiceContext()->getService()->makeClient(std::string{name});
        auto opCtx = client->makeOperationContext();
        return {std::move(client), std::move(opCtx)};
    }
};

TEST_F(QuerySettingsContextTest, NotStartedOperationYieldsEmptySettings) {
    auto opCtx = makeOperationContext();
    // No eligible command has begun: the state stays 'NotStarted' and reads see empty settings.
    ASSERT(std::holds_alternative<NotStarted>(getQuerySettingsStateForOp(opCtx.get())));
    ASSERT_FALSE(forOp(opCtx.get()).getReject().has_value());
}

TEST_F(QuerySettingsContextTest, ResolvedSettingsAreReturnedByForOp) {
    auto opCtx = makeOperationContext();
    getQuerySettingsStateForOp(opCtx.get()) = Pending{};
    auto& state = getQuerySettingsStateForOp(opCtx.get());  // 'Pending'.
    state = makeRejectingSettings();                        // Resolve with installed settings.

    ASSERT(std::holds_alternative<QuerySettings>(state));
    ASSERT_TRUE(forOp(opCtx.get()).getReject().value_or(false));
}

TEST_F(QuerySettingsContextTest, EmptyResolutionYieldsEmptySettings) {
    auto opCtx = makeOperationContext();
    getQuerySettingsStateForOp(opCtx.get()) = Pending{};
    auto& state = getQuerySettingsStateForOp(opCtx.get());  // 'Pending'.
    state = Empty{};                                        // Resolution matched nothing.

    ASSERT_FALSE(forOp(opCtx.get()).getReject().has_value());
}

// Death tests use a distinct fixture (and therefore a distinct test suite) because gtest requires
// all tests in a suite to share one fixture class, and DEATH_TEST_*_F generates its own. They are
// also required (rather than ASSERT_THROWS_CODE) because these are tripwire assertions, which the
// unittest framework treats as a fatal error even when the thrown exception is caught.
class QuerySettingsContextDeathTest : public QuerySettingsContextTest {};

DEATH_TEST_REGEX_F(QuerySettingsContextDeathTest, ForOpWhilePendingTasserts, "13020703") {
    auto opCtx = makeOperationContext();
    getQuerySettingsStateForOp(opCtx.get()) = Pending{};
    // The operation advances to 'Pending'; reading settings before resolution asserts.
    forOp(opCtx.get());
}

TEST_F(QuerySettingsContextTest, ResolvedSettingsSurviveAcrossBoundOperation) {
    // The state lives on the QueryLifespan, so resolved settings must remain visible after the
    // lifespan is bound onto a later operation (the getMore scenario).
    QueryLifespan::Handle handle;
    {
        auto origin = makeScopedOperationContext("origin");
        getQuerySettingsStateForOp(origin.get()) = Pending{};
        getQuerySettingsStateForOp(origin.get()) = makeRejectingSettings();
        handle = QueryLifespan::get(origin.get()).handle();
    }  // The originating operation ends here.

    auto getMore = makeScopedOperationContext("getMore");
    handle->bind(getMore.get());

    ASSERT(std::holds_alternative<QuerySettings>(getQuerySettingsStateForOp(getMore.get())));
    ASSERT_TRUE(forOp(getMore.get()).getReject().value_or(false));
}

}  // namespace
}  // namespace mongo::query_settings
