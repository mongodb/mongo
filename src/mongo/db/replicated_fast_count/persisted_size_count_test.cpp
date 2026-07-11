// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/dbhelpers.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"

namespace mongo::replicated_fast_count {
namespace {

class PersistedSizeCountTest : public CatalogTestFixture {
public:
    PersistedSizeCountTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>())) {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();

        auto* registry = dynamic_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        ASSERT(registry);
        registry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));

        manager = &ReplicatedFastCountManager::get(operationContext()->getServiceContext());
        manager->disablePeriodicWrites_ForTest();

        setUpReplicatedFastCount(operationContext());

        ASSERT_OK(createCollection(operationContext(), nss.dbName(), BSON("create" << nss.coll())));
    }

    void tearDown() override {
        manager = nullptr;
        CatalogTestFixture::tearDown();
    }

    ReplicatedFastCountManager* manager;
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("collection_impl_test", "coll");
};

TEST_F(PersistedSizeCountTest, UuidExistsInSizeCountStore) {
    unittest::ServerParameterGuard featureFlag("featureFlagReplicatedFastCount", true);

    constexpr int expectedCount = 5;
    int expectedSize = 0;
    auto coll = acquireCollection(operationContext(),
                                  CollectionAcquisitionRequest::fromOpCtx(
                                      operationContext(), nss, AcquisitionPrerequisites::kWrite),
                                  LockMode::MODE_IX);
    {
        WriteUnitOfWork wuow(operationContext(),
                             WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
        for (int i = 0; i < expectedCount; ++i) {
            const BSONObj document = BSON("_id" << i << "x" << i);
            ASSERT_OK(Helpers::insert(operationContext(), coll.getCollectionPtr(), document));
            expectedSize += document.objsize();
        }
        wuow.commit();
    }

    manager->flushSync_ForTest(operationContext());

    const CollectionSizeCount sizeCount =
        coll.getCollectionPtr()->persistedSizeCount(operationContext());
    EXPECT_EQ(sizeCount.count, expectedCount);
    EXPECT_EQ(sizeCount.size, expectedSize);
}
}  // namespace
}  // namespace mongo::replicated_fast_count
