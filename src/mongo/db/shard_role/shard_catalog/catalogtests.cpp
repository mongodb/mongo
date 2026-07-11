// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/shard_catalog/uncommitted_catalog_updates.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace {

bool collectionExists(OperationContext* opCtx, NamespaceString nss) {
    return static_cast<bool>(
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss));
}

class ConcurrentCreateCollectionTest {
public:
    void run() {
        auto serviceContext = getGlobalServiceContext();

        NamespaceString competingNss =
            NamespaceString::createNamespaceString_forTest("test.competingCollection");

        auto client1 = serviceContext->getService()->makeClient("client1");
        auto client2 = serviceContext->getService()->makeClient("client2");

        auto op1 = client1->makeOperationContext();
        auto op2 = client2->makeOperationContext();

        Lock::DBLock dbLk1(op1.get(), competingNss.dbName(), LockMode::MODE_IX);
        Lock::CollectionLock collLk1(op1.get(), competingNss, LockMode::MODE_IX);
        Lock::DBLock dbLk2(op2.get(), competingNss.dbName(), LockMode::MODE_IX);
        Lock::CollectionLock collLk2(op2.get(), competingNss, LockMode::MODE_IX);

        Database* db =
            DatabaseHolder::get(op1.get())->openDb(op1.get(), competingNss.dbName(), nullptr);

        {
            WriteUnitOfWork wuow1(op1.get());
            ASSERT_TRUE(db->createCollection(op1.get(), competingNss) != nullptr);
            ASSERT_TRUE(collectionExists(op1.get(), competingNss));
            ASSERT_FALSE(collectionExists(op2.get(), competingNss));
            {
                WriteUnitOfWork wuow2(op2.get());
                ASSERT_FALSE(collectionExists(op2.get(), competingNss));
                ASSERT_TRUE(db->createCollection(op2.get(), competingNss) != nullptr);

                ASSERT_TRUE(collectionExists(op1.get(), competingNss));
                ASSERT_TRUE(collectionExists(op2.get(), competingNss));

                auto [found1, collection1, newColl1] =
                    UncommittedCatalogUpdates::lookupCollection(op1.get(), competingNss);
                auto [found2, collection2, newColl2] =
                    UncommittedCatalogUpdates::lookupCollection(op2.get(), competingNss);
                ASSERT_EQUALS(found1, found2);
                ASSERT_EQUALS(newColl1, newColl2);
                ASSERT_NOT_EQUALS(collection1, collection2);
                wuow2.commit();
            }
            ASSERT_THROWS(wuow1.commit(), WriteConflictException);
        }

        ASSERT_TRUE(collectionExists(op1.get(), competingNss));
        ASSERT_TRUE(collectionExists(op2.get(), competingNss));
    }
};

class AllCatalogTests : public unittest::OldStyleSuiteSpecification {
public:
    AllCatalogTests() : unittest::OldStyleSuiteSpecification("CatalogTests") {}

    void setupTests() override {
        add<ConcurrentCreateCollectionTest>();
    }
};

unittest::OldStyleSuiteInitializer<AllCatalogTests> allCatalogTests;

}  // namespace
}  // namespace mongo
