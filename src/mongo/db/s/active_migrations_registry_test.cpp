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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/s/request_types/move_chunk_request.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

class MoveChunkRegistration : public ServiceContextMongoDTest {
public:
    void setUp() override {
        _opCtx = getClient()->makeOperationContext();
    }

    OperationContext* operationContext() {
        return _opCtx.get();
    }

protected:
    ActiveMigrationsRegistry _registry;
    ServiceContext::UniqueOperationContext _opCtx;
};

MoveChunkRequest createMoveChunkRequest(const NamespaceString& nss) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    BSONObjBuilder builder;
    MoveChunkRequest::appendAsCommand(
        &builder,
        nss,
        chunkVersion,
        assertGet(ConnectionString::parse("TestConfigRS/CS1:12345,CS2:12345,CS3:12345")),
        ShardId("shard0001"),
        ShardId("shard0002"),
        ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
        1024,
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        true,
        MoveChunkRequest::ForceJumbo::kDoNotForce);
    return assertGet(MoveChunkRequest::createFromCommand(nss, builder.obj()));
}

TEST_F(MoveChunkRegistration, ScopedDonateChunkMoveConstructorAndAssignment) {
    auto originalScopedDonateChunk = assertGet(_registry.registerDonateChunk(
        createMoveChunkRequest(NamespaceString("TestDB", "TestColl"))));
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

    const NamespaceString nss("TestDB", "TestColl");

    auto originalScopedDonateChunk =
        assertGet(_registry.registerDonateChunk(createMoveChunkRequest(nss)));

    ASSERT_EQ(nss.ns(), _registry.getActiveDonateChunkNss()->ns());

    // Need to signal the registered migration so the destructor doesn't invariant
    originalScopedDonateChunk.signalComplete(Status::OK());
}

TEST_F(MoveChunkRegistration, SecondMigrationReturnsConflictingOperationInProgress) {
    auto originalScopedDonateChunk = assertGet(_registry.registerDonateChunk(
        createMoveChunkRequest(NamespaceString("TestDB", "TestColl1"))));

    auto secondScopedDonateChunkStatus = _registry.registerDonateChunk(
        createMoveChunkRequest(NamespaceString("TestDB", "TestColl2")));
    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress,
              secondScopedDonateChunkStatus.getStatus());

    originalScopedDonateChunk.signalComplete(Status::OK());
}

TEST_F(MoveChunkRegistration, SecondMigrationWithSameArgumentsJoinsFirst) {
    auto originalScopedDonateChunk = assertGet(_registry.registerDonateChunk(
        createMoveChunkRequest(NamespaceString("TestDB", "TestColl"))));
    ASSERT(originalScopedDonateChunk.mustExecute());

    auto secondScopedDonateChunk = assertGet(_registry.registerDonateChunk(
        createMoveChunkRequest(NamespaceString("TestDB", "TestColl"))));
    ASSERT(!secondScopedDonateChunk.mustExecute());

    originalScopedDonateChunk.signalComplete({ErrorCodes::InternalError, "Test error"});
    auto opCtx = operationContext();
    ASSERT_EQ(Status(ErrorCodes::InternalError, "Test error"),
              secondScopedDonateChunk.waitForCompletion(opCtx));
}

TEST_F(MoveChunkRegistration, TestBlockingDonateChunk) {
    auto opCtx = operationContext();

    _registry.lock(opCtx);

    auto scopedDonateChunk = _registry.registerDonateChunk(
        createMoveChunkRequest(NamespaceString("TestDB", "TestColl")));

    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, scopedDonateChunk.getStatus());

    _registry.unlock();

    auto scopedDonateChunk2 = _registry.registerDonateChunk(
        createMoveChunkRequest(NamespaceString("TestDB", "TestColl")));
    ASSERT_OK(scopedDonateChunk2.getStatus());

    scopedDonateChunk2.getValue().signalComplete(Status::OK());
}

TEST_F(MoveChunkRegistration, TestBlockingReceiveChunk) {
    auto opCtx = operationContext();

    _registry.lock(opCtx);

    auto scopedReceiveChunk =
        _registry.registerReceiveChunk(NamespaceString("TestDB", "TestColl"),
                                       ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
                                       ShardId("shard0001"));

    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, scopedReceiveChunk.getStatus());

    _registry.unlock();

    auto scopedReceiveChunk2 =
        _registry.registerReceiveChunk(NamespaceString("TestDB", "TestColl"),
                                       ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
                                       ShardId("shard0001"));

    ASSERT_OK(scopedReceiveChunk2.getStatus());
}

// This test validates that the ActiveMigrationsRegistry lock will block while there is a donation
// in progress. The test will fail if any of the futures are not signalled indicating that some part
// of the sequence is not working correctly.
TEST_F(MoveChunkRegistration, TestBlockingWhileDonateInProgress) {
    stdx::promise<void> blockDonate;
    stdx::promise<void> readyToLock;
    stdx::promise<void> inLock;

    // Migration thread.
    auto result = stdx::async(stdx::launch::async, [&] {
        // 2. Start a migration so that the registry lock will block when acquired.
        auto scopedDonateChunk = _registry.registerDonateChunk(
            createMoveChunkRequest(NamespaceString("TestDB", "TestColl")));
        ASSERT_OK(scopedDonateChunk.getStatus());

        // 3. Signal the registry locking thread that the registry is ready to be locked.
        readyToLock.set_value();

        blockDonate.get_future().wait();

        scopedDonateChunk.getValue().signalComplete(Status::OK());

        // 8. Destroy the ScopedDonateChunk to signal the registy lock.
    });

    // Registry locking thread.
    auto lockReleased = stdx::async(stdx::launch::async, [&] {
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext());
        auto opCtx = tc->makeOperationContext();

        auto baton = opCtx->getBaton();
        baton->schedule([&inLock](Status) {
            // 6. This is called when the registry lock is blocking. We let the test method know
            // that we're blocked on the registry lock so that it tell the migration thread to let
            // the donate operation complete.
            inLock.set_value();
        });

        // 4. This is woken up by the migration thread.
        readyToLock.get_future().wait();

        // 5. Now that we're woken up by the migration thread, let's attempt to lock the registry.
        // This will block and call the lambda set on the baton above.
        _registry.lock(opCtx.get());

        // 9. Unlock the registry and return.
        _registry.unlock();
    });

    // 1. Wait for registry lock to be acquired.
    inLock.get_future().wait();

    // 7. Let the donate operation complete so that the ScopedDonateChunk is destroyed. That will
    // signal the registry lock.
    blockDonate.set_value();

    // 10. The registy locking thread has returned and this future is set.
    lockReleased.wait();
}

// This test validates that the ActiveMigrationsRegistry lock will block while there is a receive
// in progress. The test will fail if any of the futures are not signalled indicating that some part
// of the sequence is not working correctly.
TEST_F(MoveChunkRegistration, TestBlockingWhileReceiveInProgress) {
    stdx::promise<void> blockReceive;
    stdx::promise<void> readyToLock;
    stdx::promise<void> inLock;

    // Migration thread.
    auto result = stdx::async(stdx::launch::async, [&] {
        // 2. Start a migration so that the registry lock will block when acquired.
        auto scopedReceiveChunk =
            _registry.registerReceiveChunk(NamespaceString("TestDB", "TestColl"),
                                           ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
                                           ShardId("shard0001"));
        ASSERT_OK(scopedReceiveChunk.getStatus());

        // 3. Signal the registry locking thread that the registry is ready to be locked.
        readyToLock.set_value();

        blockReceive.get_future().wait();

        // 8. Destroy the scopedReceiveChunk to signal the registy lock.
    });

    // Registry locking thread.
    auto lockReleased = stdx::async(stdx::launch::async, [&] {
        ThreadClient tc("ActiveMigrationsRegistryTest", getGlobalServiceContext());
        auto opCtx = tc->makeOperationContext();

        auto baton = opCtx->getBaton();
        baton->schedule([&inLock](Status) {
            // 6. This is called when the registry lock is blocking. We let the test method know
            // that we're blocked on the registry lock so that it tell the migration thread to let
            // the receive operation complete.
            inLock.set_value();
        });

        // 4. This is woken up by the migration thread.
        readyToLock.get_future().wait();

        // 5. Now that we're woken up by the migration thread, let's attempt to lock the registry.
        // This will block and call the lambda set on the baton above.
        _registry.lock(opCtx.get());

        // 9. Unlock the registry and return.
        _registry.unlock();
    });

    // 1. Wait for registry lock to be acquired.
    inLock.get_future().wait();

    // 7. Let the receive operation complete so that the scopedReceiveChunk is destroyed. That will
    // signal the registry lock.
    blockReceive.set_value();

    // 10. The registy locking thread has returned and this future is set.
    lockReleased.wait();
}

}  // namespace
}  // namespace mongo
