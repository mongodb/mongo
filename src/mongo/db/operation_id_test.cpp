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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/operation_id.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using UniqueOperationIdRegistryTestHarness =
    UniqueOperationIdRegistry::UniqueOperationIdRegistryTestHarness;

TEST(OperationIdTest, OperationIdIncrementsProperly) {
    auto registry = UniqueOperationIdRegistry::create();
    auto slot = registry->acquireSlot();
    ASSERT_EQ(slot.getId(), 1);

    {
        auto slot2 = registry->acquireSlot();
        ASSERT_EQ(slot2.getId(), 2);
    }

    ASSERT(UniqueOperationIdRegistryTestHarness::isActive(*registry, 1));
    ASSERT(!UniqueOperationIdRegistryTestHarness::isActive(*registry, 2));

    slot = registry->acquireSlot();
    ASSERT_EQ(slot.getId(), 3);

    ASSERT(!UniqueOperationIdRegistryTestHarness::isActive(*registry, 1));
    ASSERT(UniqueOperationIdRegistryTestHarness::isActive(*registry, 3));
}

TEST(OperationIdTest, OperationIdsAreUnique) {
    auto registry = UniqueOperationIdRegistry::create();
    auto slot = registry->acquireSlot();
    ASSERT_EQ(slot.getId(), 1);

    auto slot2 = registry->acquireSlot();
    ASSERT_EQ(slot2.getId(), 2);

    // If the registry's state points to a currently active operation ID, we will
    // find the next hole and issue that instead.
    UniqueOperationIdRegistryTestHarness::setNextOpId(*registry, 1);
    auto slot3 = registry->acquireSlot();
    ASSERT_EQ(slot3.getId(), 3);
    ASSERT(UniqueOperationIdRegistryTestHarness::isActive(*registry, 1));
    ASSERT(UniqueOperationIdRegistryTestHarness::isActive(*registry, 2));
    ASSERT(UniqueOperationIdRegistryTestHarness::isActive(*registry, 3));
}

TEST(OperationIdTest, OperationIdsAreMovable) {
    auto registry = UniqueOperationIdRegistry::create();
    auto slot = registry->acquireSlot();
    ASSERT_EQ(slot.getId(), 1);
    ASSERT(UniqueOperationIdRegistryTestHarness::isActive(*registry, 1));

    auto moveSlot(std::move(slot));
    ASSERT_EQ(moveSlot.getId(), 1);
    ASSERT(UniqueOperationIdRegistryTestHarness::isActive(*registry, 1));

    auto assignSlot = std::move(moveSlot);
    ASSERT_EQ(assignSlot.getId(), 1);
    ASSERT(UniqueOperationIdRegistryTestHarness::isActive(*registry, 1));
}

DEATH_TEST(OperationIdTest, TooManyTransactionsShouldCrash, "invariant") {
    auto registry = UniqueOperationIdRegistry::create();
    std::vector<OperationIdSlot> slots;

    while (true) {
        slots.push_back(registry->acquireSlot());
    }
}

}  // namespace

}  // namespace mongo
