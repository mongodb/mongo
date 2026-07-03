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

#include "mongo/db/query/query_lifespan.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <tuple>
#include <utility>

namespace mongo {
namespace {

// Test-only decorations exercised through the public 'declareOpCtxDecoration' seam. Two distinct
// types verify that decorations declared on the lifespan are independent.
struct CounterState {
    int value = 0;
};
struct FlagState {
    bool flag = false;
};

auto counterDecoration = QueryLifespan::declareOpCtxDecoration<CounterState>();
auto flagDecoration = QueryLifespan::declareOpCtxDecoration<FlagState>();

class QueryLifespanTest : public ServiceContextTest {
protected:
    // Creates an OperationContext on its own Client so multiple operations can be live at once
    // (a Client owns at most one OperationContext at a time). The returned handle keeps both the
    // Client and the OperationContext alive.
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

TEST_F(QueryLifespanTest, GetIsLazyAndStableForAnOperation) {
    auto opCtx = makeOperationContext();
    auto& first = QueryLifespan::get(opCtx.get());
    auto& second = QueryLifespan::get(opCtx.get());
    // Repeated access returns the same lazily-created instance.
    ASSERT_EQ(&first, &second);
}

TEST_F(QueryLifespanTest, DecoratedStatePersistsAcrossAccessesOnSameOpCtx) {
    auto opCtx = makeOperationContext();
    counterDecoration(opCtx.get()).value = 42;
    // A subsequent access through the same OperationContext observes the mutation.
    ASSERT_EQ(counterDecoration(opCtx.get()).value, 42);
}

TEST_F(QueryLifespanTest, DistinctOperationsHaveDistinctLifespans) {
    auto op1 = makeScopedOperationContext("op1");
    auto op2 = makeScopedOperationContext("op2");
    // Without binding, each operation gets its own lifespan and its own decorated state.
    counterDecoration(op1.get()).value = 1;
    counterDecoration(op2.get()).value = 2;
    ASSERT_NE(&QueryLifespan::get(op1.get()), &QueryLifespan::get(op2.get()));
    ASSERT_EQ(counterDecoration(op1.get()).value, 1);
    ASSERT_EQ(counterDecoration(op2.get()).value, 2);
}

TEST_F(QueryLifespanTest, MultipleDecorationTypesAreIndependent) {
    auto opCtx = makeOperationContext();
    counterDecoration(opCtx.get()).value = 7;
    flagDecoration(opCtx.get()).flag = true;
    ASSERT_EQ(counterDecoration(opCtx.get()).value, 7);
    ASSERT_TRUE(flagDecoration(opCtx.get()).flag);
}

TEST_F(QueryLifespanTest, HandleKeepsLifespanAlive) {
    auto opCtx = makeOperationContext();
    auto handle = QueryLifespan::get(opCtx.get()).handle();
    // The OperationContext's decoration holds one Handle and the local 'handle' holds another.
    ASSERT_GTE(handle.use_count(), 2);
    // The handled instance is the same one 'get()' returns.
    ASSERT_EQ(handle.get(), &QueryLifespan::get(opCtx.get()));
}

TEST_F(QueryLifespanTest, BindMakesLifespanVisibleOnAnotherOperation) {
    auto op1 = makeScopedOperationContext("op1");
    counterDecoration(op1.get()).value = 99;
    auto handle = QueryLifespan::get(op1.get()).handle();

    auto op2 = makeScopedOperationContext("op2");
    handle->bind(op2.get());

    // After binding, the second operation resolves to the very same lifespan and sees its state.
    ASSERT_EQ(&QueryLifespan::get(op2.get()), handle.get());
    ASSERT_EQ(counterDecoration(op2.get()).value, 99);
}

TEST_F(QueryLifespanTest, StateSurvivesOriginatingOperationDestruction) {
    QueryLifespan::Handle handle;
    {
        // Mirrors a cursor's originating operation: set query-scoped state, then take a handle.
        auto origin = makeScopedOperationContext("origin");
        counterDecoration(origin.get()).value = 123;
        handle = QueryLifespan::get(origin.get()).handle();
    }  // The originating OperationContext (and its Client) are destroyed here.

    // A later getMore-style operation binds the surviving lifespan and observes the prior state.
    auto getMore = makeScopedOperationContext("getMore");
    handle->bind(getMore.get());
    ASSERT_EQ(counterDecoration(getMore.get()).value, 123);
}

TEST_F(QueryLifespanTest, LifespanSurvivesSequentialGetMores) {
    // Mirrors the production pattern: the cursor's originating operation binds the lifespan onto
    // each getMore in turn, and every getMore sees state left behind by the previous one.
    auto cursorOp = makeScopedOperationContext("cursorOp");
    counterDecoration(cursorOp.get()).value = 1;
    auto handle = QueryLifespan::get(cursorOp.get()).handle();

    auto getMore1 = makeScopedOperationContext("getMore1");
    handle->bind(getMore1.get());
    ASSERT_EQ(counterDecoration(getMore1.get()).value, 1);
    counterDecoration(getMore1.get()).value = 2;

    auto getMore2 = makeScopedOperationContext("getMore2");
    handle->bind(getMore2.get());
    ASSERT_EQ(counterDecoration(getMore2.get()).value, 2);
}

TEST_F(QueryLifespanTest, RegionInstallsOverAPrePopulatedSlotAndRestoresIt) {
    // Regression: before a getMore body runs, other work on the same opCtx (e.g. an auth privilege
    // lookup via DBDirectClient) can already have created a different lifespan on it. The region
    // must install the cursor's lifespan over that slot for its scope, then restore the original.
    auto cursorOp = makeScopedOperationContext("cursorOp");
    counterDecoration(cursorOp.get()).value = 7;
    auto cursorLifespan = QueryLifespan::get(cursorOp.get()).handle();

    auto getMore = makeScopedOperationContext("getMore");
    // Simulate the pre-amble lazily creating a different lifespan on the getMore's opCtx.
    auto* preexisting = &QueryLifespan::get(getMore.get());
    ASSERT_NE(preexisting, cursorLifespan.get());

    {
        QueryLifespan::AlternativeQueryRegion region(getMore.get(), cursorLifespan);
        // Within the region the getMore resolves to the cursor's lifespan and sees its state.
        ASSERT_EQ(&QueryLifespan::get(getMore.get()), cursorLifespan.get());
        ASSERT_EQ(counterDecoration(getMore.get()).value, 7);
    }

    // On exit the opCtx's original lifespan is restored, with its own state intact.
    ASSERT_EQ(&QueryLifespan::get(getMore.get()), preexisting);
}

TEST_F(QueryLifespanTest, NestedRegionsRestoreLifo) {
    auto opCtx = makeScopedOperationContext("opCtx");
    auto* original = &QueryLifespan::get(opCtx.get());

    auto opB = makeScopedOperationContext("opB");
    auto lifespanB = QueryLifespan::get(opB.get()).handle();
    auto opC = makeScopedOperationContext("opC");
    auto lifespanC = QueryLifespan::get(opC.get()).handle();
    ASSERT_NE(lifespanB.get(), lifespanC.get());

    {
        QueryLifespan::AlternativeQueryRegion outer(opCtx.get(), lifespanB);
        ASSERT_EQ(&QueryLifespan::get(opCtx.get()), lifespanB.get());
        {
            QueryLifespan::AlternativeQueryRegion inner(opCtx.get(), lifespanC);
            ASSERT_EQ(&QueryLifespan::get(opCtx.get()), lifespanC.get());
        }
        // Inner region restores the outer region's lifespan.
        ASSERT_EQ(&QueryLifespan::get(opCtx.get()), lifespanB.get());
    }
    // Outer region restores the opCtx's original lifespan.
    ASSERT_EQ(&QueryLifespan::get(opCtx.get()), original);
}

TEST_F(QueryLifespanTest, MoveConstructedRegionDisarmsSource) {
    auto opCtx = makeScopedOperationContext("opCtx");
    auto* original = &QueryLifespan::get(opCtx.get());

    auto other = makeScopedOperationContext("other");
    auto lifespan = QueryLifespan::get(other.get()).handle();
    ASSERT_NE(original, lifespan.get());

    {
        QueryLifespan::AlternativeQueryRegion region(opCtx.get(), lifespan);
        ASSERT_EQ(&QueryLifespan::get(opCtx.get()), lifespan.get());

        // Move ownership; the destination keeps the cursor's lifespan installed.
        QueryLifespan::AlternativeQueryRegion moved(std::move(region));
        ASSERT_EQ(&QueryLifespan::get(opCtx.get()), lifespan.get());
    }
    // 'moved' restored the original; the moved-from 'region' was disarmed and did not swap again,
    // so the slot is restored exactly once (a double-restore would leave 'lifespan' installed).
    ASSERT_EQ(&QueryLifespan::get(opCtx.get()), original);
}

TEST_F(QueryLifespanTest, MoveAssignmentEagerlyRestoresLhsThenAdoptsRhs) {
    auto opA = makeScopedOperationContext("opA");
    auto* originalA = &QueryLifespan::get(opA.get());
    auto opB = makeScopedOperationContext("opB");
    auto* originalB = &QueryLifespan::get(opB.get());

    auto c1Op = makeScopedOperationContext("c1");
    auto c1 = QueryLifespan::get(c1Op.get()).handle();
    auto c2Op = makeScopedOperationContext("c2");
    auto c2 = QueryLifespan::get(c2Op.get()).handle();

    {
        QueryLifespan::AlternativeQueryRegion region1(opA.get(), c1);
        {
            QueryLifespan::AlternativeQueryRegion region2(opB.get(), c2);
            ASSERT_EQ(&QueryLifespan::get(opA.get()), c1.get());
            ASSERT_EQ(&QueryLifespan::get(opB.get()), c2.get());

            region1 = std::move(region2);

            // Assigning over region1 eagerly restored opA; region1 now owns opB's obligation, and
            // the moved-from region2 is disarmed.
            ASSERT_EQ(&QueryLifespan::get(opA.get()), originalA);
            ASSERT_EQ(&QueryLifespan::get(opB.get()), c2.get());
        }
        // region2 leaving scope does nothing (disarmed); opB stays installed until region1 ends.
        ASSERT_EQ(&QueryLifespan::get(opB.get()), c2.get());
    }
    // region1's destruction restores opB exactly once; opA was already restored at assignment.
    ASSERT_EQ(&QueryLifespan::get(opA.get()), originalA);
    ASSERT_EQ(&QueryLifespan::get(opB.get()), originalB);
}

// Death tests use a distinct fixture (and therefore a distinct test suite) because gtest requires
// all tests in a suite to share one fixture class, and DEATH_TEST_*_F generates its own.
class QueryLifespanDeathTest : public QueryLifespanTest {};

DEATH_TEST_REGEX_F(QueryLifespanDeathTest, GetWithNullOpCtxTasserts, "13020600") {
    QueryLifespan::get(nullptr);
}

DEATH_TEST_REGEX_F(QueryLifespanDeathTest, BindWithNullOpCtxTasserts, "13020601") {
    auto opCtx = makeOperationContext();
    QueryLifespan::get(opCtx.get()).bind(nullptr);
}

DEATH_TEST_REGEX_F(QueryLifespanDeathTest, BindOverDifferentLifespanTasserts, "13020602") {
    auto op1 = makeScopedOperationContext("op1");
    auto op2 = makeScopedOperationContext("op2");
    auto other = makeScopedOperationContext("other");
    QueryLifespan::get(op1.get()).bind(op2.get());
    // 'op2' is already bound to op1's lifespan; binding a different one over it should tassert.
    QueryLifespan::get(other.get()).bind(op2.get());
}

DEATH_TEST_REGEX_F(QueryLifespanDeathTest,
                   DeclareOpCtxDecorationTwiceForSameTypeInvariants,
                   "Invariant failure.*declareOpCtxDecoration") {
    std::ignore = QueryLifespan::declareOpCtxDecoration<CounterState>();
}

}  // namespace
}  // namespace mongo
