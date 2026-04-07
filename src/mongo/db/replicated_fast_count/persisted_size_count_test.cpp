/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/dbhelpers.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class PersistedSizeCountTest : public CatalogTestFixture {
public:
    PersistedSizeCountTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<replicated_fast_count_test_helpers::
                                   ReplicatedFastCountTestPersistenceProvider>())) {}

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
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    constexpr int expectedCount = 5;
    int expectedSize = 0;
    AutoGetCollection coll(operationContext(), nss, LockMode::MODE_IX);
    {
        WriteUnitOfWork wuow(operationContext(),
                             WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
        for (int i = 0; i < expectedCount; ++i) {
            const BSONObj document = BSON("_id" << i << "x" << i);
            ASSERT_OK(Helpers::insert(operationContext(), *coll, document));
            expectedSize += document.objsize();
        }
        wuow.commit();
    }

    manager->flushSync(operationContext());

    const CollectionSizeCount sizeCount = coll->persistedSizeCount(operationContext());
    EXPECT_EQ(sizeCount.count, expectedCount);
    EXPECT_EQ(sizeCount.size, expectedSize);
}
}  // namespace
}  // namespace mongo
