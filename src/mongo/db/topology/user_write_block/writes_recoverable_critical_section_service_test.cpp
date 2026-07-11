// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/user_write_block/writes_recoverable_critical_section_service.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/ddl/create_gen.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_bypass.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_state.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class WritesRecoverableCriticalSectionServiceTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        auto* serviceContext = getServiceContext();
        auto opCtx = cc().makeOperationContext();
        repl::StorageInterface::set(serviceContext, std::make_unique<repl::StorageInterfaceMock>());

        repl::ReplicationCoordinator::set(serviceContext,
                                          std::make_unique<repl::ReplicationCoordinatorMock>(
                                              serviceContext, createReplSettings()));
        repl::createOplog(opCtx.get());

        auto* replCoord = repl::ReplicationCoordinator::get(opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

        ASSERT_OK(createCollection(
            opCtx.get(),
            CreateCommand(NamespaceString::kReplicaSetWritesCriticalSectionsNamespace)));
    }

    void resetPersistedCriticalSectionAndMemory(OperationContext* opCtx) {
        auto* service = UserWritesRecoverableCriticalSectionService::get(opCtx);
        service->releaseRecoverableCriticalSectionBlockingReplicaSetWrites(
            opCtx,
            UserWritesRecoverableCriticalSectionService::kBlockReplicaSetWritesNamespace,
            ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);

        Lock::GlobalLock globalLock(opCtx, MODE_IX);
        auto* rsState = ReplicaSetWriteBlockState::get(opCtx);
        rsState->disableReplicaSetWriteBlocking();
        rsState->disableReplicaSetDeletionsBlocking();
        ReplicaSetWriteBlockBypass::get(opCtx).set(false);
    }

private:
    // Creates a reasonable set of ReplSettings for most tests.
    repl::ReplSettings createReplSettings() {
        repl::ReplSettings settings;
        settings.setOplogSizeBytes(5 * 1024 * 1024);
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }
};

TEST_F(WritesRecoverableCriticalSectionServiceTest,
       RecoverReplicaSetWritesCriticalSectionAllowDeletionsTrueDoesNotEnableDeletionBlocking) {
    auto opCtx = cc().makeOperationContext();
    resetPersistedCriticalSectionAndMemory(opCtx.get());

    UserWritesRecoverableCriticalSectionService::get(opCtx.get())
        ->acquireRecoverableCriticalSectionBlockingReplicaSetWrites(
            opCtx.get(),
            UserWritesRecoverableCriticalSectionService::kBlockReplicaSetWritesNamespace,
            true /* allowDeletions */,
            ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);

    UserWritesRecoverableCriticalSectionService::get(opCtx.get())
        ->recoverRecoverableCriticalSections(opCtx.get());

    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    ASSERT_TRUE(state->isReplicaSetWriteBlockingEnabled());
    ASSERT_FALSE(state->isReplicaSetDeletionsBlockingEnabled_forTest());

    const auto userNss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetDeletionsAllowed(opCtx.get(), userNss));
}

TEST_F(WritesRecoverableCriticalSectionServiceTest,
       RecoverReplicaSetWritesCriticalSectionAllowDeletionsFalseEnablesDeletionBlocking) {
    auto opCtx = cc().makeOperationContext();
    resetPersistedCriticalSectionAndMemory(opCtx.get());

    UserWritesRecoverableCriticalSectionService::get(opCtx.get())
        ->acquireRecoverableCriticalSectionBlockingReplicaSetWrites(
            opCtx.get(),
            UserWritesRecoverableCriticalSectionService::kBlockReplicaSetWritesNamespace,
            false /* allowDeletions */,
            ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);

    UserWritesRecoverableCriticalSectionService::get(opCtx.get())
        ->recoverRecoverableCriticalSections(opCtx.get());

    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    ASSERT_TRUE(state->isReplicaSetWriteBlockingEnabled());
    ASSERT_TRUE(state->isReplicaSetDeletionsBlockingEnabled_forTest());

    const auto userNss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_THROWS_CODE(state->checkReplicaSetDeletionsAllowed(opCtx.get(), userNss),
                       AssertionException,
                       ErrorCodes::ReplicaSetWritesBlocked);
}

}  // namespace
}  // namespace mongo
