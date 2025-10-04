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

#include "mongo/db/repl/replication_process.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <fmt/format.h>

namespace {

using namespace mongo;
using namespace mongo::repl;

class ReplicationProcessTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    std::unique_ptr<StorageInterface> _storageInterface;
};

void ReplicationProcessTest::setUp() {
    ServiceContextMongoDTest::setUp();
    _storageInterface = std::make_unique<StorageInterfaceImpl>();
    auto service = getServiceContext();
    ReplicationCoordinator::set(service, std::make_unique<ReplicationCoordinatorMock>(service));
}

void ReplicationProcessTest::tearDown() {
    _storageInterface = {};
    ServiceContextMongoDTest::tearDown();
}

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

TEST_F(ReplicationProcessTest, ServiceContextDecorator) {
    auto serviceContext = getServiceContext();
    ASSERT_FALSE(ReplicationProcess::get(serviceContext));
    ReplicationProcess* replicationProcess = new ReplicationProcess(
        _storageInterface.get(),
        std::make_unique<ReplicationConsistencyMarkersImpl>(_storageInterface.get()),
        std::make_unique<ReplicationRecoveryMock>());
    ReplicationProcess::set(serviceContext,
                            std::unique_ptr<ReplicationProcess>(replicationProcess));
    ASSERT_TRUE(replicationProcess == ReplicationProcess::get(serviceContext));
    ASSERT_TRUE(replicationProcess == ReplicationProcess::get(*serviceContext));
    ASSERT_TRUE(replicationProcess == ReplicationProcess::get(makeOpCtx().get()));
}

TEST_F(ReplicationProcessTest, RollbackIDIncrementsBy1) {
    auto opCtx = makeOpCtx();
    ReplicationProcess replicationProcess(
        _storageInterface.get(),
        std::make_unique<ReplicationConsistencyMarkersImpl>(_storageInterface.get()),
        std::make_unique<ReplicationRecoveryMock>());

    // We make no assumptions about the initial value of the rollback ID.
    ASSERT_OK(replicationProcess.initializeRollbackID(opCtx.get()));
    int initRBID = replicationProcess.getRollbackID();

    // Make sure the rollback ID is incremented by exactly 1.
    ASSERT_OK(replicationProcess.incrementRollbackID(opCtx.get()));
    int rbid = replicationProcess.getRollbackID();
    ASSERT_EQ(rbid, initRBID + 1);
}

TEST_F(ReplicationProcessTest, RefreshRollbackIDResetsCachedValueFromStorage) {
    auto opCtx = makeOpCtx();
    ReplicationProcess replicationProcess(
        _storageInterface.get(),
        std::make_unique<ReplicationConsistencyMarkersImpl>(_storageInterface.get()),
        std::make_unique<ReplicationRecoveryMock>());

    // RefreshRollbackID returns NamespaceNotFound if there is no rollback.id collection.
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, replicationProcess.refreshRollbackID(opCtx.get()));

    // We make no assumptions about the initial value of the rollback ID.
    ASSERT_OK(replicationProcess.initializeRollbackID(opCtx.get()));
    int initRBID = replicationProcess.getRollbackID();

    // Increment rollback ID on disk. Cached value should different from storage.
    int storageRBID = unittest::assertGet(_storageInterface->incrementRollbackID(opCtx.get()));
    ASSERT_EQUALS(storageRBID, initRBID + 1);
    ASSERT_EQUALS(initRBID, replicationProcess.getRollbackID());

    // Refresh cached value and check cached value against storage again.
    ASSERT_OK(replicationProcess.refreshRollbackID(opCtx.get()));
    ASSERT_EQUALS(storageRBID, replicationProcess.getRollbackID());
}

}  // namespace
