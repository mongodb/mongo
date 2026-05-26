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

#include "mongo/db/fle_compact_cleanup_mutex.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <thread>

namespace mongo {
namespace {

bool waitForPredicate(const std::function<bool()>& pred) {
    constexpr auto kTimeout = std::chrono::seconds(5);
    constexpr auto kSleepInterval = std::chrono::milliseconds(10);

    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(kSleepInterval);
    }
    return pred();
}

void markKilled(OperationContext* opCtx) {
    std::lock_guard<Client> lk(*opCtx->getClient());
    opCtx->markKilled(ErrorCodes::Interrupted);
}

void rethrowIfSet(const std::exception_ptr& ex) {
    if (ex) {
        std::rethrow_exception(ex);
    }
}

class FLECompactCleanupMutexTest : public ServiceContextMongoDTest {
protected:
    size_t registrySize() const {
        return getFLECompactCleanupMutexRegistrySizeForTest(getServiceContext());
    }
};

struct TestNamespaces {
    NamespaceString ecocLockNss;
};

TEST_F(FLECompactCleanupMutexTest, StableNamespacesHelperReturnsPostLockNamespaces) {
    auto opCtx = makeOperationContext();
    std::vector<TestNamespaces> namespaceResults{
        {NamespaceString::createNamespaceString_forTest("test.enxcol_.coll.ecoc.lock")},
        {NamespaceString::createNamespaceString_forTest("test.enxcol_.coll.ecoc.lock")}};
    size_t calls = 0;

    auto [namespaces, scopedMutex] = acquireFLECompactCleanupMutexWithStableNamespaces(
        opCtx.get(), [&] { return namespaceResults[calls++]; });

    ASSERT_EQ(namespaceResults.back().ecocLockNss, namespaces.ecocLockNss);
    ASSERT_EQ(2, calls);
    ASSERT_EQ(1, registrySize());

    scopedMutex.reset();
    ASSERT_EQ(0, registrySize());
}

TEST_F(FLECompactCleanupMutexTest, StableNamespacesHelperRetriesWhenLockNamespaceChanges) {
    auto opCtx = makeOperationContext();
    auto firstNss = NamespaceString::createNamespaceString_forTest("test.enxcol_.coll1.ecoc.lock");
    auto secondNss = NamespaceString::createNamespaceString_forTest("test.enxcol_.coll2.ecoc.lock");
    std::vector<TestNamespaces> namespaceResults{{firstNss}, {secondNss}, {secondNss}};
    size_t calls = 0;

    auto [namespaces, scopedMutex] = acquireFLECompactCleanupMutexWithStableNamespaces(
        opCtx.get(), [&] { return namespaceResults[calls++]; });

    ASSERT_EQ(secondNss, namespaces.ecocLockNss);
    ASSERT_EQ(3, calls);
    ASSERT_EQ(1, registrySize());

    scopedMutex.reset();
    ASSERT_EQ(0, registrySize());
}

TEST_F(FLECompactCleanupMutexTest, ScopedAcquisitionsOnSameNamespaceSerialize) {
    auto nss = NamespaceString::createNamespaceString_forTest("test.enxcol_.coll.ecoc.lock");
    auto holderOpCtx = makeOperationContext();
    auto holder = std::make_unique<ScopedFLECompactCleanupMutex>(holderOpCtx.get(), nss);

    std::promise<OperationContext*> waiterOpCtxPromise;
    auto waiterOpCtxFuture = waiterOpCtxPromise.get_future();
    std::promise<void> acquiredPromise;
    auto acquiredFuture = acquiredPromise.get_future();
    std::promise<void> releasePromise;
    auto releaseFuture = releasePromise.get_future();
    std::exception_ptr waiterException;

    std::thread waiter([&] {
        try {
            auto client = getServiceContext()->getService()->makeClient("sameNamespaceWaiter");
            auto opCtx = client->makeOperationContext();
            waiterOpCtxPromise.set_value(opCtx.get());

            ScopedFLECompactCleanupMutex scoped(opCtx.get(), nss);
            acquiredPromise.set_value();
            releaseFuture.get();
        } catch (...) {
            waiterException = std::current_exception();
        }
    });

    auto* waiterOpCtx = waiterOpCtxFuture.get();
    const auto waiterBlocked =
        waitForPredicate([&] { return waiterOpCtx->isWaitingForConditionOrInterrupt(); });
    const auto acquiredBeforeRelease =
        acquiredFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;

    holder.reset();
    const auto acquiredAfterRelease =
        acquiredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!acquiredAfterRelease) {
        markKilled(waiterOpCtx);
    }
    releasePromise.set_value();
    waiter.join();

    ASSERT_TRUE(waiterBlocked);
    ASSERT_FALSE(acquiredBeforeRelease);
    ASSERT_TRUE(acquiredAfterRelease);
    rethrowIfSet(waiterException);
    ASSERT_EQ(0, registrySize());
}

TEST_F(FLECompactCleanupMutexTest, ScopedAcquisitionsOnDifferentNamespacesDoNotBlock) {
    auto nss1 = NamespaceString::createNamespaceString_forTest("test.enxcol_.coll1.ecoc.lock");
    auto nss2 = NamespaceString::createNamespaceString_forTest("test.enxcol_.coll2.ecoc.lock");
    auto holderOpCtx = makeOperationContext();
    auto holder = std::make_unique<ScopedFLECompactCleanupMutex>(holderOpCtx.get(), nss1);

    std::promise<void> acquiredPromise;
    auto acquiredFuture = acquiredPromise.get_future();
    std::promise<void> releasePromise;
    auto releaseFuture = releasePromise.get_future();
    std::exception_ptr waiterException;

    std::thread waiter([&] {
        try {
            auto client = getServiceContext()->getService()->makeClient("differentNamespaceWaiter");
            auto opCtx = client->makeOperationContext();

            ScopedFLECompactCleanupMutex scoped(opCtx.get(), nss2);
            acquiredPromise.set_value();
            releaseFuture.get();
        } catch (...) {
            waiterException = std::current_exception();
        }
    });

    const auto acquiredWhileFirstNamespaceHeld =
        acquiredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    const auto registrySizeWhileBothAreHeld = registrySize();

    holder.reset();
    releasePromise.set_value();
    waiter.join();

    ASSERT_TRUE(acquiredWhileFirstNamespaceHeld);
    ASSERT_EQ(2, registrySizeWhileBothAreHeld);
    rethrowIfSet(waiterException);
    ASSERT_EQ(0, registrySize());
}

TEST_F(FLECompactCleanupMutexTest, InterruptedWaiterDoesNotLeakRegistryEntry) {
    auto nss = NamespaceString::createNamespaceString_forTest("test.enxcol_.coll.ecoc.lock");
    auto holderOpCtx = makeOperationContext();
    auto holder = std::make_unique<ScopedFLECompactCleanupMutex>(holderOpCtx.get(), nss);

    std::promise<OperationContext*> waiterOpCtxPromise;
    auto waiterOpCtxFuture = waiterOpCtxPromise.get_future();
    std::promise<void> interruptedPromise;
    auto interruptedFuture = interruptedPromise.get_future();
    std::exception_ptr waiterException;

    std::thread waiter([&] {
        try {
            auto client = getServiceContext()->getService()->makeClient("interruptedWaiter");
            auto opCtx = client->makeOperationContext();
            waiterOpCtxPromise.set_value(opCtx.get());

            ASSERT_THROWS_CODE(ScopedFLECompactCleanupMutex(opCtx.get(), nss),
                               DBException,
                               ErrorCodes::Interrupted);
            interruptedPromise.set_value();
        } catch (...) {
            waiterException = std::current_exception();
        }
    });

    auto* waiterOpCtx = waiterOpCtxFuture.get();
    const auto waiterBlocked =
        waitForPredicate([&] { return waiterOpCtx->isWaitingForConditionOrInterrupt(); });
    markKilled(waiterOpCtx);
    const auto interrupted =
        interruptedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready;

    holder.reset();
    waiter.join();

    ASSERT_TRUE(waiterBlocked);
    ASSERT_TRUE(interrupted);
    rethrowIfSet(waiterException);
    ASSERT_EQ(0, registrySize());
}

TEST_F(FLECompactCleanupMutexTest, RegistryEntriesAreRemovedAfterScopedObjectsAreDestroyed) {
    auto nss = NamespaceString::createNamespaceString_forTest("test.enxcol_.coll.ecoc.lock");
    auto opCtx = makeOperationContext();

    ASSERT_EQ(0, registrySize());
    {
        ScopedFLECompactCleanupMutex scoped(opCtx.get(), nss);
        ASSERT_EQ(1, registrySize());
    }
    ASSERT_EQ(0, registrySize());
}

}  // namespace
}  // namespace mongo
