// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_role.h"
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
    auto coll = acquireCollection(operationContext(),
                                  CollectionAcquisitionRequest::fromOpCtx(
                                      operationContext(), nss, AcquisitionPrerequisites::kWrite),
                                  LockMode::MODE_IX);

    const SizeCountStore::Entry entry{
        .timestamp = Timestamp(1, 1), .size = 100, .count = 5, .hash = boost::none};
    test_helpers::insertSizeCountEntry(
        operationContext(), *manager->getSizeCountStores_ForTest().first, coll.uuid(), entry);

    const CollectionSizeCount sizeCount =
        coll.getCollectionPtr()->persistedSizeCount(operationContext());
    EXPECT_EQ(sizeCount.count, 5);
    EXPECT_EQ(sizeCount.size, 100);
}
}  // namespace
}  // namespace mongo::replicated_fast_count
