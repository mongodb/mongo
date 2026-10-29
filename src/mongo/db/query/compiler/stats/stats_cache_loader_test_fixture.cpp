// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/stats_cache_loader_test_fixture.h"

#include "mongo/db/client.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo::stats {

void StatsCacheLoaderTestFixture::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();
    _storage = std::make_unique<repl::StorageInterfaceImpl>();
    _opCtx = cc().makeOperationContext();

    // Set up ReplicationCoordinator and ensure that we are primary.
    auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    repl::ReplicationCoordinator::set(service, std::move(replCoord));

    // Set up oplog collection. If the WT storage engine is used, the oplog collection is expected
    // to exist when fetching the next opTime (LocalOplogInfo::getNextOpTimes) to use for a write.
    repl::createOplog(operationContext());
}

void StatsCacheLoaderTestFixture::tearDown() {
    _storage.reset();
    _opCtx.reset();

    // Tear down mongod.
    ServiceContextMongoDTest::tearDown();
}

OperationContext* StatsCacheLoaderTestFixture::operationContext() {
    return _opCtx.get();
}

repl::StorageInterface* StatsCacheLoaderTestFixture::storageInterface() {
    return _storage.get();
}

}  // namespace mongo::stats
