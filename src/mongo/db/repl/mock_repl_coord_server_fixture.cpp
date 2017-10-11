/**
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/memory.h"

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
                                  stdx::make_unique<repl::ReplicationProcess>(
                                      _storageInterface,
                                      stdx::make_unique<repl::ReplicationConsistencyMarkersMock>(),
                                      stdx::make_unique<repl::ReplicationRecoveryMock>()));

    ASSERT_OK(repl::ReplicationProcess::get(service)->initializeRollbackID(opCtx()));

    // Insert code path assumes existence of repl coordinator!
    repl::ReplSettings replSettings;
    replSettings.setReplSetString(
        ConnectionString::forReplicaSet("sessionTxnStateTest", {HostAndPort("a:1")}).toString());
    replSettings.setMaster(true);

    repl::ReplicationCoordinator::set(
        service, stdx::make_unique<repl::ReplicationCoordinatorMock>(service, replSettings));

    // Note: internal code does not allow implicit creation of non-capped oplog collection.
    DBDirectClient client(opCtx());
    ASSERT_TRUE(
        client.createCollection(NamespaceString::kRsOplogNamespace.ns(), 1024 * 1024, true));

    repl::setOplogCollectionName();
    repl::acquireOplogCollectionForLogging(opCtx());
}

void MockReplCoordServerFixture::tearDown() {
    // ServiceContextMongoDTest::tearDown() will try to create it's own opCtx, and it's not
    // allowed to have 2 present per client, so destroy this one.
    _opCtx.reset();

    ServiceContextMongoDTest::tearDown();
}

void MockReplCoordServerFixture::insertOplogEntry(const repl::OplogEntry& entry) {
    AutoGetCollection autoColl(opCtx(), NamespaceString::kRsOplogNamespace, MODE_IX);
    auto coll = autoColl.getCollection();
    ASSERT_TRUE(coll != nullptr);

    auto status = coll->insertDocument(opCtx(),
                                       InsertStatement(entry.toBSON()),
                                       &CurOp::get(opCtx())->debug(),
                                       /* enforceQuota */ false,
                                       /* fromMigrate */ false);
    ASSERT_OK(status);
}

OperationContext* MockReplCoordServerFixture::opCtx() {
    return _opCtx.get();
}

}  // namespace mongo
