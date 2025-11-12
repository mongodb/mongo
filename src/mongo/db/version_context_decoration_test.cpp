/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/version_context.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
// (Generic FCV reference): used for testing, should exist across LTS binary versions
static const VersionContext kLatestVersionContext{multiversion::GenericFCV::kLatest};
static const VersionContext kLastLTSVersionContext{multiversion::GenericFCV::kLastLTS};

class VersionContextDecorationTest : public ServiceContextTest {
public:
    ServiceContext::UniqueOperationContext opCtxHolder{makeOperationContext()};
    OperationContext* opCtx{opCtxHolder.get()};
};

TEST_F(VersionContextDecorationTest, GetDecorationDefault) {
    ASSERT_EQ(VersionContext{}, VersionContext::getDecoration(opCtx));
}

TEST_F(VersionContextDecorationTest, SetDecoration) {
    {
        ClientLock lk(opCtx->getClient());
        VersionContext::setDecoration(lk, opCtx, kLatestVersionContext);
    }
    ASSERT_EQ(kLatestVersionContext, VersionContext::getDecoration(opCtx));
}

TEST_F(VersionContextDecorationTest, ScopedSetDecoration) {
    ASSERT_EQ(VersionContext{}, VersionContext::getDecoration(opCtx));
    {
        VersionContext::ScopedSetDecoration scopedSetDecoration(opCtx, kLastLTSVersionContext);
        ASSERT_EQ(kLastLTSVersionContext, VersionContext::getDecoration(opCtx));
    }
    ASSERT_EQ(VersionContext{}, VersionContext::getDecoration(opCtx));
}

DEATH_TEST_F(VersionContextDecorationTest,
             ScopedSetDecorationRecursive,
             "Refusing to set a VersionContext on an operation that already has one") {
    VersionContext::ScopedSetDecoration scopedVCtx(opCtx, kLastLTSVersionContext);
    VersionContext::ScopedSetDecoration(opCtx, kLastLTSVersionContext);
}

TEST_F(VersionContextDecorationTest, FixedOperationFCVRegion) {
    ASSERT_EQ(VersionContext{}, VersionContext::getDecoration(opCtx));

    {
        // OFCV set while `FixedOperationFCVRegion` is in scope
        VersionContext expectedVc{serverGlobalParams.featureCompatibility.acquireFCVSnapshot()};
        VersionContext::FixedOperationFCVRegion fixedOperationFcvRegion(opCtx);
        ASSERT_EQ(expectedVc, VersionContext::getDecoration(opCtx));
    }

    // No OFCV after `FixedOperationFCVRegion` gets out of scope
    ASSERT_EQ(VersionContext{}, VersionContext::getDecoration(opCtx));
}

TEST_F(VersionContextDecorationTest, FixedOperationFCVRegionReentrancy) {
    ASSERT_EQ(VersionContext{}, VersionContext::getDecoration(opCtx));

    {
        // (Generic FCV reference): used for testing
        serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
        VersionContext expectedVc{multiversion::GenericFCV::kLatest};

        // Set latest OFCV on the operation context
        VersionContext::FixedOperationFCVRegion fixedOperationFcvRegion0(opCtx);
        ASSERT_EQ(expectedVc, VersionContext::getDecoration(opCtx));

        {
            // The original OFCV is preserved even though the FCV snapshot changed
            serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLastLTS);
            VersionContext::FixedOperationFCVRegion fixedOperationFcvRegion1(opCtx);
            ASSERT_EQ(expectedVc, VersionContext::getDecoration(opCtx));
        }

        // The OFCV is not unset when the second scoped object gets out of scope
        ASSERT_EQ(expectedVc, VersionContext::getDecoration(opCtx));
    }

    // No OFCV after all `FixedOperationFCVRegion` objects got out of scope
    ASSERT_EQ(VersionContext{}, VersionContext::getDecoration(opCtx));
}

TEST_F(VersionContextDecorationTest, FixedOperationFCVRegionSetDuringFcvTransition) {
    auto failPoint = globalFailPointRegistry().find("waitBeforeFixedOperationFCVRegionRaceCheck");
    failPoint->setMode(FailPoint::alwaysOn);

    // (Generic FCV reference): used for testing
    // Set FCV to last-lts
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLastLTS);

    // Make the FixedOperationFCVRegion constructor race with a FCV transition (last-lts to latest),
    // then check that the target FCV version has been set
    unittest::JoinThread setFcvToLatestThread([&] {
        failPoint->waitForTimesEntered(1);

        serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
        failPoint->setMode(FailPoint::off);
    });

    VersionContext expectedVc = kLatestVersionContext;
    VersionContext::FixedOperationFCVRegion fixedOperationFcvRegion(opCtx);
    ASSERT_EQ(expectedVc, VersionContext::getDecoration(opCtx));
}

class VersionContextDrainTest : public ServiceContextTest {
public:
    // We need to hold both the client and the operation context in scope in order for the OFCV to
    // be in use
    using ScopedHoldOperation =
        std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>;

    const VersionContext kStaleVersion{kLatestVersionContext};
    const VersionContext kCurrentVersion{kLastLTSVersionContext};

    auto makeClient(std::string desc = "client") {
        return getServiceContext()->getService()->makeClient(desc, nullptr /* session */);
    }

    [[nodiscard]] ScopedHoldOperation getRunningOperationWithStaleOfcv() {
        auto client = makeClient("clientWithStaleOFCV");
        auto opCtx = client->makeOperationContext();

        {
            ClientLock lk(opCtx->getClient());
            VersionContext::setDecoration(lk, opCtx.get(), kStaleVersion);
        }

        return {std::move(client), std::move(opCtx)};
    }

    [[nodiscard]] ScopedHoldOperation getRunningOperationWithCurrentOrWithoutOfcv() {
        auto client = makeClient("clientWithoutStaleOFCV");
        auto opCtx = client->makeOperationContext();

        if (SecureRandom().nextInt64() % 2) {
            ClientLock lk(opCtx->getClient());
            VersionContext::setDecoration(lk, opCtx.get(), kCurrentVersion);
        }

        return {std::move(client), std::move(opCtx)};
    }

    VersionContextDrainTest() : _client(makeClient("VersionContextDrainTest")) {
        _uniqueOpCtx = _client->makeOperationContext();
    }

    OperationContext* operationContext() {
        return _uniqueOpCtx.get();
    }

private:
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _uniqueOpCtx;
};

TEST_F(VersionContextDrainTest, NoOperationAtAll) {
    waitForOperationsNotMatchingVersionContextToComplete(operationContext(), kCurrentVersion);
    waitForOperationsNotMatchingVersionContextToComplete(
        operationContext(), kCurrentVersion, Date_t::now());
}

TEST_F(VersionContextDrainTest, NoOperationToWaitOn) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();
    {
        ClientLock lk(opCtx->getClient());
        VersionContext::setDecoration(lk, opCtx.get(), kCurrentVersion);
    }

    waitForOperationsNotMatchingVersionContextToComplete(operationContext(), kCurrentVersion);
    waitForOperationsNotMatchingVersionContextToComplete(
        operationContext(), kCurrentVersion, Date_t::now());
}

TEST_F(VersionContextDrainTest, NoWaitOnKilledOperation) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();
    {
        ClientLock lk(opCtx->getClient());
        VersionContext::setDecoration(lk, opCtx.get(), kStaleVersion);
    }
    opCtx->markKilled(ErrorCodes::Interrupted);

    waitForOperationsNotMatchingVersionContextToComplete(operationContext(), kCurrentVersion);
}

TEST_F(VersionContextDrainTest, OneOperationToWaitOn) {
    SharedPromise<void> operationWithStaleVersionInitialized;
    SharedPromise<void> drainOperationWithStaleVersion;

    unittest::JoinThread threadOpWithStaleOFCV([&] {
        auto holdClientAndOpctx = getRunningOperationWithStaleOfcv();
        // Signal that the operation with stale OFCV has been initialized
        operationWithStaleVersionInitialized.emplaceValue();
        // Wait for signal to drain operations
        drainOperationWithStaleVersion.getFuture().get();
    });

    // Wait for `threadOpWithStaleOFCV` to finish initializing the operation with stale OFCV
    operationWithStaleVersionInitialized.getFuture().get();

    unittest::JoinThread asyncWaitForOpToDrain([&] {
        // All possible interleavings will be covered by multiple test runs since this
        // async wait may start before/during/after drain.
        auto client = makeClient("asyncWaitForOpsToDrain");
        auto opCtx = client->makeOperationContext();
        waitForOperationsNotMatchingVersionContextToComplete(opCtx.get(), kCurrentVersion);
    });

    // Also initialize an opCtx that should NOT be drained, it should not influence the wait
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();
    ASSERT_THROWS_CODE(waitForOperationsNotMatchingVersionContextToComplete(
                           operationContext(), kCurrentVersion, Date_t::now() + Milliseconds(50)),
                       DBException,
                       ErrorCodes::ExceededTimeLimit);

    // Signal thread that the operation with stale OFCV can drain
    drainOperationWithStaleVersion.emplaceValue();
    auto ignoredOpWithoutStaleOfcv = getRunningOperationWithCurrentOrWithoutOfcv();
    waitForOperationsNotMatchingVersionContextToComplete(operationContext(), kCurrentVersion);
}

/**
 * Reduce the internal recheck interval to 10ms, then wait for an operation to drain requiring at
 * least 100ms. Regression test to make sure internal retries don't throw.
 */
TEST_F(VersionContextDrainTest, LowerRecheckOperationContextsInterval) {
    auto failPoint = globalFailPointRegistry().find("reduceWaitForOfcvInternalIntervalTo10Ms");
    failPoint->setMode(FailPoint::alwaysOn);
    ON_BLOCK_EXIT([failPoint]() { failPoint->setMode(FailPoint::off); });

    SharedPromise<void> operationWithStaleVersionInitialized;
    SharedPromise<void> startWaitingForOperationsToDrain;
    SharedPromise<void> drainOperationWithStaleVersion;

    unittest::JoinThread threadOpWithStaleOFCV([&] {
        auto holdClientAndOpctx = getRunningOperationWithStaleOfcv();
        // Signal that the operation with stale OFCV has been initialized
        operationWithStaleVersionInitialized.emplaceValue();
        // Wait for signal to drain operations
        drainOperationWithStaleVersion.getFuture().get();
    });

    // Wait for `threadOpWithStaleOFCV` to finish initializing the operation with stale OFCV
    operationWithStaleVersionInitialized.getFuture().get();

    unittest::JoinThread asyncWaitForOpToDrain([&] {
        auto client = makeClient("asyncWaitForOpsToDrain");
        auto opCtx = client->makeOperationContext();
        // Future not exactly ready when starting waiting, but just before
        startWaitingForOperationsToDrain.emplaceValue();
        waitForOperationsNotMatchingVersionContextToComplete(opCtx.get(), kCurrentVersion);
    });

    startWaitingForOperationsToDrain.getFuture().get();
    sleepFor(Milliseconds(100));

    // Signal thread that the operation with stale OFCV can drain
    drainOperationWithStaleVersion.emplaceValue();
}

/**
 * Checks that when an operation context releases the OFCV, the wait finishes despite the operation
 * context being still alive.
 */
TEST_F(VersionContextDrainTest, OperationContextIgnoredWhenScopedOfcvReleased) {
    auto failPoint = globalFailPointRegistry().find("reduceWaitForOfcvInternalIntervalTo10Ms");
    failPoint->setMode(FailPoint::alwaysOn);
    ON_BLOCK_EXIT([failPoint]() { failPoint->setMode(FailPoint::off); });

    SharedPromise<void> operationWithScopedStaleOfcvInitialized;
    SharedPromise<void> releaseScopedOfcv;
    SharedPromise<void> drainOperationContext;

    unittest::JoinThread threadOpWithScopedStaleOFCV([&] {
        auto client = makeClient("threadOpWithStaleOFCV");
        auto opCtx = client->makeOperationContext();

        {
            VersionContext::ScopedSetDecoration scopedSetDecoration(opCtx.get(), kStaleVersion);
            // Signal that the operation with scoped stale OFCV has been initialized
            operationWithScopedStaleOfcvInitialized.emplaceValue();
            // Wait for signal to release the scoped OFCV
            releaseScopedOfcv.getFuture().get();
        }

        // Wait before releasing the operation context
        drainOperationContext.getFuture().get();
    });

    operationWithScopedStaleOfcvInitialized.getFuture().get();

    unittest::JoinThread asyncWaitForOfcvToBeReleased([&] {
        auto client = makeClient("asyncWaitForOpsToDrain");
        auto opCtx = client->makeOperationContext();

        // All possible interleavings will be covered by multiple test runs since this
        // async wait may start before/during/after the scoped OFCV is released.
        waitForOperationsNotMatchingVersionContextToComplete(opCtx.get(), kCurrentVersion);
        drainOperationContext.emplaceValue();
    });

    // Signal thread that the operation with stale OFCV can release it
    releaseScopedOfcv.emplaceValue();
}

TEST_F(VersionContextDrainTest, RandomNoOperationToWaitOn) {
    std::vector<ScopedHoldOperation> ops;
    const int nOps = SecureRandom().nextInt64() % 5;
    for (int i = 0; i < nOps; i++) {
        ops.emplace_back(getRunningOperationWithCurrentOrWithoutOfcv());
    }

    waitForOperationsNotMatchingVersionContextToComplete(operationContext(), kCurrentVersion);
    waitForOperationsNotMatchingVersionContextToComplete(
        operationContext(), kCurrentVersion, Date_t::now());
}

TEST_F(VersionContextDrainTest, RandomOperationsToWaitOn) {
    std::vector<ScopedHoldOperation> ops;
    const int nOps = SecureRandom().nextInt64() % 5;
    for (int i = 0; i < nOps; i++) {
        ops.emplace_back(getRunningOperationWithStaleOfcv());
    }

    for (int i = 0; i < nOps; i++) {
        ASSERT_THROWS_CODE(waitForOperationsNotMatchingVersionContextToComplete(
                               operationContext(), kCurrentVersion, Date_t::now()),
                           DBException,
                           ErrorCodes::ExceededTimeLimit);
        ops.pop_back();
    }

    waitForOperationsNotMatchingVersionContextToComplete(
        operationContext(), kCurrentVersion, Date_t::now());
}

TEST_F(VersionContextDrainTest, RandomMixedOperations) {
    std::vector<ScopedHoldOperation> ops;
    const int nOps = SecureRandom().nextInt64() % 5;

    // Random running/killed operations running with or without stale OFCV
    int nRunningOpsStaleOfcv = 0;
    for (int i = 0; i < nOps; i++) {
        bool opWithStaleOfcv = SecureRandom().nextInt64() % 2;
        bool killedOp = SecureRandom().nextInt64() % 2;

        if (opWithStaleOfcv) {
            ops.emplace_back(getRunningOperationWithStaleOfcv());
        } else {
            ops.emplace_back(getRunningOperationWithCurrentOrWithoutOfcv());
        }

        if (killedOp) {
            ops.back().second->markKilled(ErrorCodes::CursorKilled);
        }

        if (opWithStaleOfcv && !killedOp) {
            nRunningOpsStaleOfcv++;
        }
    }

    // Check that waiting with a deadline fails as long as at least one operation
    // with stale OFCV is running
    for (int i = 0; i < nOps; i++) {
        if (nRunningOpsStaleOfcv == 0) {
            break;
        }

        ASSERT_THROWS_CODE(waitForOperationsNotMatchingVersionContextToComplete(
                               operationContext(), kCurrentVersion, Date_t::now()),
                           DBException,
                           ErrorCodes::ExceededTimeLimit);

        const ScopedHoldOperation op = std::move(ops.back());
        ops.pop_back();

        OperationContext* opCtx = op.first->getOperationContext();
        invariant(opCtx);

        const bool notKillPending = !opCtx->isKillPending();
        const bool withStaleVersion = VersionContext::getDecoration(opCtx) == kStaleVersion;

        if (notKillPending && withStaleVersion) {
            nRunningOpsStaleOfcv--;
        }
    }

    waitForOperationsNotMatchingVersionContextToComplete(
        operationContext(), kCurrentVersion, Date_t::now());
}

}  // namespace
}  // namespace mongo
