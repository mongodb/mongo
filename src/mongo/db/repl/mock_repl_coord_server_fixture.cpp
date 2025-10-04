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

#include "mongo/db/repl/mock_repl_coord_server_fixture.h"

#include "mongo/client/connection_string.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
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
    AutoGetCollection coll(opCtx(), NamespaceString::kRsOplogNamespace, MODE_IX);
    ASSERT_TRUE(coll);

    WriteUnitOfWork wuow(opCtx());
    auto status = collection_internal::insertDocument(opCtx(),
                                                      *coll,
                                                      InsertStatement(entry.getEntry().toBSON()),
                                                      &CurOp::get(opCtx())->debug(),
                                                      /* fromMigrate */ false);
    ASSERT_OK(status);
    wuow.commit();
}

OperationContext* MockReplCoordServerFixture::opCtx() {
    return _opCtx.get();
}

}  // namespace mongo
