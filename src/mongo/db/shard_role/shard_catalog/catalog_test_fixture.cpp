// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"

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
