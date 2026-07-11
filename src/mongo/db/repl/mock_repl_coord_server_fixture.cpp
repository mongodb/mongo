// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/mock_repl_coord_server_fixture.h"

#include "mongo/client/connection_string.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

#include <memory>

namespace mongo {

void MockReplCoordServerFixture::setUp() {
    ServiceContextMongoDTest::setUp();

    _opCtx = cc().makeOperationContext();

    auto service = getServiceContext();

    _storageInterface = new repl::StorageInterfaceMock();
    repl::StorageInterface::set(service,
                                std::unique_ptr<repl::StorageInterface>(_storageInterface));
    ASSERT_TRUE(_storageInterface == repl::StorageInterface::get(service));

    repl::ReplicationProcess::set(service,
                                  std::make_unique<repl::ReplicationProcess>(
                                      _storageInterface,
                                      std::make_unique<repl::ReplicationConsistencyMarkersMock>(),
                                      std::make_unique<repl::ReplicationRecoveryMock>()));

    ASSERT_OK(repl::ReplicationProcess::get(service)->initializeRollbackID(opCtx()));

    // Insert code path assumes existence of repl coordinator!
    repl::ReplSettings replSettings;
    replSettings.setReplSetString(
        ConnectionString::forReplicaSet("sessionTxnStateTest", {HostAndPort("a:1")}).toString());

    repl::ReplicationCoordinator::set(
        service, std::make_unique<repl::ReplicationCoordinatorMock>(service, replSettings));
    ASSERT_OK(
        repl::ReplicationCoordinator::get(service)->setFollowerMode(repl::MemberState::RS_PRIMARY));

    // Note: internal code does not allow implicit creation of non-capped oplog collection.
    DBDirectClient client(opCtx());
    ASSERT_TRUE(client.createCollection(NamespaceString::kRsOplogNamespace, 1024 * 1024, true));

    repl::acquireOplogCollectionForLogging(opCtx());

    // Set a committed snapshot so that we can perform majority reads.
    WriteUnitOfWork wuow{_opCtx.get()};
    if (auto snapshotManager =
            _opCtx->getServiceContext()->getStorageEngine()->getSnapshotManager()) {
        snapshotManager->setCommittedSnapshot(repl::getNextOpTime(_opCtx.get()).getTimestamp());
    }
    wuow.commit();
}

void MockReplCoordServerFixture::insertOplogEntry(const repl::OplogEntry& entry) {
    auto coll = acquireCollection(
        opCtx(),
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx(), NamespaceString::kRsOplogNamespace, AcquisitionPrerequisites::kWrite),
        MODE_IX);
    ASSERT_TRUE(coll.exists());

    WriteUnitOfWork wuow(opCtx());
    auto status = Helpers::insert(opCtx(), coll.getCollectionPtr(), entry.getEntry().toBSON());
    ASSERT_OK(status);
    wuow.commit();
}

OperationContext* MockReplCoordServerFixture::opCtx() {
    return _opCtx.get();
}

}  // namespace mongo
