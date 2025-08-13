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

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_mock.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

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

        createCollection(kNss);
    }

    void createCollection(const NamespaceString& nss) {
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
        CollectionCatalog::write(getServiceContext(), [&](CollectionCatalog& catalog) {
            catalog.registerCollection(
                operationContext(), std::move(collection), /*ts=*/boost::none);
        });
    }

    CollectionPtr lookupCollectionFromCatalog() {
        // The lifetime of the collection returned by the lookup is guaranteed to be valid as
        // it's controlled by the test. The initialization is therefore safe.
        return CollectionPtr::CollectionPtr_UNSAFE(
            CollectionCatalog::get(operationContext())
                ->lookupCollectionByNamespace(operationContext(), kNss));
    }

    const Collection* lookupCollectionFromCatalogForRead() {
        return lookupCollectionFromCatalogForRead(kNss);
    }

    const Collection* lookupCollectionFromCatalogForRead(const NamespaceString& nss) {
        return CollectionCatalog::get(operationContext())
            ->lookupCollectionByNamespace(operationContext(), nss);
    }

    void verifyCollectionInCatalogUsingDifferentClient(const Collection* expected,
                                                       const NamespaceString& nss) {
        stdx::thread t([this, expected, nss]() {
            ThreadClient client(getServiceContext()->getService());
            auto opCtx = client->makeOperationContext();
            ASSERT_EQ(
                expected,
                CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss));
        });
        t.join();
    }

    void verifyCollectionInCatalogUsingDifferentClient(const Collection* expected) {
        verifyCollectionInCatalogUsingDifferentClient(expected, kNss);
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
    stdx::mutex m;
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

// Validate that the oplog supports copy on write like any other collection.
TEST_F(CollectionWriterTest, OplogCOW) {
    const auto nss = NamespaceString::kRsOplogNamespace;

    const auto& oplogInitial = lookupCollectionFromCatalogForRead(nss);
    ASSERT(oplogInitial);

    AutoGetCollection lock(operationContext(), nss, MODE_X);
    CollectionWriter writer(operationContext(), nss);

    const auto& oplogDuringDDL = lookupCollectionFromCatalogForRead(nss);
    ASSERT(oplogDuringDDL);
    ASSERT(oplogInitial == oplogDuringDDL);

    {
        WriteUnitOfWork wuow(operationContext());
        auto writable = writer.getWritableCollection(operationContext());

        // get() and getWritableCollection() should return the same instance
        ASSERT_EQ(writer.get().get(), writable);

        // Catalog lookups for this OperationContext should see the uncommitted collection.
        ASSERT_EQ(writable, lookupCollectionFromCatalogForRead(nss));

        wuow.commit();
    }

    // A new oplog lookup should return a new Collection pointer.
    const auto& oplogAfterDDL = lookupCollectionFromCatalogForRead(nss);
    ASSERT(oplogAfterDDL != oplogInitial);

    // Likewise from another thread.
    verifyCollectionInCatalogUsingDifferentClient(oplogAfterDDL, nss);
}

void runAutoGetOplogFastPathObjectStabilityConcurrentDDL(ServiceContext* svcCtx,
                                                         OperationContext* opCtx,
                                                         OplogAccessMode mode) {
    boost::optional<Lock::GlobalLock> gl;
    if (mode == OplogAccessMode::kLogOp) {
        gl.emplace(opCtx, MODE_IX);
    }

    AutoGetOplogFastPath oplogRead(opCtx, mode);
    const auto& oplogR = oplogRead.getCollection();
    ASSERT(oplogR);

    // Modify the oplog catalog object in another thread. This is possible because the fast-path
    // oplog acquisition does not hold the oplog collection lock.
    {
        stdx::thread t([&]() {
            ThreadClient client(svcCtx->getService());
            auto opCtx = client->makeOperationContext();

            writeConflictRetry(
                opCtx.get(), "dummy oplog DDL", NamespaceString::kRsOplogNamespace, [&] {
                    AutoGetCollection autoColl(
                        opCtx.get(), NamespaceString::kRsOplogNamespace, MODE_X);
                    ASSERT(autoColl.getCollection());
                    WriteUnitOfWork wunit(opCtx.get());
                    CollectionWriter writer{opCtx.get(), autoColl};

                    auto oplogWrite = writer.getWritableCollection(opCtx.get());
                    ASSERT(oplogWrite);
                    wunit.commit();
                });
        });
        t.join();
    }

    // Access the oplog object. It doesn't matter why we access it, as long as we do. This ensures
    // that the collection object is still valid, albeit stale due to the DDL that committed above.
    // If the object is unexpectedly invalid, this would be detected by our sanitizers (heap use
    // after free).
    ASSERT(oplogR->ns() == NamespaceString::kRsOplogNamespace);
}

// Validate that the oplog object remains valid in the case of a concurrent DDL operation.
TEST_F(CollectionWriterTest, AutoGetOplogFastPathkReadObjectStabilityConcurrentDDL) {
    runAutoGetOplogFastPathObjectStabilityConcurrentDDL(
        getServiceContext(), operationContext(), OplogAccessMode::kRead);
}
TEST_F(CollectionWriterTest, AutoGetOplogFastPathkWriteObjectStabilityConcurrentDDL) {
    runAutoGetOplogFastPathObjectStabilityConcurrentDDL(
        getServiceContext(), operationContext(), OplogAccessMode::kWrite);
}
TEST_F(CollectionWriterTest, AutoGetOplogFastPathkLogOpObjectStabilityConcurrentDDL) {
    runAutoGetOplogFastPathObjectStabilityConcurrentDDL(
        getServiceContext(), operationContext(), OplogAccessMode::kLogOp);
}

}  // namespace
}  // namespace mongo
