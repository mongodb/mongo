/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

// IWYU pragma: no_include "cxxabi.h"
#include "mongo/db/s/active_migrations_registry.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/baton.h"
#include "mongo/db/client.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/unittest.h"

#include <future>
#include <system_error>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

template <typename T>
using nolint_promise = std::promise<T>;  // NOLINT

using unittest::assertGet;

class MoveChunkRegistration : public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();
        _opCtx = operationContext();
        _opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
    }

protected:
    ActiveMigrationsRegistry _registry;
    OperationContext* _opCtx;
};

ShardsvrMoveRange createMoveRangeRequest(const NamespaceString& nss,
                                         const OID& epoch = OID::gen()) {
    const ShardId fromShard = ShardId("shard0001");
    const long long maxChunkSizeBytes = 1024;
    ShardsvrMoveRange req(nss, Timestamp(10), fromShard, maxChunkSizeBytes);
    req.getMoveRangeRequestBase().setToShard(ShardId("shard0002"));
    req.setMaxChunkSizeBytes(1024);
    req.getMoveRangeRequestBase().setMin(BSON("Key" << -100));
    req.getMoveRangeRequestBase().setMax(BSON("Key" << 100));
    return req;
}

TEST_F(MoveChunkRegistration, ScopedDonateChunkMoveConstructorAndAssignment) {
    auto originalScopedDonateChunk = assertGet(_registry.registerDonateChunk(
        _opCtx,
        createMoveRangeRequest(
            NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"))));
    ASSERT(originalScopedDonateChunk.mustExecute());

    ScopedDonateChunk movedScopedDonateChunk(std::move(originalScopedDonateChunk));
    ASSERT(movedScopedDonateChunk.mustExecute());

    originalScopedDonateChunk = std::move(movedScopedDonateChunk);
    ASSERT(originalScopedDonateChunk.mustExecute());

    // Need to signal the registered migration so the destructor doesn't invariant
    originalScopedDonateChunk.signalComplete(Status::OK());
}

TEST_F(MoveChunkRegistration, GetActiveMigrationNamespace) {
    ASSERT(!_registry.getActiveDonateChunkNss());

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");

    auto originalScopedDonateChunk =
        assertGet(_registry.registerDonateChunk(operationContext(), createMoveRangeRequest(nss)));

    ASSERT_EQ(nss, _registry.getActiveDonateChunkNss());

    // Need to signal the registered migration so the destructor doesn't invariant
    originalScopedDonateChunk.signalComplete(Status::OK());
}

TEST_F(MoveChunkRegistration, SecondMigrationReturnsConflictingOperationInProgress) {
    auto originalScopedDonateChunk = assertGet(_registry.registerDonateChunk(
        operationContext(),
        createMoveRangeRequest(
            NamespaceString::createNamespaceString_forTest("TestDB", "TestColl1"))));

    auto secondScopedDonateChunkStatus = _registry.registerDonateChunk(
        operationContext(),
        createMoveRangeRequest(
            NamespaceString::createNamespaceString_forTest("TestDB", "TestColl2")));
    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              secondScopedDonateChunkStatus.getStatus());

    originalScopedDonateChunk.signalComplete(Status::OK());
}

TEST_F(MoveChunkRegistration, SecondMigrationWithSameArgumentsJoinsFirst) {
    const auto epoch = OID::gen();

    auto swOriginalScopedDonateChunkPtr =
        std::make_unique<StatusWith<ScopedDonateChunk>>(_registry.registerDonateChunk(
            operationContext(),
            createMoveRangeRequest(
                NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"), epoch)));

    ASSERT_OK(swOriginalScopedDonateChunkPtr->getStatus());
    auto& originalScopedDonateChunk = swOriginalScopedDonateChunkPtr->getValue();
    ASSERT(originalScopedDonateChunk.mustExecute());

    auto secondScopedDonateChunk = assertGet(_registry.registerDonateChunk(
        operationContext(),
        createMoveRangeRequest(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
                               epoch)));
    ASSERT(!secondScopedDonateChunk.mustExecute());

    originalScopedDonateChunk.signalComplete({ErrorCodes::InternalError, "Test error"});
    swOriginalScopedDonateChunkPtr.reset();

    auto opCtx = operationContext();
    ASSERT_EQ(Status(ErrorCodes::InternalError, "Test error"),
              secondScopedDonateChunk.waitForCompletion(opCtx));
}

TEST_F(MoveChunkRegistration, TestDonateChunkIsRejectedWhenRegistryIsLocked) {
    _registry.lock(_opCtx, "dummy");
    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              _registry.registerDonateChunk(
                  _opCtx,
                  createMoveRangeRequest(
                      NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"))));
    _registry.unlock("dummy");
}

TEST_F(MoveChunkRegistration, TestReceiveChunkIsRejectedWhenRegistryIsLocked) {
    _registry.lock(_opCtx, "dummy");
    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              _registry.registerReceiveChunk(
                  _opCtx,
                  NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
                  ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
                  ShardId("shard0001"),
                  false /* waitForCompletionOfConflictingOps */));
    _registry.unlock("dummy");
}


TEST_F(MoveChunkRegistration,
       TestReceiveChunkWithWaitForConflictingOpsIsBlockedWhenRegistryIsLocked) {
    nolint_promise<void> blockReceive;
    nolint_promise<void> readyToLock;
    nolint_promise<void> inLock;

    // Registry thread.
    auto result = stdx::async(stdx::launch::async, [&] {
        // 2. Lock the registry so that starting to receive will block.
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext()->getService());
        auto opCtxHolder = tc->makeOperationContext();
        opCtxHolder->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        _registry.lock(opCtxHolder.get(), "dummy");

        // 3. Signal the receive thread that the receive is ready to be started.
        readyToLock.set_value();

        // 4. Wait for the receive thread to start blocking because the registry is locked.
        blockReceive.get_future().wait();

        // 9. Unlock the registry to signal the receive thread.
        _registry.unlock("dummy");
    });

    // Receive thread.
    auto lockReleased = stdx::async(stdx::launch::async, [&] {
        ThreadClient tc("receive thread", getGlobalServiceContext()->getService());
        auto opCtx = tc->makeOperationContext();

        auto baton = opCtx->getBaton();
        baton->schedule([&inLock](Status) {
            // 7. This is called when the receive is blocking. We let the test method know
            // that we're blocked on the receive so that it can tell the registry thread to unlock
            // the registry.
            inLock.set_value();
        });

        // 5. This is woken up by the registry thread.
        readyToLock.get_future().wait();

        // 6. Now that we're woken up by the registry thread, let's attempt to start to receive.
        // This will block and call the lambda set on the baton above.
        auto scopedReceiveChunk = _registry.registerReceiveChunk(
            opCtx.get(),
            NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
            ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
            ShardId("shard0001"),
            true /* waitForCompletionOfConflictingOps */);

        ASSERT_OK(scopedReceiveChunk.getStatus());

        // 10. Destroy the ScopedReceiveChunk and return.
    });

    // 1. Wait for the receive thread to start blocking.
    inLock.get_future().wait();

    // 8. Tell the registry thread to unlock the registry. That will signal the receive thread to
    // continue.
    blockReceive.set_value();

    // 11. The receive thread has returned and this future is set.
    lockReleased.wait();
}

// This test validates that the ActiveMigrationsRegistry lock will block while there is a donation
// in progress. The test will fail if any of the futures are not signalled indicating that some part
// of the sequence is not working correctly.
TEST_F(MoveChunkRegistration, TestBlockingWhileDonateInProgress) {
    nolint_promise<void> blockDonate;
    nolint_promise<void> readyToLock;
    nolint_promise<void> inLock;

    // Migration thread.
    auto result = stdx::async(stdx::launch::async, [&] {
        // 2. Start a migration so that the registry lock will block when acquired.
        auto scopedDonateChunk = _registry.registerDonateChunk(
            operationContext(),
            createMoveRangeRequest(
                NamespaceString::createNamespaceString_forTest("TestDB", "TestColl")));
        ASSERT_OK(scopedDonateChunk.getStatus());

        // 3. Signal the registry locking thread that the registry is ready to be locked.
        readyToLock.set_value();

        // 4. Wait for the registry thread to start blocking because there is an active donate.
        blockDonate.get_future().wait();

        scopedDonateChunk.getValue().signalComplete(Status::OK());

        // 9. Destroy the ScopedDonateChunk to signal the registy lock.
    });

    // Registry locking thread.
    auto lockReleased = stdx::async(stdx::launch::async, [&] {
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext()->getService());
        auto opCtxHolder = tc->makeOperationContext();
        opCtxHolder->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        auto baton = opCtxHolder->getBaton();
        baton->schedule([&inLock](Status) {
            // 7. This is called when the registry lock is blocking. We let the test method know
            // that we're blocked on the registry lock so that it tell the migration thread to let
            // the donate operation complete.
            inLock.set_value();
        });

        // 5. This is woken up by the migration thread.
        readyToLock.get_future().wait();

        // 6. Now that we're woken up by the migration thread, let's attempt to lock the registry.
        // This will block and call the lambda set on the baton above.
        _registry.lock(opCtxHolder.get(), "dummy");

        // 10. Unlock the registry and return.
        _registry.unlock("dummy");
    });

    // 1. Wait for registry lock to be acquired.
    inLock.get_future().wait();

    // 8. Let the donate operation complete so that the ScopedDonateChunk is destroyed. That will
    // signal the registry lock.
    blockDonate.set_value();

    // 11. The registy locking thread has returned and this future is set.
    lockReleased.wait();
}

// This test validates that the ActiveMigrationsRegistry lock will block while there is a receive
// in progress. The test will fail if any of the futures are not signalled indicating that some part
// of the sequence is not working correctly.
TEST_F(MoveChunkRegistration, TestBlockingWhileReceiveInProgress) {
    nolint_promise<void> blockReceive;
    nolint_promise<void> readyToLock;
    nolint_promise<void> inLock;

    // Migration thread.
    auto result = stdx::async(stdx::launch::async, [&] {
        // 2. Start a migration so that the registry lock will block when acquired.
        auto scopedReceiveChunk = _registry.registerReceiveChunk(
            operationContext(),
            NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
            ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
            ShardId("shard0001"),
            false);
        ASSERT_OK(scopedReceiveChunk.getStatus());

        // 3. Signal the registry locking thread that the registry is ready to be locked.
        readyToLock.set_value();

        // 4. Wait for the registry thread to start blocking because there is an active receive.
        blockReceive.get_future().wait();

        // 9. Destroy the scopedReceiveChunk to signal the registy lock.
    });

    // Registry locking thread.
    auto lockReleased = stdx::async(stdx::launch::async, [&] {
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext()->getService());
        auto opCtxHolder = tc->makeOperationContext();
        opCtxHolder->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        auto baton = opCtxHolder->getBaton();
        baton->schedule([&inLock](Status) {
            // 7. This is called when the registry lock is blocking. We let the test method know
            // that we're blocked on the registry lock so that it tell the migration thread to let
            // the receive operation complete.
            inLock.set_value();
        });

        // 5. This is woken up by the migration thread.
        readyToLock.get_future().wait();

        // 6. Now that we're woken up by the migration thread, let's attempt to lock the registry.
        // This will block and call the lambda set on the baton above.
        _registry.lock(opCtxHolder.get(), "dummy");

        // 10. Unlock the registry and return.
        _registry.unlock("dummy");
    });

    // 1. Wait for registry lock to be acquired.
    inLock.get_future().wait();

    // 8. Let the receive operation complete so that the scopedReceiveChunk is destroyed. That will
    // signal the registry lock.
    blockReceive.set_value();

    // 11. The registy locking thread has returned and this future is set.
    lockReleased.wait();
}

}  // namespace
}  // namespace mongo
