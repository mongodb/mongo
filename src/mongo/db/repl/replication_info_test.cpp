// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_state.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace mongo {
namespace repl {
namespace {

constexpr int kWriteBlockUnknown = 0;
constexpr int kWriteBlockEnabled = 2;

class ReplicationInfoServerStatusTest : public ServiceContextMongoDTest {
public:
    ReplicationInfoServerStatusTest() : ServiceContextMongoDTest(Options{}.useReplSettings(true)) {}

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto* service = getServiceContext();

        // The fixture installs a mock with empty ReplSettings. Replace it so generateSection
        // treats this node as a replica set member.
        ReplSettings settings;
        settings.setReplSetString("rs0/localhost:27017");
        ReplicationCoordinator::set(
            service, std::make_unique<ReplicationCoordinatorMock>(service, settings));

        _storage = std::make_unique<StorageInterfaceMock>();
        ReplicationProcess::set(service,
                                std::make_unique<ReplicationProcess>(
                                    _storage.get(),
                                    std::make_unique<ReplicationConsistencyMarkersMock>(),
                                    std::make_unique<ReplicationRecoveryMock>()));
    }

protected:
    ServerStatusSection* replSection() {
        auto* registry = ServerStatusSectionRegistry::instance();
        auto sectionIt = std::find_if(registry->begin(), registry->end(), [](const auto& entry) {
            return entry.second->getSectionName() == "repl";
        });
        ASSERT(sectionIt != registry->end());
        return sectionIt->second.get();
    }

    BSONObj generateReplSection(OperationContext* opCtx) {
        return replSection()->generateSection(opCtx, BSONElement{});
    }

    BSONObj generateReplSectionWhileHoldingGlobalX() {
        auto lockClient = getServiceContext()->getService()->makeClient("replInfoLockHolder");
        auto lockOpCtx = lockClient->makeOperationContext();
        Lock::GlobalLock globalX(lockOpCtx.get(), MODE_X);
        ASSERT(globalX.isLocked());

        auto statusOpCtx = cc().makeOperationContext();
        return generateReplSection(statusOpCtx.get());
    }

    std::unique_ptr<StorageInterfaceMock> _storage;
};

TEST_F(ReplicationInfoServerStatusTest, AllowDeletionsFalseWhenDeletionsBlockedAndLockAvailable) {
    auto opCtx = cc().makeOperationContext();
    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    state->enableReplicaSetDeletionsBlocking();

    const auto status = generateReplSection(opCtx.get());
    ASSERT_EQ(kWriteBlockEnabled, status["replicaSetWritesBlock"].numberInt());
    ASSERT_FALSE(status["replicaSetWritesBlockAllowDeletions"].boolean());
}

TEST_F(ReplicationInfoServerStatusTest,
       AllowDeletionsFalseWhenDeletionsBlockedAndGlobalLockUnavailable) {
    auto* state = ReplicaSetWriteBlockState::get(getServiceContext());
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    state->enableReplicaSetDeletionsBlocking();

    const auto status = generateReplSectionWhileHoldingGlobalX();
    ASSERT_EQ(kWriteBlockUnknown, status["replicaSetWritesBlock"].numberInt());
    ASSERT_EQ(kWriteBlockUnknown, status["userWriteBlockMode"].numberInt());
    ASSERT_FALSE(status.hasField("replicaSetWritesBlockReason"));
    ASSERT_FALSE(status["replicaSetWritesBlockAllowDeletions"].boolean());
}

TEST_F(ReplicationInfoServerStatusTest,
       AllowDeletionsTrueWhenDeletionsUnblockedAndGlobalLockUnavailable) {
    auto* state = ReplicaSetWriteBlockState::get(getServiceContext());
    state->disableReplicaSetWriteBlocking();
    state->disableReplicaSetDeletionsBlocking();

    const auto status = generateReplSectionWhileHoldingGlobalX();
    ASSERT_EQ(kWriteBlockUnknown, status["replicaSetWritesBlock"].numberInt());
    ASSERT_TRUE(status["replicaSetWritesBlockAllowDeletions"].boolean());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
