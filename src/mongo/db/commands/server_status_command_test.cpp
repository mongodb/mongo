/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>


namespace mongo {
namespace {

class MockSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;
    bool includeByDefault() const override {
        return true;
    }
    BSONObj generateSection(OperationContext*, const BSONElement&) const final {
        return {};
    }
};

void trySection(ServerStatusSectionRegistry& reg, ClusterRole role) {
    reg.addSection(std::make_unique<MockSection>("mock", role));
}

void trySections(std::vector<ClusterRole> sectionRoles) {
    ServerStatusSectionRegistry registry;
    for (auto role : sectionRoles)
        trySection(registry, role);
}

TEST(ServerStatusSectionTest, CompatibleRoles) {
    // Shard role only section compatible with shard role.
    MockSection shardOnlySection("mock", ClusterRole::ShardServer);
    ASSERT_TRUE(shardOnlySection.relevantTo(ClusterRole::ShardServer));

    // Router role only section compatible with router role.
    MockSection routerOnlySection("mock", ClusterRole::RouterServer);
    ASSERT_TRUE(routerOnlySection.relevantTo(ClusterRole::RouterServer));

    // Section applicable to shard and router roles compatible with each.
    ClusterRole bothRoles{ClusterRole::ShardServer, ClusterRole::RouterServer};
    MockSection routerAndShardSection("mock", bothRoles);
    ASSERT_TRUE(routerAndShardSection.relevantTo(ClusterRole::ShardServer));
    ASSERT_TRUE(routerAndShardSection.relevantTo(ClusterRole::RouterServer));
}

TEST(ServerStatusSectionTest, IncompatibleRoles) {
    // Shard role only section not compatible with router role.
    MockSection shardOnlySection("mock", ClusterRole::ShardServer);
    ASSERT_FALSE(shardOnlySection.relevantTo(ClusterRole::RouterServer));

    // Router role only section not compatible with shard role.
    MockSection routerOnlySection("mock", ClusterRole::RouterServer);
    ASSERT_FALSE(routerOnlySection.relevantTo(ClusterRole::ShardServer));
}

TEST(ServerStatusSectionRegistryTest, CanRegisterSectionsWithSameNameUnderDifferentRoles) {
    trySections({ClusterRole::ShardServer, ClusterRole::RouterServer});
}

DEATH_TEST(ServerStatusSectionRegistryTest,
           CannotRegisterSectionWithSameNameAndSameRole,
           "Duplicate ServerStatusSection") {
    trySections({ClusterRole::ShardServer, ClusterRole::ShardServer});
}

const ClusterRole bothRoles{ClusterRole::ShardServer, ClusterRole::RouterServer};

DEATH_TEST(ServerStatusSectionRegistryTest,
           CannotRegisterShardSectionWithSameNameAsShardAndRouterSection,
           "Duplicate ServerStatusSection") {
    trySections({bothRoles, ClusterRole::ShardServer});
}

DEATH_TEST(ServerStatusSectionRegistryTest,
           CannotRegisterRouterSectionWithSameNameAsShardAndRouterSection,
           "Duplicate ServerStatusSection") {
    trySections({bothRoles, ClusterRole::RouterServer});
}

}  // namespace
}  // namespace mongo
