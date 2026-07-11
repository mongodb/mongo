// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
