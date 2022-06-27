/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/uncommitted_catalog_updates.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

bool collectionExists(OperationContext* opCtx, NamespaceString nss) {
    return CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss) != nullptr;
}

class ConcurrentCreateCollectionTest {
public:
    void run() {
        auto serviceContext = getGlobalServiceContext();

        NamespaceString competingNss("test.competingCollection");

        auto client1 = serviceContext->makeClient("client1");
        auto client2 = serviceContext->makeClient("client2");

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

}  // namespace

class AllCatalogTests : public unittest::OldStyleSuiteSpecification {
public:
    AllCatalogTests() : unittest::OldStyleSuiteSpecification("CatalogTests") {}

    template <typename T>
    void add() {
        addNameCallback(nameForTestClass<T>(), [] { T().run(); });
    }

    void setupTests() {
        add<ConcurrentCreateCollectionTest>();
    }
};

unittest::OldStyleSuiteInitializer<AllCatalogTests> allCatalogTests;

}  // namespace mongo
