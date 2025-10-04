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

#include "mongo/db/local_catalog/catalog_test_fixture.h"

#include "mongo/db/client.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {

CatalogScopedGlobalServiceContextForTest::CatalogScopedGlobalServiceContextForTest(
    Options options, bool shouldSetupTL)
    : MongoDScopedGlobalServiceContextForTest(std::move(options), shouldSetupTL) {
    auto service = getServiceContext();
    auto setupClient = getService()->makeClient("CatalogSCTestCtor");
    AlternativeClientRegion acr(setupClient);

    // Set up ReplicationCoordinator and ensure that we are primary.
    auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    repl::ReplicationCoordinator::set(service, std::move(replCoord));

    // Setup ReplicationInterface
    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());

    // Set up oplog collection. If the WT storage engine is used, the oplog collection is expected
    // to exist when fetching the next opTime (LocalOplogInfo::getNextOpTimes) to use for a write.
    auto opCtx = cc().makeOperationContext();
    repl::createOplog(opCtx.get());
}

OperationContext* CatalogTestFixture::operationContext() const {
    return _opCtx.get();
}

void CatalogTestFixture::setUp() {
    _opCtx = getClient()->makeOperationContext();
}
void CatalogTestFixture::tearDown() {
    _opCtx.reset();
}

repl::StorageInterface* CatalogTestFixture::storageInterface() const {
    return repl::StorageInterface::get(getServiceContext());
}

ConsistentCollection CatalogTestFixture::makeConsistentCollection(const Collection* coll) const {
    return makeConsistentCollection(operationContext(), coll);
}

ConsistentCollection CatalogTestFixture::makeConsistentCollection(OperationContext* opCtx,
                                                                  const Collection* coll) const {
    return ConsistentCollection{opCtx, coll};
}

int CatalogTestFixture::getReferenceCount(const ConsistentCollection& coll) const {
#ifdef MONGO_CONFIG_DEBUG_BUILD
    return coll._getRefCount();
#else
    return 1;
#endif
}

}  // namespace mongo
