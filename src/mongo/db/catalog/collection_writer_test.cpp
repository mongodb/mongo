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
        Lock::GlobalWrite lk(operationContext());

        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(kNss);
        CollectionCatalog::write(getServiceContext(), [&](CollectionCatalog& catalog) {
            auto uuid = collection->uuid();
            catalog.registerCollection(
                operationContext(), uuid, std::move(collection), /*ts=*/boost::none);
        });
    }

    CollectionPtr lookupCollectionFromCatalog() {
        return CollectionPtr(CollectionCatalog::get(operationContext())
                                 ->lookupCollectionByNamespace(operationContext(), kNss));
    }

    const Collection* lookupCollectionFromCatalogForRead() {
        return CollectionCatalog::get(operationContext())
            ->lookupCollectionByNamespace(operationContext(), kNss);
    }

    void verifyCollectionInCatalogUsingDifferentClient(const Collection* expected) {
        stdx::thread t([this, expected]() {
            ThreadClient client(getServiceContext());
            auto opCtx = client->makeOperationContext();
            ASSERT_EQ(expected,
                      CollectionCatalog::get(opCtx.get())
                          ->lookupCollectionByNamespace(opCtx.get(), kNss));
        });
        t.join();
    }

    const NamespaceString kNss =
        NamespaceString::createNamespaceString_forTest("testdb", "testcol");
};

TEST_F(CollectionWriterTest, Commit) {
    AutoGetCollection lock(operationContext(), kNss, MODE_X);
    CollectionWriter writer(operationContext(), kNss);

    const Collection* before = lookupCollectionFromCatalog().get();

    // Before we request a writable collection it should return the same instance installed in the
    // catalog.
    ASSERT_EQ(writer.get(), lookupCollectionFromCatalog());
    ASSERT_EQ(lookupCollectionFromCatalog().get(), lookupCollectionFromCatalogForRead());

    {
        WriteUnitOfWork wuow(operationContext());
        auto writable = writer.getWritableCollection(operationContext());

        // get() and getWritableCollection() should return the same instance
        ASSERT_EQ(writer.get().get(), writable);

        // Regular catalog lookups for this OperationContext should see the uncommitted Collection
        ASSERT_EQ(writable, lookupCollectionFromCatalog().get());

        // Lookup for read should also see uncommitted collection. This in theory supports nested
        // read-only operations, if they ever occur during a top level write operation.
        ASSERT_EQ(writable, lookupCollectionFromCatalogForRead());

        // Regular catalog lookups for different clients should not see any change in the catalog
        verifyCollectionInCatalogUsingDifferentClient(before);
        wuow.commit();
    }

    // We should now have a different Collection pointer written in the catalog
    ASSERT_NE(before, lookupCollectionFromCatalog().get());
    ASSERT_EQ(writer.get(), lookupCollectionFromCatalog());

    // The CollectionWriter can be used again for a different WUOW, perform the logic again
    before = lookupCollectionFromCatalog().get();

    {
        WriteUnitOfWork wuow(operationContext());
        auto writable = writer.getWritableCollection(operationContext());

        ASSERT_EQ(writer.get().get(), writable);
        ASSERT_EQ(writable, lookupCollectionFromCatalog().get());
        ASSERT_EQ(writable, lookupCollectionFromCatalogForRead());

        verifyCollectionInCatalogUsingDifferentClient(before);
        wuow.commit();
    }

    ASSERT_NE(before, lookupCollectionFromCatalog().get());
    ASSERT_EQ(writer.get(), lookupCollectionFromCatalog());
}

TEST_F(CollectionWriterTest, Rollback) {
    AutoGetCollection lock(operationContext(), kNss, MODE_X);
    CollectionWriter writer(operationContext(), kNss);

    const Collection* before = lookupCollectionFromCatalog().get();

    ASSERT_EQ(writer.get(), lookupCollectionFromCatalog());
    ASSERT_EQ(lookupCollectionFromCatalog().get(), lookupCollectionFromCatalogForRead());

    {
        WriteUnitOfWork wuow(operationContext());
        auto writable = writer.getWritableCollection(operationContext());

        ASSERT_EQ(writer.get().get(), writable);
        ASSERT_EQ(writable, lookupCollectionFromCatalog().get());
        ASSERT_EQ(writable, lookupCollectionFromCatalogForRead());
        verifyCollectionInCatalogUsingDifferentClient(before);
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
            writable = writer.getWritableCollection(operationContext());
        }

        wuow.commit();
    }

    // The writable Collection should have been written into the catalog
    ASSERT_EQ(writable, lookupCollectionFromCatalog().get());
}

TEST_F(CollectionWriterTest, CatalogWrite) {
    auto catalog = CollectionCatalog::latest(getServiceContext());
    CollectionCatalog::write(
        getServiceContext(), [this, &catalog](CollectionCatalog& writableCatalog) {
            // We should see a different catalog instance than a reader would
            ASSERT_NE(&writableCatalog, catalog.get());
            // However, it should be a shallow copy. The collection instance should be the same
            ASSERT_EQ(writableCatalog.lookupCollectionByNamespace(operationContext(), kNss),
                      catalog->lookupCollectionByNamespace(operationContext(), kNss));
        });
    auto after = CollectionCatalog::latest(getServiceContext());
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
    static constexpr size_t NumCollections = 50000;

    void setUp() override {
        CatalogTestFixture::setUp();
        Lock::GlobalWrite lk(operationContext());

        CollectionCatalog::write(getServiceContext(), [&](CollectionCatalog& catalog) {
            for (size_t i = 0; i < NumCollections; ++i) {
                catalog.registerCollection(
                    operationContext(),
                    UUID::gen(),
                    std::make_shared<CollectionMock>(NamespaceString::createNamespaceString_forTest(
                        "many", fmt::format("coll{}", i))),
                    /*ts=*/boost::none);
            }
        });
    }
};

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

class BatchedCollectionCatalogWriterTest : public CollectionWriterTest {
public:
    Collection* lookupCollectionFromCatalogForMetadataWrite() {
        return CollectionCatalog::get(operationContext())
            ->lookupCollectionByNamespaceForMetadataWrite(operationContext(), kNss);
    }
};

TEST_F(BatchedCollectionCatalogWriterTest, BatchedTest) {

    const Collection* before = lookupCollectionFromCatalogForRead();
    const Collection* after = nullptr;
    {
        Lock::GlobalWrite lock(operationContext());
        BatchedCollectionCatalogWriter batched(operationContext());

        // We should get a unique clone the first time we request a writable collection
        Collection* firstWritable = lookupCollectionFromCatalogForMetadataWrite();
        ASSERT_NE(firstWritable, before);

        // Subsequent requests should return the same instance.
        Collection* secondWritable = lookupCollectionFromCatalogForMetadataWrite();
        ASSERT_EQ(secondWritable, firstWritable);

        after = firstWritable;
    }

    // When the batched writer commits our collection instance should be replaced.
    ASSERT_NE(lookupCollectionFromCatalogForRead(), before);
    ASSERT_EQ(lookupCollectionFromCatalogForRead(), after);
}

}  // namespace
