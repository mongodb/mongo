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
#include "mongo/unittest/barrier.h"
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

TEST_F(CollectionWriterTest, CatalogWrite) {
    auto catalog = CollectionCatalog::get(getServiceContext());
    CollectionCatalog::write(
        getServiceContext(), [this, &catalog](CollectionCatalog& writableCatalog) {
            // We should see a different catalog instance than a reader would
            ASSERT_NE(&writableCatalog, catalog.get());
            // However, it should be a shallow copy. The collection instance should be the same
            ASSERT_EQ(
                writableCatalog.lookupCollectionByNamespaceForRead(operationContext(), kNss).get(),
                catalog->lookupCollectionByNamespaceForRead(operationContext(), kNss).get());
        });
    auto after = CollectionCatalog::get(getServiceContext());
    ASSERT_NE(&catalog, &after);
}

TEST_F(CatalogTestFixture, ConcurrentCatalogWritesSerialized) {
    // Start threads and perform write that will try to lock mutex which should always succeed as
    // all writes are serialized
    constexpr int32_t NumThreads = 4;
    constexpr int32_t WritesPerThread = 1000;

    unittest::Barrier barrier(NumThreads);
    Mutex m;
    auto job = [&]() {
        barrier.countDownAndWait();

        for (int i = 0; i < WritesPerThread; ++i) {
            CollectionCatalog::write(getServiceContext(), [&](CollectionCatalog& writableCatalog) {
                stdx::unique_lock lock(m, stdx::try_to_lock);
                ASSERT(lock.owns_lock());
            });
        }
    };

    std::array<stdx::thread, NumThreads> threads;
    for (auto&& thread : threads) {
        thread = stdx::thread(job);
    }
    for (auto&& thread : threads) {
        thread.join();
    }
}

/**
 * This test uses a catalog with a large number of collections to make it slow to copy. The idea
 * is to trigger the batching behavior when multiple threads want to perform catalog writes
 * concurrently. The batching works correctly if the threads all observe the same catalog
 * instance when they write. If this test is flaky, try to increase the number of collections in
 * the catalog setup.
 */
class CatalogReadCopyUpdateTest : public CatalogTestFixture {
public:
    // Number of collection instances in the catalog. We want to have a large number to make the
    // CollectionCatalog copy constructor slow enough to trigger the batching behavior. All threads
    // need to enter CollectionCatalog::write() to be batched before the first thread finishes its
    // write.
    static constexpr size_t NumCollections = 100000;

    void setUp() override {
        CatalogTestFixture::setUp();

        CollectionCatalog::write(getServiceContext(), [&](CollectionCatalog& catalog) {
            for (size_t i = 0; i < NumCollections; ++i) {
                catalog.registerCollection(operationContext(),
                                           CollectionUUID::gen(),
                                           std::make_shared<CollectionMock>(
                                               NamespaceString("many", fmt::format("coll{}", i))));
            }
        });
    }
};

TEST_F(CatalogReadCopyUpdateTest, ConcurrentCatalogWriteBatches) {
    // Start threads and perform write at the same time, record catalog instance observed
    constexpr int32_t NumThreads = 4;

    unittest::Barrier barrier(NumThreads);
    std::array<const CollectionCatalog*, NumThreads> catalogInstancesObserved;
    AtomicWord<int32_t> threadIndex{0};
    auto job = [&]() {
        auto index = threadIndex.fetchAndAdd(1);
        barrier.countDownAndWait();

        // The first thread that enters write() will begin copying the catalog instance, other
        // threads that enter while this copy is being made will be enqueued. When the thread
        // copying the catalog instance finishes the copy it will execute all writes using the same
        // writable catalog instance.
        //
        // To minimize the risk of this test being flaky we need to make the catalog copy slow
        // enough so the other threads properly enter the queue state. We achieve this by having a
        // large numbers of collections in the catalog.
        CollectionCatalog::write(getServiceContext(), [&](CollectionCatalog& writableCatalog) {
            catalogInstancesObserved[index] = &writableCatalog;
        });
    };

    std::array<stdx::thread, NumThreads> threads;
    for (auto&& thread : threads) {
        thread = stdx::thread(job);
    }
    for (auto&& thread : threads) {
        thread.join();
    }

    // Verify that all threads observed same instance. We do this by sorting the array, removing all
    // duplicates and last verify that there is only one element remaining.
    std::sort(catalogInstancesObserved.begin(), catalogInstancesObserved.end());
    auto it = std::unique(catalogInstancesObserved.begin(), catalogInstancesObserved.end());
    ASSERT_EQ(std::distance(catalogInstancesObserved.begin(), it), 1);
}

TEST_F(CatalogReadCopyUpdateTest, ConcurrentCatalogWriteBatchingMayThrow) {
    // Start threads and perform write that will throw at the same time
    constexpr int32_t NumThreads = 4;

    unittest::Barrier barrier(NumThreads);
    AtomicWord<int32_t> threadIndex{0};
    auto job = [&]() {
        auto index = threadIndex.fetchAndAdd(1);
        barrier.countDownAndWait();

        try {
            CollectionCatalog::write(getServiceContext(),
                                     [&](CollectionCatalog& writableCatalog) { throw index; });
            // Should not reach this assert
            ASSERT(false);
        } catch (int32_t ex) {
            // Verify that we received the correct exception even if our write job executed on a
            // different thread
            ASSERT_EQ(ex, index);
        }
    };

    std::array<stdx::thread, NumThreads> threads;
    for (auto&& thread : threads) {
        thread = stdx::thread(job);
    }
    for (auto&& thread : threads) {
        thread.join();
    }
}

}  // namespace
