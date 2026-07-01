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
#include "mongo/db/baton.h"
#include "mongo/db/client.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/version_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/version/releases.h"

#include <future>
#include <mutex>
#include <system_error>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {

class ActiveMigrationsRegistryTestAccessor {
public:
    static ActiveMigrationsRegistry::BypassRecoveryWait makeRecoveryBypass() {
        return {};
    }
};

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

// The registry now takes the chained ShardsvrMoveRangeRequest plus a NamespaceString — the OpMsg
// envelope on ShardsvrMoveRange is not part of the migration intent and is not needed here.
ShardsvrMoveRangeRequest createMoveRangeRequestFields() {
    ShardsvrMoveRangeRequest req;
    req.getMoveRangeRequestBase().setToShard(ShardId("shard0002"));
    req.setCollectionTimestamp(Timestamp(10));
    req.setFromShard(ShardId("shard0001"));
    req.setMaxChunkSizeBytes(1024);
    req.getMoveRangeRequestBase().setMin(BSON("Key" << -100));
    req.getMoveRangeRequestBase().setMax(BSON("Key" << 100));
    return req;
}

TEST_F(MoveChunkRegistration, ScopedDonateChunkMoveConstructorAndAssignment) {
    auto originalScopedDonateChunk = assertGet(_registry.registerDonateChunk(
        _opCtx,
        NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
        createMoveRangeRequestFields()));
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

    auto originalScopedDonateChunk = assertGet(
        _registry.registerDonateChunk(operationContext(), nss, createMoveRangeRequestFields()));

    ASSERT_EQ(nss, _registry.getActiveDonateChunkNss());

    // Need to signal the registered migration so the destructor doesn't invariant
    originalScopedDonateChunk.signalComplete(Status::OK());
}

TEST_F(MoveChunkRegistration, SecondMigrationReturnsConflictingOperationInProgress) {
    auto originalScopedDonateChunk = assertGet(_registry.registerDonateChunk(
        operationContext(),
        NamespaceString::createNamespaceString_forTest("TestDB", "TestColl1"),
        createMoveRangeRequestFields()));

    auto secondScopedDonateChunkStatus = _registry.registerDonateChunk(
        operationContext(),
        NamespaceString::createNamespaceString_forTest("TestDB", "TestColl2"),
        createMoveRangeRequestFields());
    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              secondScopedDonateChunkStatus.getStatus());

    originalScopedDonateChunk.signalComplete(Status::OK());
}

TEST_F(MoveChunkRegistration, SecondMigrationWithSameArgumentsJoinsFirst) {
    const auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");

    auto swOriginalScopedDonateChunkPtr = std::make_unique<StatusWith<ScopedDonateChunk>>(
        _registry.registerDonateChunk(operationContext(), nss, createMoveRangeRequestFields()));

    ASSERT_OK(swOriginalScopedDonateChunkPtr->getStatus());
    auto& originalScopedDonateChunk = swOriginalScopedDonateChunkPtr->getValue();
    ASSERT(originalScopedDonateChunk.mustExecute());

    auto secondScopedDonateChunk = assertGet(
        _registry.registerDonateChunk(operationContext(), nss, createMoveRangeRequestFields()));
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
                  NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
                  createMoveRangeRequestFields()));
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
                  false /* waitForCompletionOfMigrationOps */));
    _registry.unlock("dummy");
}

TEST_F(MoveChunkRegistration, TestSplitOrMergeChunkIsRejectedWhenRegistryIsLocked) {
    _registry.lock(_opCtx, "dummy");
    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              _registry.registerSplitOrMergeChunk(
                  _opCtx,
                  NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
                  ChunkRange(BSON("Key" << -100), BSON("Key" << 100))));
    _registry.unlock("dummy");
}

TEST_F(MoveChunkRegistration, SplitOrMergeBlocksWhileReceiveInProgressOnSameNss) {
    const auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    const ChunkRange chunkRange(BSON("Key" << -100), BSON("Key" << 100));

    nolint_promise<void> blockReceive;
    nolint_promise<void> readyToSplitMerge;
    nolint_promise<void> inSplitMerge;

    // Receive thread.
    auto result = std::async(std::launch::async, [&] {
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext()->getService());
        auto opCtxHolder = tc->makeOperationContext();
        opCtxHolder->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        // 2. Start a receive so that the split/merge will block when registered.
        auto scopedReceiveChunk =
            assertGet(_registry.registerReceiveChunk(opCtxHolder.get(),
                                                     nss,
                                                     chunkRange,
                                                     ShardId("shard0001"),
                                                     false /* waitForCompletionOfMigrationOps */));

        // 3. Signal the split/merge thread that the receive is registered.
        readyToSplitMerge.set_value();

        // 4. Wait for the split/merge thread to start blocking because there is an active receive.
        blockReceive.get_future().wait();

        // 9. Destroy the ScopedReceiveChunk to signal the split/merge thread.
    });

    // Split/merge thread.
    auto splitMergeCompleted = std::async(std::launch::async, [&] {
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext()->getService());
        auto opCtxHolder = tc->makeOperationContext();
        opCtxHolder->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        auto baton = opCtxHolder->getBaton();
        baton->schedule([&inSplitMerge](Status) {
            // 7. This is called when the split/merge is blocking. Let the test method know so it
            // can tell the receive thread to complete.
            inSplitMerge.set_value();
        });

        // 5. This is woken up by the receive thread.
        readyToSplitMerge.get_future().wait();

        // 6. Attempt to register the split/merge. This blocks until the receive completes and calls
        // the lambda set on the baton above.
        auto scopedSplitMergeChunk =
            _registry.registerSplitOrMergeChunk(opCtxHolder.get(), nss, chunkRange);

        // 10. The split/merge proceeds once the receive has completed.
        ASSERT_OK(scopedSplitMergeChunk.getStatus());
    });

    // 1. Wait for the split/merge thread to start blocking.
    inSplitMerge.get_future().wait();

    // 8. Let the receive complete so that the ScopedReceiveChunk is destroyed. That signals the
    // split/merge thread to continue.
    blockReceive.set_value();

    // 11. The split/merge thread has returned and this future is set.
    splitMergeCompleted.wait();
}

TEST_F(MoveChunkRegistration, SplitOrMergeAllowedWhileReceivingDifferentNss) {
    const auto receiveNss = NamespaceString::createNamespaceString_forTest("TestDB", "ReceiveColl");
    const auto splitNss = NamespaceString::createNamespaceString_forTest("TestDB", "SplitColl");
    const ChunkRange chunkRange(BSON("Key" << -100), BSON("Key" << 100));

    // A receive of one namespace does not block a split/merge of a different namespace.
    auto scopedReceiveChunk =
        assertGet(_registry.registerReceiveChunk(_opCtx,
                                                 receiveNss,
                                                 chunkRange,
                                                 ShardId("shard0001"),
                                                 false /* waitForCompletionOfMigrationOps */));

    ASSERT_OK(_registry.registerSplitOrMergeChunk(_opCtx, splitNss, chunkRange).getStatus());
}

TEST_F(MoveChunkRegistration, ReceiveBlocksWhileSplitOrMergeInProgressOnSameNss) {
    const auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    const ChunkRange chunkRange(BSON("Key" << -100), BSON("Key" << 100));

    nolint_promise<void> blockSplitMerge;
    nolint_promise<void> readyToReceive;
    nolint_promise<void> inReceive;

    // Split/merge thread.
    auto result = std::async(std::launch::async, [&] {
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext()->getService());
        auto opCtxHolder = tc->makeOperationContext();
        opCtxHolder->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        // 2. Start a split/merge so that the receive will block when registered.
        auto scopedSplitMergeChunk =
            assertGet(_registry.registerSplitOrMergeChunk(opCtxHolder.get(), nss, chunkRange));

        // 3. Signal the receive thread that the split/merge is registered.
        readyToReceive.set_value();

        // 4. Wait for the receive thread to start blocking because there is an active split/merge.
        blockSplitMerge.get_future().wait();

        // 9. Destroy the ScopedSplitMergeChunk to signal the receive thread.
    });

    // Receive thread.
    auto receiveCompleted = std::async(std::launch::async, [&] {
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext()->getService());
        auto opCtxHolder = tc->makeOperationContext();
        opCtxHolder->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        auto baton = opCtxHolder->getBaton();
        baton->schedule([&inReceive](Status) {
            // 7. This is called when the receive is blocking. Let the test method know so it can
            // tell the split/merge thread to complete.
            inReceive.set_value();
        });

        // 5. This is woken up by the split/merge thread.
        readyToReceive.get_future().wait();

        // 6. Attempt to register the receive. This blocks until the split/merge completes and calls
        // the lambda set on the baton above.
        auto scopedReceiveChunk =
            _registry.registerReceiveChunk(opCtxHolder.get(),
                                           nss,
                                           chunkRange,
                                           ShardId("shard0001"),
                                           false /* waitForCompletionOfMigrationOps */);

        // 10. The receive proceeds once the split/merge has completed.
        ASSERT_OK(scopedReceiveChunk.getStatus());
    });

    // 1. Wait for the receive thread to start blocking.
    inReceive.get_future().wait();

    // 8. Let the split/merge complete so that the ScopedSplitMergeChunk is destroyed. That signals
    // the receive thread to continue.
    blockSplitMerge.set_value();

    // 11. The receive thread has returned and this future is set.
    receiveCompleted.wait();
}

TEST_F(MoveChunkRegistration, ReceiveAllowedWhileSplitOrMergeOnDifferentNss) {
    const auto splitNss = NamespaceString::createNamespaceString_forTest("TestDB", "SplitColl");
    const auto receiveNss = NamespaceString::createNamespaceString_forTest("TestDB", "ReceiveColl");
    const ChunkRange chunkRange(BSON("Key" << -100), BSON("Key" << 100));

    // A split/merge of one namespace does not block a receive of a different namespace.
    auto scopedSplitMergeChunk =
        assertGet(_registry.registerSplitOrMergeChunk(_opCtx, splitNss, chunkRange));

    ASSERT_OK(_registry
                  .registerReceiveChunk(_opCtx,
                                        receiveNss,
                                        chunkRange,
                                        ShardId("shard0001"),
                                        false /* waitForCompletionOfMigrationOps */)
                  .getStatus());
}

TEST_F(MoveChunkRegistration,
       TestReceiveChunkWithWaitForConflictingOpsIsBlockedWhenRegistryIsLocked) {
    nolint_promise<void> blockReceive;
    nolint_promise<void> readyToLock;
    nolint_promise<void> inLock;

    // Registry thread.
    auto result = std::async(std::launch::async, [&] {
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
    auto lockReleased = std::async(std::launch::async, [&] {
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
            true /* waitForCompletionOfMigrationOps */);

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
    auto result = std::async(std::launch::async, [&] {
        // 2. Start a migration so that the registry lock will block when acquired.
        auto scopedDonateChunk = _registry.registerDonateChunk(
            operationContext(),
            NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
            createMoveRangeRequestFields());
        ASSERT_OK(scopedDonateChunk.getStatus());

        // 3. Signal the registry locking thread that the registry is ready to be locked.
        readyToLock.set_value();

        // 4. Wait for the registry thread to start blocking because there is an active donate.
        blockDonate.get_future().wait();

        scopedDonateChunk.getValue().signalComplete(Status::OK());

        // 9. Destroy the ScopedDonateChunk to signal the registy lock.
    });

    // Registry locking thread.
    auto lockReleased = std::async(std::launch::async, [&] {
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
    auto result = std::async(std::launch::async, [&] {
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
    auto lockReleased = std::async(std::launch::async, [&] {
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

TEST_F(MoveChunkRegistration, TestBlockingWhileSplitOrMergeInProgress) {
    nolint_promise<void> blockSplitMerge;
    nolint_promise<void> readyToLock;
    nolint_promise<void> inLock;

    auto result = std::async(std::launch::async, [&] {
        auto scopedSplitMergeChunk = _registry.registerSplitOrMergeChunk(
            operationContext(),
            NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
            ChunkRange(BSON("Key" << -100), BSON("Key" << 100)));
        ASSERT_OK(scopedSplitMergeChunk.getStatus());

        readyToLock.set_value();
        blockSplitMerge.get_future().wait();
    });

    auto lockReleased = std::async(std::launch::async, [&] {
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext()->getService());
        auto opCtxHolder = tc->makeOperationContext();
        opCtxHolder->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        auto baton = opCtxHolder->getBaton();
        baton->schedule([&inLock](Status) { inLock.set_value(); });

        readyToLock.get_future().wait();
        _registry.lock(opCtxHolder.get(), "dummy");
        _registry.unlock("dummy");
    });

    inLock.get_future().wait();
    blockSplitMerge.set_value();
    lockReleased.wait();
}

TEST_F(MoveChunkRegistration, RegisterChunkOperationsRejectDuringFCVTransitionWithoutOperationFCV) {
    const auto originalFCV =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ScopeGuard restoreFCV([&] { serverGlobalParams.mutableFCV.setVersion(originalFCV); });
    // (Generic FCV reference): Put the server in the authoritative metadata transition state to
    // verify normal chunk registrations are rejected.
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);

    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              _registry.registerDonateChunk(
                  _opCtx,
                  NamespaceString::createNamespaceString_forTest("TestDB", "DonateColl"),
                  createMoveRangeRequestFields()));

    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              _registry.registerReceiveChunk(
                  _opCtx,
                  NamespaceString::createNamespaceString_forTest("TestDB", "ReceiveColl"),
                  ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
                  ShardId("shard0001"),
                  false /* waitForCompletionOfConflictingOps */));

    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              _registry.registerSplitOrMergeChunk(
                  _opCtx,
                  NamespaceString::createNamespaceString_forTest("TestDB", "SplitMergeColl"),
                  ChunkRange(BSON("Key" << -100), BSON("Key" << 100))));

    // (Generic FCV reference): Restore latest FCV to verify registration is accepted once the
    // transition condition is gone.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);

    auto scopedSplitMergeChunk = assertGet(_registry.registerSplitOrMergeChunk(
        _opCtx,
        NamespaceString::createNamespaceString_forTest("TestDB", "StableLatestColl"),
        ChunkRange(BSON("Key" << -100), BSON("Key" << 100))));
}

TEST_F(MoveChunkRegistration, RegisterChunkOperationsWithStableOperationFCVRejectDuringTransition) {
    const auto originalFCV =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ScopeGuard restoreFCV([&] { serverGlobalParams.mutableFCV.setVersion(originalFCV); });

    // (Generic FCV reference): Pin the operation to last LTS before transitioning the server FCV
    // to verify stale-OFCV registrations are still rejected outside recovery.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLastLTS);
    VersionContext::FixedOperationFCVRegion fixedOperationFCV(_opCtx);
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);

    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              _registry.registerDonateChunk(
                  _opCtx,
                  NamespaceString::createNamespaceString_forTest("TestDB", "DonateColl"),
                  createMoveRangeRequestFields()));

    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              _registry.registerReceiveChunk(
                  _opCtx,
                  NamespaceString::createNamespaceString_forTest("TestDB", "ReceiveColl"),
                  ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
                  ShardId("shard0001"),
                  false /* waitForCompletionOfConflictingOps */));

    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              _registry.registerSplitOrMergeChunk(
                  _opCtx,
                  NamespaceString::createNamespaceString_forTest("TestDB", "SplitMergeColl"),
                  ChunkRange(BSON("Key" << -100), BSON("Key" << 100))));
}

TEST_F(MoveChunkRegistration, RecoveryBypassWithStableOperationFCVAllowsRegistration) {
    const auto originalFCV =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ScopeGuard restoreFCV([&] { serverGlobalParams.mutableFCV.setVersion(originalFCV); });

    // (Generic FCV reference): Pin the operation to last LTS before transitioning the server FCV
    // to verify recovery-bypassed registrations may reacquire local state with the old OFCV.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLastLTS);
    VersionContext::FixedOperationFCVRegion fixedOperationFCV(_opCtx);
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);

    {
        auto scopedDonateChunk = assertGet(_registry.registerDonateChunk(
            _opCtx,
            NamespaceString::createNamespaceString_forTest("TestDB", "RecoveryDonateColl"),
            createMoveRangeRequestFields(),
            ActiveMigrationsRegistryTestAccessor::makeRecoveryBypass()));
        scopedDonateChunk.signalComplete(Status::OK());
    }

    {
        auto scopedReceiveChunk = assertGet(_registry.registerReceiveChunk(
            _opCtx,
            NamespaceString::createNamespaceString_forTest("TestDB", "RecoveryReceiveColl"),
            ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
            ShardId("shard0001"),
            false /* waitForCompletionOfConflictingOps */,
            ActiveMigrationsRegistryTestAccessor::makeRecoveryBypass()));
    }

    {
        auto scopedSplitMergeChunk = assertGet(_registry.registerSplitOrMergeChunk(
            _opCtx,
            NamespaceString::createNamespaceString_forTest("TestDB", "RecoverySplitMergeColl"),
            ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
            ActiveMigrationsRegistryTestAccessor::makeRecoveryBypass()));
    }
}

// Fake Recoverable that gates waitForRecovery() on an externally-settable flag. Used to verify
// that ActiveMigrationsRegistry lock-acquisition methods block on recovery before touching state,
// per SERVER-125057.
//
// waitForRecovery() fulfils _enteredWait as its first action, before going into the condvar wait.
// Tests retrieve that future via whenEnteredWait() and block on it before calling markRecovered()
// (or markKilled() on the worker's opCtx). That guarantees the worker is actually inside
// waitForRecovery() at the point the test signals, eliminating the race the older
// baton-scheduled "waiting" promise had — which fired before the registry call even started, so
// the test could unblock and signal the recoverable while the worker was still ramping up.
class FakeRecoverable : public ActiveMigrationsRegistry::Recoverable {
public:
    void waitForRecovery(OperationContext* opCtx) const override {
        _enteredWait.set_value();
        std::unique_lock<std::mutex> lk(_mutex);
        opCtx->waitForConditionOrInterrupt(_cv, lk, [this] { return _recovered; });
    }

    // Resolves once a worker thread has entered waitForRecovery(). Must be called at most once
    // per FakeRecoverable instance, since it consumes the underlying promise's future.
    std::future<void> whenEnteredWait() {
        return _enteredWait.get_future();
    }

    void markRecovered() {
        {
            std::lock_guard<std::mutex> lk(_mutex);
            _recovered = true;
        }
        _cv.notify_all();
    }

private:
    mutable nolint_promise<void> _enteredWait;
    mutable std::mutex _mutex;
    mutable stdx::condition_variable _cv;
    bool _recovered{false};
};

// Verifies that registerDonateChunk blocks on waitForRecovery and only proceeds once the
// recoverable signals completion.
TEST_F(MoveChunkRegistration, RegisterDonateChunkBlocksOnRecovery) {
    FakeRecoverable recoverable;
    _registry.setRecoverable(&recoverable);
    auto enteredWait = recoverable.whenEnteredWait();

    auto result = std::async(std::launch::async, [&] {
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext()->getService());
        auto opCtxHolder = tc->makeOperationContext();
        opCtxHolder->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        auto scoped = _registry.registerDonateChunk(
            opCtxHolder.get(),
            NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
            createMoveRangeRequestFields());
        ASSERT_OK(scoped.getStatus());
        scoped.getValue().signalComplete(Status::OK());
    });

    // Confirm the worker thread is actually inside waitForRecovery before we unblock it.
    enteredWait.wait();
    recoverable.markRecovered();
    result.get();
}

// Verifies that registerReceiveChunk blocks on waitForRecovery.
TEST_F(MoveChunkRegistration, RegisterReceiveChunkBlocksOnRecovery) {
    FakeRecoverable recoverable;
    _registry.setRecoverable(&recoverable);
    auto enteredWait = recoverable.whenEnteredWait();

    auto result = std::async(std::launch::async, [&] {
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext()->getService());
        auto opCtxHolder = tc->makeOperationContext();
        opCtxHolder->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        auto scoped = _registry.registerReceiveChunk(
            opCtxHolder.get(),
            NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
            ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
            ShardId("shard0001"),
            false);
        ASSERT_OK(scoped.getStatus());
    });

    enteredWait.wait();
    recoverable.markRecovered();
    result.get();
}

// Verifies that registerSplitOrMergeChunk blocks on waitForRecovery.
TEST_F(MoveChunkRegistration, RegisterSplitOrMergeChunkBlocksOnRecovery) {
    FakeRecoverable recoverable;
    _registry.setRecoverable(&recoverable);
    auto enteredWait = recoverable.whenEnteredWait();

    auto result = std::async(std::launch::async, [&] {
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext()->getService());
        auto opCtxHolder = tc->makeOperationContext();
        opCtxHolder->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        auto scoped = _registry.registerSplitOrMergeChunk(
            opCtxHolder.get(),
            NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
            ChunkRange(BSON("Key" << -100), BSON("Key" << 100)));
        ASSERT_OK(scoped.getStatus());
    });

    enteredWait.wait();
    recoverable.markRecovered();
    result.get();
}

// Verifies that lock() blocks on waitForRecovery.
TEST_F(MoveChunkRegistration, LockBlocksOnRecovery) {
    FakeRecoverable recoverable;
    _registry.setRecoverable(&recoverable);
    auto enteredWait = recoverable.whenEnteredWait();

    auto result = std::async(std::launch::async, [&] {
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext()->getService());
        auto opCtxHolder = tc->makeOperationContext();
        opCtxHolder->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        _registry.lock(opCtxHolder.get(), "dummy");
        _registry.unlock("dummy");
    });

    enteredWait.wait();
    recoverable.markRecovered();
    result.get();
}

// Verifies that interrupting the opCtx while it is waiting on recovery propagates out of the
// registry call.
TEST_F(MoveChunkRegistration, RegisterDonateChunkInterruptedWhileWaitingOnRecovery) {
    FakeRecoverable recoverable;  // Never marked recovered.
    _registry.setRecoverable(&recoverable);
    auto enteredWait = recoverable.whenEnteredWait();

    nolint_promise<OperationContext*> workerOpCtxPromise;

    auto result = std::async(std::launch::async, [&] {
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext()->getService());
        auto opCtxHolder = tc->makeOperationContext();
        opCtxHolder->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        workerOpCtxPromise.set_value(opCtxHolder.get());

        ASSERT_THROWS_CODE(_registry.registerDonateChunk(
                               opCtxHolder.get(),
                               NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
                               createMoveRangeRequestFields()),
                           DBException,
                           ErrorCodes::Interrupted);
    });

    auto* workerOpCtx = workerOpCtxPromise.get_future().get();
    enteredWait.wait();
    workerOpCtx->markKilled(ErrorCodes::Interrupted);
    result.get();
}

}  // namespace
}  // namespace mongo
