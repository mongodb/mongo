/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/serverless/serverless_operation_lock_registry.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(ServerlessOperationLockRegistryTest, InsertRemoveOne) {
    ServerlessOperationLockRegistry registry;

    auto id = UUID::gen();
    registry.acquireLock(ServerlessOperationLockRegistry::LockType::kShardSplit, id);
    registry.releaseLock(ServerlessOperationLockRegistry::LockType::kShardSplit, id);

    ASSERT_FALSE(registry.getActiveOperationType_forTest());
}

DEATH_TEST(ServerlessOperationLockRegistryTest,
           InsertSameIdTwice,
           "Cannot acquire the serverless lock twice for the same operationId.") {
    ServerlessOperationLockRegistry registry;

    auto id = UUID::gen();
    registry.acquireLock(ServerlessOperationLockRegistry::LockType::kShardSplit, id);
    registry.acquireLock(ServerlessOperationLockRegistry::LockType::kShardSplit, id);
}

TEST(ServerlessOperationLockRegistryTest, AcquireDifferentNamespaceFail) {
    ServerlessOperationLockRegistry registry;

    auto id = UUID::gen();
    registry.acquireLock(ServerlessOperationLockRegistry::LockType::kShardSplit, id);

    ASSERT_THROWS_CODE(
        registry.acquireLock(ServerlessOperationLockRegistry::LockType::kTenantDonor, UUID::gen()),
        DBException,
        ErrorCodes::ConflictingServerlessOperation);
}

DEATH_TEST(ServerlessOperationLockRegistryTest,
           ReleaseDifferentNsTriggersInvariant,
           "Cannot release a serverless lock that is not owned by the given lock type.") {
    ServerlessOperationLockRegistry registry;

    auto id = UUID::gen();
    registry.acquireLock(ServerlessOperationLockRegistry::LockType::kShardSplit, id);
    registry.releaseLock(ServerlessOperationLockRegistry::LockType::kTenantDonor, id);
}


DEATH_TEST(ServerlessOperationLockRegistryTest,
           ReleaseDifferentIdTriggersInvariant,
           "Cannot release a serverless lock if the given operationId does not own the lock.") {
    ServerlessOperationLockRegistry registry;

    registry.acquireLock(ServerlessOperationLockRegistry::LockType::kShardSplit, UUID::gen());
    registry.releaseLock(ServerlessOperationLockRegistry::LockType::kShardSplit, UUID::gen());
}

TEST(ServerlessOperationLockRegistryTest, ClearReleasesAllLocks) {
    ServerlessOperationLockRegistry registry;

    registry.acquireLock(ServerlessOperationLockRegistry::LockType::kShardSplit, UUID::gen());
    registry.acquireLock(ServerlessOperationLockRegistry::LockType::kShardSplit, UUID::gen());

    registry.clear();

    // Verify the lock has been released.
    ASSERT_FALSE(registry.getActiveOperationType_forTest());
}

TEST(ServerlessOperationLockRegistryTest, LockIsReleasedWhenAllInstanceAreRemoved) {
    ServerlessOperationLockRegistry registry;

    std::vector<UUID> ids;
    for (int i = 0; i < 5; ++i) {
        ids.push_back(UUID::gen());
    }

    for (auto& id : ids) {
        registry.acquireLock(ServerlessOperationLockRegistry::LockType::kShardSplit, id);
    }

    // Verify the lock is held;
    ASSERT_EQ(*registry.getActiveOperationType_forTest(),
              ServerlessOperationLockRegistry::LockType::kShardSplit);


    for (auto& id : ids) {
        registry.releaseLock(ServerlessOperationLockRegistry::LockType::kShardSplit, id);
    }

    // Verify the lock has been released.
    ASSERT_FALSE(registry.getActiveOperationType_forTest());
}

TEST(ServerlessOperationLockRegistryTest, LockIsNotReleasedWhenNotAllInstanceAreRemoved) {
    ServerlessOperationLockRegistry registry;

    std::vector<UUID> ids;
    for (int i = 0; i < 5; ++i) {
        ids.push_back(UUID::gen());
    }

    for (auto& id : ids) {
        registry.acquireLock(ServerlessOperationLockRegistry::LockType::kShardSplit, id);
    }
    // Add an additional id;
    registry.acquireLock(ServerlessOperationLockRegistry::LockType::kShardSplit, UUID::gen());

    // Verify the lock is held;
    ASSERT_EQ(*registry.getActiveOperationType_forTest(),
              ServerlessOperationLockRegistry::LockType::kShardSplit);

    for (auto& id : ids) {
        registry.releaseLock(ServerlessOperationLockRegistry::LockType::kShardSplit, id);
    }

    // Verify the lock is held;
    ASSERT_EQ(*registry.getActiveOperationType_forTest(),
              ServerlessOperationLockRegistry::LockType::kShardSplit);
}


}  // namespace mongo
