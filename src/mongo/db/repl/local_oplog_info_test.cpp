// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/local_oplog_info.h"

#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_oplog_manager.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

class LocalOplogInfoTest : public ServiceContextMongoDTest {
protected:
    LocalOplogInfoTest() : ServiceContextMongoDTest(Options{}.useReplSettings(true)) {}

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto service = getServiceContext();
        _opCtx = cc().makeOperationContext();

        ReplSettings replSettings;
        replSettings.setReplSetString("rs0/host1");
        ReplicationCoordinator::set(
            service, std::make_unique<ReplicationCoordinatorMock>(service, replSettings));
        ASSERT_OK(
            ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    }

    KVEngine* engine() const {
        return getServiceContext()->getStorageEngine()->getEngine();
    }

    StorageOplogManager* oplogManager() const {
        return engine()->getOplogManager();
    }

    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(LocalOplogInfoTest, CreateOplogStartsOplogManager) {
    ASSERT_FALSE(oplogManager()->isRunning_forTest());
    createOplog(_opCtx.get());
    ASSERT_TRUE(oplogManager()->isRunning_forTest());
}

TEST_F(LocalOplogInfoTest, ResetRecordStoreStopsOplogManager) {
    createOplog(_opCtx.get());
    ASSERT_TRUE(oplogManager()->isRunning_forTest());

    LocalOplogInfo::get(_opCtx.get())->resetRecordStore();
    ASSERT_FALSE(oplogManager()->isRunning_forTest());
}

// Re-establishing a new record store while the manager is already running must stop the manager
// for the old record store and restart it for the new one, rather than leaving it running on the
// old one. This mimics a catalog reopen handing LocalOplogInfo a fresh RecordStore for the oplog.
TEST_F(LocalOplogInfoTest, EstablishingRecordStoreWhileRunningRestartsOplogManager) {
    createOplog(_opCtx.get());
    auto* originalRs = LocalOplogInfo::get(_opCtx.get())->getRecordStore();
    ASSERT(originalRs);
    ASSERT_TRUE(oplogManager()->isRunningForSpecificRS_forTest(originalRs));

    Lock::GlobalWrite globalLk(_opCtx.get());

    // Open a second record store instance backed by the same oplog table.
    RecordStore::Options oplogOptions;
    oplogOptions.isOplog = true;
    oplogOptions.isCapped = true;
    oplogOptions.oplogMaxSize = 1024 * 1024 * 1024;
    auto newRs = engine()->getRecordStore(_opCtx.get(),
                                          NamespaceString::kRsOplogNamespace,
                                          originalRs->getIdent(),
                                          oplogOptions,
                                          originalRs->uuid());
    ASSERT_NE(originalRs, newRs.get());

    establishOplogRecordStoreForLogging(_opCtx.get(), newRs.get());

    // The manager must now be running for the new record store and no longer the original.
    ASSERT_TRUE(oplogManager()->isRunningForSpecificRS_forTest(newRs.get()));
    ASSERT_FALSE(oplogManager()->isRunningForSpecificRS_forTest(originalRs));

    // Stop the manager while 'newRs' is still alive: the visibility thread holds a pointer to it,
    // but the catalog (torn down on teardown) only knows about the original record store.
    LocalOplogInfo::get(_opCtx.get())->resetRecordStore();
}

TEST_F(LocalOplogInfoTest, DeregisteringOplogCollectionStopsOplogManager) {
    createOplog(_opCtx.get());
    ASSERT_TRUE(oplogManager()->isRunning_forTest());

    Lock::GlobalWrite globalLk(_opCtx.get());
    CollectionCatalog::write(_opCtx.get(), [&](CollectionCatalog& catalog) {
        catalog.deregisterAllCollectionsAndViews(getServiceContext());
    });
    ASSERT_FALSE(oplogManager()->isRunning_forTest());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
