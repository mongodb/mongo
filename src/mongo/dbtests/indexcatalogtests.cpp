/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/dbtests/dbtests.h"

namespace IndexCatalogTests {
namespace {
const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;
}  // namespace

static const NamespaceString _nss("unittests.indexcatalog");

class IndexCatalogTestBase {
protected:
    // Helper to refetch the IndexCatalog from the catalog in order to see any changes made to the
    // collection
    const IndexCatalog* indexCatalog(OperationContext* opCtx) const {
        return CollectionCatalog::get(opCtx)
            ->lookupCollectionByNamespace(opCtx, _nss)
            ->getIndexCatalog();
    }
};

class IndexIteratorTests : IndexCatalogTestBase {
public:
    IndexIteratorTests() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        Lock::DBLock lk(&opCtx, _nss.dbName(), MODE_X);
        OldClientContext ctx(&opCtx, _nss.ns());
        WriteUnitOfWork wuow(&opCtx);

        ctx.db()->createCollection(&opCtx, _nss);
        wuow.commit();
    }

    ~IndexIteratorTests() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        Lock::DBLock lk(&opCtx, _nss.dbName(), MODE_X);
        OldClientContext ctx(&opCtx, _nss.ns());
        WriteUnitOfWork wuow(&opCtx);

        ctx.db()->dropCollection(&opCtx, _nss).transitional_ignore();
        wuow.commit();
    }

    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        dbtests::WriteContextForTests ctx(&opCtx, _nss.ns());

        int numFinishedIndexesStart = indexCatalog(&opCtx)->numIndexesReady(&opCtx);

        dbtests::createIndex(&opCtx, _nss.ns(), BSON("x" << 1)).transitional_ignore();
        dbtests::createIndex(&opCtx, _nss.ns(), BSON("y" << 1)).transitional_ignore();

        ASSERT_TRUE(indexCatalog(&opCtx)->numIndexesReady(&opCtx) == numFinishedIndexesStart + 2);

        auto ii =
            indexCatalog(&opCtx)->getIndexIterator(&opCtx, IndexCatalog::InclusionPolicy::kReady);
        int indexesIterated = 0;
        bool foundIndex = false;
        while (ii->more()) {
            auto indexDesc = ii->next()->descriptor();
            indexesIterated++;
            BSONObjIterator boit(indexDesc->infoObj());
            while (boit.more() && !foundIndex) {
                BSONElement e = boit.next();
                if (e.fieldNameStringData() == "name" && e.valueStringDataSafe() == "y_1") {
                    foundIndex = true;
                    break;
                }
            }
        }

        ASSERT_TRUE(indexesIterated == indexCatalog(&opCtx)->numIndexesReady(&opCtx));
        ASSERT_TRUE(foundIndex);
    }
};

class IndexCatalogEntryDroppedTest : IndexCatalogTestBase {
public:
    IndexCatalogEntryDroppedTest() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        Lock::DBLock lk(&opCtx, _nss.dbName(), MODE_X);
        OldClientContext ctx(&opCtx, _nss.ns());
        WriteUnitOfWork wuow(&opCtx);

        ctx.db()->createCollection(&opCtx, _nss);
        wuow.commit();
    }

    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        dbtests::WriteContextForTests ctx(&opCtx, _nss.ns());

        const IndexDescriptor* idDesc = indexCatalog(&opCtx)->findIdIndex(&opCtx);
        std::shared_ptr<const IndexCatalogEntry> entry =
            indexCatalog(&opCtx)->getEntryShared(idDesc);

        ASSERT_FALSE(entry->isDropped());

        {
            AutoGetCollection autoColl(&opCtx, _nss, MODE_X);
            WriteUnitOfWork wuow(&opCtx);
            ASSERT_OK(autoColl.getDb()->dropCollection(&opCtx, _nss));
            ASSERT_FALSE(entry->isDropped());
        }

        ASSERT_FALSE(entry->isDropped());

        {
            AutoGetCollection autoColl(&opCtx, _nss, MODE_X);
            WriteUnitOfWork wuow(&opCtx);
            ASSERT_OK(autoColl.getDb()->dropCollection(&opCtx, _nss));
            wuow.commit();
            ASSERT_TRUE(entry->isDropped());
        }
    }
};

/**
 * Test for IndexCatalog::refreshEntry().
 */
class RefreshEntry : IndexCatalogTestBase {
public:
    RefreshEntry() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        Lock::DBLock lk(&opCtx, _nss.dbName(), MODE_X);
        OldClientContext ctx(&opCtx, _nss.ns());
        WriteUnitOfWork wuow(&opCtx);

        ctx.db()->createCollection(&opCtx, _nss);
        wuow.commit();
    }

    ~RefreshEntry() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        Lock::DBLock lk(&opCtx, _nss.dbName(), MODE_X);
        OldClientContext ctx(&opCtx, _nss.ns());
        WriteUnitOfWork wuow(&opCtx);

        ctx.db()->dropCollection(&opCtx, _nss).transitional_ignore();
        wuow.commit();
    }

    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        dbtests::WriteContextForTests ctx(&opCtx, _nss.ns());
        const std::string indexName = "x_1";

        ASSERT_OK(dbtests::createIndexFromSpec(&opCtx,
                                               _nss.ns(),
                                               BSON("name" << indexName << "key" << BSON("x" << 1)
                                                           << "v" << static_cast<int>(kIndexVersion)
                                                           << "expireAfterSeconds" << 5)));

        const IndexDescriptor* desc = indexCatalog(&opCtx)->findIndexByName(&opCtx, indexName);
        ASSERT(desc);
        ASSERT_EQUALS(5, desc->infoObj()["expireAfterSeconds"].numberLong());

        // Change value of "expireAfterSeconds" on disk. This will update the metadata for the
        // Collection but not propagate the change to the IndexCatalog
        {
            AutoGetCollection autoColl(&opCtx, _nss, MODE_X);
            CollectionWriter coll(&opCtx, autoColl);

            WriteUnitOfWork wuow(&opCtx);
            coll.getWritableCollection(&opCtx)->updateTTLSetting(&opCtx, "x_1", 10);
            wuow.commit();
        }

        // Confirm that the index catalog does not yet know of the change.
        desc = indexCatalog(&opCtx)->findIndexByName(&opCtx, indexName);
        ASSERT_EQUALS(5, desc->infoObj()["expireAfterSeconds"].numberLong());

        {
            AutoGetCollection autoColl(&opCtx, _nss, MODE_X);
            CollectionWriter coll(&opCtx, autoColl);

            // Notify the catalog of the change.
            WriteUnitOfWork wuow(&opCtx);
            desc = coll.getWritableCollection(&opCtx)->getIndexCatalog()->refreshEntry(
                &opCtx, coll.getWritableCollection(&opCtx), desc, CreateIndexEntryFlags::kIsReady);
            wuow.commit();
        }

        // Test that the catalog reflects the change.
        ASSERT_EQUALS(10, desc->infoObj()["expireAfterSeconds"].numberLong());
    }
};

class IndexCatalogTests : public OldStyleSuiteSpecification {
public:
    IndexCatalogTests() : OldStyleSuiteSpecification("indexcatalogtests") {}
    void setupTests() {
        add<IndexIteratorTests>();
        add<IndexCatalogEntryDroppedTest>();
        add<RefreshEntry>();
    }
};

OldStyleSuiteInitializer<IndexCatalogTests> indexCatalogTests;
}  // namespace IndexCatalogTests
