/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <memory>

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

/**
 * Sets up the catalog (via CatalogTestFixture), installs a collection in the catalog and provides
 * helper function to access the collection from the catalog.
 */
class CollectionWriterTest : public CatalogTestFixture {
public:
    CollectionWriterTest() = default;

protected:
    void setUp() override {
        CatalogTestFixture::setUp();

        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(kNss);
        CollectionCatalog::write(getServiceContext(), [&](CollectionCatalog& catalog) {
            catalog.registerCollection(
                operationContext(), CollectionUUID::gen(), std::move(collection));
        });
    }

    CollectionPtr lookupCollectionFromCatalog() {
        return CollectionCatalog::get(operationContext())
            ->lookupCollectionByNamespace(operationContext(), kNss);
    }

    const Collection* lookupCollectionFromCatalogForRead() {
        return CollectionCatalog::get(operationContext())
            ->lookupCollectionByNamespaceForRead(operationContext(), kNss)
            .get();
    }

    void verifyCollectionInCatalogUsingDifferentClient(const Collection* expected) {
        stdx::thread t([this, expected]() {
            ThreadClient client(getServiceContext());
            auto opCtx = client->makeOperationContext();
            ASSERT_EQ(expected,
                      CollectionCatalog::get(opCtx.get())
                          ->lookupCollectionByNamespace(opCtx.get(), kNss)
                          .get());
        });
        t.join();
    }

    const NamespaceString kNss{"testdb", "testcol"};
};

TEST_F(CollectionWriterTest, Inplace) {
    CollectionWriter writer(operationContext(), kNss, CollectionCatalog::LifetimeMode::kInplace);

    // CollectionWriter in Inplace mode should operate directly on the Collection instance stored in
    // the catalog. So no Collection copy should be made.
    ASSERT_EQ(writer.get(), lookupCollectionFromCatalog());
    ASSERT_EQ(lookupCollectionFromCatalog().get(), lookupCollectionFromCatalogForRead());


    auto writable = writer.getWritableCollection();
    ASSERT_EQ(writable, lookupCollectionFromCatalog().get());
    ASSERT_EQ(lookupCollectionFromCatalog().get(), lookupCollectionFromCatalogForRead());
}

TEST_F(CollectionWriterTest, Commit) {
    CollectionWriter writer(operationContext(), kNss);

    const Collection* before = lookupCollectionFromCatalog().get();

    // Before we request a writable collection it should return the same instance installed in the
    // catalog.
    ASSERT_EQ(writer.get(), lookupCollectionFromCatalog());
    ASSERT_EQ(lookupCollectionFromCatalog().get(), lookupCollectionFromCatalogForRead());

    {
        AutoGetCollection lock(operationContext(), kNss, MODE_X);
        WriteUnitOfWork wuow(operationContext());
        auto writable = writer.getWritableCollection();

        // get() and getWritableCollection() should return the same instance
        ASSERT_EQ(writer.get().get(), writable);

        // Regular catalog lookups for this OperationContext should see the uncommitted Collection
        ASSERT_EQ(writable, lookupCollectionFromCatalog().get());

        // Regular catalog lookups for different clients should not see any change in the catalog
        verifyCollectionInCatalogUsingDifferentClient(before);

        // Lookup for read should not see the uncommitted Collection. This is fine as in reality
        // this API should only be used for readers and there should be no uncommitted writes as
        // there are in this unittest.
        ASSERT_NE(writable, lookupCollectionFromCatalogForRead());

        wuow.commit();
    }

    // We should now have a different Collection pointer written in the catalog
    ASSERT_NE(before, lookupCollectionFromCatalog().get());
    ASSERT_EQ(writer.get(), lookupCollectionFromCatalog());

    // The CollectionWriter can be used again for a different WUOW, perform the logic again
    before = lookupCollectionFromCatalog().get();

    {
        AutoGetCollection lock(operationContext(), kNss, MODE_X);
        WriteUnitOfWork wuow(operationContext());
        auto writable = writer.getWritableCollection();

        ASSERT_EQ(writer.get().get(), writable);
        ASSERT_EQ(writable, lookupCollectionFromCatalog().get());
        verifyCollectionInCatalogUsingDifferentClient(before);
        ASSERT_NE(writable, lookupCollectionFromCatalogForRead());
        wuow.commit();
    }

    ASSERT_NE(before, lookupCollectionFromCatalog().get());
    ASSERT_EQ(writer.get(), lookupCollectionFromCatalog());
}

TEST_F(CollectionWriterTest, Rollback) {
    CollectionWriter writer(operationContext(), kNss);

    const Collection* before = lookupCollectionFromCatalog().get();

    ASSERT_EQ(writer.get(), lookupCollectionFromCatalog());
    ASSERT_EQ(lookupCollectionFromCatalog().get(), lookupCollectionFromCatalogForRead());

    {
        AutoGetCollection lock(operationContext(), kNss, MODE_X);
        WriteUnitOfWork wuow(operationContext());
        auto writable = writer.getWritableCollection();

        ASSERT_EQ(writer.get().get(), writable);
        ASSERT_EQ(writable, lookupCollectionFromCatalog().get());
        verifyCollectionInCatalogUsingDifferentClient(before);
        ASSERT_NE(writable, lookupCollectionFromCatalogForRead());
    }

    // No update in the catalog should have happened
    ASSERT_EQ(before, lookupCollectionFromCatalog().get());

    // CollectionWriter should be in sync with the catalog
    ASSERT_EQ(writer.get(), lookupCollectionFromCatalog());
}

TEST_F(CollectionWriterTest, CommitAfterDestroy) {
    const Collection* writable = nullptr;

    {
        AutoGetCollection lock(operationContext(), kNss, MODE_X);
        WriteUnitOfWork wuow(operationContext());

        {
            CollectionWriter writer(operationContext(), kNss);

            // Request a writable Collection and destroy CollectionWriter before WUOW commits
            writable = writer.getWritableCollection();
        }

        wuow.commit();
    }

    // The writable Collection should have been written into the catalog
    ASSERT_EQ(writable, lookupCollectionFromCatalog().get());
}

}  // namespace
