// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/db_command_test_fixture.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

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

DEATH_TEST(ServerStatusSectionRegistryTestDeathTest,
           CannotRegisterSectionWithSameNameAndSameRole,
           "Duplicate ServerStatusSection") {
    trySections({ClusterRole::ShardServer, ClusterRole::ShardServer});
}

const ClusterRole bothRoles{ClusterRole::ShardServer, ClusterRole::RouterServer};

DEATH_TEST(ServerStatusSectionRegistryTestDeathTest,
           CannotRegisterShardSectionWithSameNameAsShardAndRouterSection,
           "Duplicate ServerStatusSection") {
    trySections({bothRoles, ClusterRole::ShardServer});
}

DEATH_TEST(ServerStatusSectionRegistryTestDeathTest,
           CannotRegisterRouterSectionWithSameNameAsShardAndRouterSection,
           "Duplicate ServerStatusSection") {
    trySections({bothRoles, ClusterRole::RouterServer});
}


class FailingSection : public ServerStatusSection {
    using ServerStatusSection::ServerStatusSection;
    bool includeByDefault() const override {
        return false;
    }
    BSONObj generateSection(OperationContext*, const BSONElement&) const final {
        invariant(false);
        return BSONObj();
    }
};

auto& fatalSection =
    *ServerStatusSectionBuilder<FailingSection>("failingSection").forShard().forRouter();

class ServerStatusCmdTest : public DBCommandTestFixture {};

/**
 * Ensure that the enableDiagnosticPrintingOnFailure feature emits the correct diagnostic
 * information on an invariant.
 */
using ServerStatusCmdTestDeathTest = ServerStatusCmdTest;
DEATH_TEST_REGEX_F(
    ServerStatusCmdTestDeathTest,
    CommandLogsDiagnosticsOnFailure,
    R"#(ScopedDebugInfo.*\'opDescription\': \{ serverStatus: 1, failingSection: 1.*)#") {
    runCommand(BSON("serverStatus" << 1 << "failingSection" << 1));
}

/**
 * Uses an auto-advancing mock fast clock so serverStatus exceeds the slow threshold and appends
 * timing without waiting on real wall clock.
 */
class ServerStatusSlowTimingTest : public ServiceContextMongoDTest {
public:
    ServerStatusSlowTimingTest()
        : ServiceContextMongoDTest(Options{}.useMockClock(true, Milliseconds{50})) {}

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        const auto service = getServiceContext();
        auto replCoord =
            std::make_unique<repl::ReplicationCoordinatorMock>(service, repl::ReplSettings{});
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));
        repl::createOplog(opCtx);
    }

    BSONObj runCommand(BSONObj cmd) {
        DBDirectClient client(opCtx);
        BSONObj result;
        ASSERT_TRUE(client.runCommand(DatabaseName::kAdmin, cmd, result)) << result;
        return result;
    }

    ServiceContext::UniqueOperationContext _uniqueOpCtx{makeOperationContext()};
    OperationContext* opCtx{_uniqueOpCtx.get()};
};

TEST_F(ServerStatusSlowTimingTest, TimingBreakdownSumsToTotal) {
    auto result = runCommand(BSON("serverStatus" << 1));
    ASSERT_TRUE(result.hasField("timing")) << result;
    BSONObj timing = result["timing"].Obj();

    ASSERT_TRUE(timing.hasField("after basic")) << timing;
    ASSERT_TRUE(timing.hasField("at end")) << timing;
    ASSERT_TRUE(timing.hasField("other")) << timing;
    ASSERT_TRUE(timing.hasField("after metrics")) << timing;

    long long sum = 0;
    for (const auto& el : timing) {
        ASSERT_TRUE(el.isNumber()) << el;
        if (el.fieldNameStringData() != "at end"sv) {
            sum += el.numberLong();
        }
    }
    ASSERT_EQ(sum, timing["at end"].numberLong()) << timing;
}

}  // namespace
}  // namespace mongo
