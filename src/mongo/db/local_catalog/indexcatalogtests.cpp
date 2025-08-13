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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace IndexCatalogTests {
namespace {
const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;
}  // namespace

static const NamespaceString _nss =
    NamespaceString::createNamespaceString_forTest("unittests.indexcatalog");

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
        AutoGetDb autodb(&opCtx, _nss.dbName(), MODE_X);
        WriteUnitOfWork wuow(&opCtx);

        autodb.ensureDbExists(&opCtx)->createCollection(&opCtx, _nss);
        wuow.commit();
    }

    ~IndexIteratorTests() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        AutoGetDb autodb(&opCtx, _nss.dbName(), MODE_X);
        WriteUnitOfWork wuow(&opCtx);

        autodb.ensureDbExists(&opCtx)->dropCollection(&opCtx, _nss).transitional_ignore();
        wuow.commit();
    }

    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        dbtests::WriteContextForTests ctx(&opCtx, _nss.ns_forTest());

        int numFinishedIndexesStart = indexCatalog(&opCtx)->numIndexesReady();

        dbtests::createIndex(&opCtx, _nss.ns_forTest(), BSON("x" << 1)).transitional_ignore();
        dbtests::createIndex(&opCtx, _nss.ns_forTest(), BSON("y" << 1)).transitional_ignore();

        ASSERT_TRUE(indexCatalog(&opCtx)->numIndexesReady() == numFinishedIndexesStart + 2);

        auto ii = indexCatalog(&opCtx)->getIndexIterator(IndexCatalog::InclusionPolicy::kReady);
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

        ASSERT_TRUE(indexesIterated == indexCatalog(&opCtx)->numIndexesReady());
        ASSERT_TRUE(foundIndex);
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
        AutoGetDb autodb(&opCtx, _nss.dbName(), MODE_X);
        WriteUnitOfWork wuow(&opCtx);

        autodb.ensureDbExists(&opCtx)->createCollection(&opCtx, _nss);
        wuow.commit();
    }

    ~RefreshEntry() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        AutoGetDb autodb(&opCtx, _nss.dbName(), MODE_X);
        WriteUnitOfWork wuow(&opCtx);

        autodb.ensureDbExists(&opCtx)->dropCollection(&opCtx, _nss).transitional_ignore();
        wuow.commit();
    }

    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        dbtests::WriteContextForTests ctx(&opCtx, _nss.ns_forTest());
        const std::string indexName = "x_1";

        ASSERT_OK(dbtests::createIndexFromSpec(&opCtx,
                                               _nss.ns_forTest(),
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

class PrepareUniqueIndexRecords : IndexCatalogTestBase {
public:
    ~PrepareUniqueIndexRecords() {
        auto opCtx = cc().makeOperationContext();
        AutoGetDb db{opCtx.get(), _nss.dbName(), LockMode::MODE_X};
        WriteUnitOfWork wuow{opCtx.get()};
        ASSERT_OK(db.getDb()->dropCollection(opCtx.get(), _nss));
        wuow.commit();
    }

    void run() {
        auto opCtx = cc().makeOperationContext();
        dbtests::WriteContextForTests ctx{opCtx.get(), _nss.ns_forTest()};

        ASSERT_OK(dbtests::createIndexFromSpec(
            opCtx.get(),
            _nss.ns_forTest(),
            BSON(IndexDescriptor::kIndexVersionFieldName
                 << static_cast<int>(kIndexVersion) << IndexDescriptor::kIndexNameFieldName << "a_1"
                 << IndexDescriptor::kKeyPatternFieldName << BSON("a" << 1)
                 << IndexDescriptor::kPrepareUniqueFieldName << true)));

        AutoGetCollection coll{opCtx.get(), _nss, LockMode::MODE_X};
        auto doc1 = BSON("_id" << 1 << "a" << 1);
        auto doc2 = BSON("_id" << 2 << "a" << 1);

        {
            WriteUnitOfWork wuow{opCtx.get()};
            ASSERT_OK(indexCatalog(opCtx.get())
                          ->indexRecords(opCtx.get(), *coll, {{RecordId{1}, {}, &doc1}}, nullptr));
            wuow.commit();
        }

        {
            WriteUnitOfWork wuow{opCtx.get()};
            ASSERT_NOT_OK(
                indexCatalog(opCtx.get())
                    ->indexRecords(opCtx.get(), *coll, {{RecordId{2}, {}, &doc2}}, nullptr));
        }

        opCtx->setEnforceConstraints(false);

        {
            WriteUnitOfWork wuow{opCtx.get()};
            ASSERT_OK(indexCatalog(opCtx.get())
                          ->indexRecords(opCtx.get(), *coll, {{RecordId{2}, {}, &doc2}}, nullptr));
            wuow.commit();
        }
    }
};

class PrepareUniqueUpdateRecord : IndexCatalogTestBase {
public:
    ~PrepareUniqueUpdateRecord() {
        auto opCtx = cc().makeOperationContext();
        AutoGetDb db{opCtx.get(), _nss.dbName(), LockMode::MODE_X};
        WriteUnitOfWork wuow{opCtx.get()};
        ASSERT_OK(db.getDb()->dropCollection(opCtx.get(), _nss));
        wuow.commit();
    }

    void run() {
        auto opCtx = cc().makeOperationContext();
        dbtests::WriteContextForTests ctx{opCtx.get(), _nss.ns_forTest()};

        ASSERT_OK(dbtests::createIndexFromSpec(
            opCtx.get(),
            _nss.ns_forTest(),
            BSON(IndexDescriptor::kIndexVersionFieldName
                 << static_cast<int>(kIndexVersion) << IndexDescriptor::kIndexNameFieldName << "a_1"
                 << IndexDescriptor::kKeyPatternFieldName << BSON("a" << 1)
                 << IndexDescriptor::kPrepareUniqueFieldName << true)));

        AutoGetCollection coll{opCtx.get(), _nss, LockMode::MODE_X};
        auto doc1 = BSON("_id" << 1 << "a" << 1);
        auto doc2 = BSON("_id" << 2 << "a" << 2);
        auto updatedDoc2 = BSON("_id" << 2 << "a" << 1);

        {
            WriteUnitOfWork wuow{opCtx.get()};
            ASSERT_OK(indexCatalog(opCtx.get())
                          ->indexRecords(opCtx.get(),
                                         *coll,
                                         {{RecordId{1}, {}, &doc1}, {RecordId{2}, {}, &doc2}},
                                         nullptr));
            wuow.commit();
        }

        {
            WriteUnitOfWork wuow{opCtx.get()};
            int64_t keysInsertedOut, keysDeletedOut;
            ASSERT_NOT_OK(indexCatalog(opCtx.get())
                              ->updateRecord(opCtx.get(),
                                             *coll,
                                             doc2,
                                             updatedDoc2,
                                             nullptr,
                                             RecordId{2},
                                             &keysInsertedOut,
                                             &keysDeletedOut));
            ASSERT_EQ(keysInsertedOut, 0);
            ASSERT_EQ(keysDeletedOut, 0);
        }

        opCtx->setEnforceConstraints(false);

        {
            WriteUnitOfWork wuow{opCtx.get()};
            int64_t keysInsertedOut, keysDeletedOut;
            ASSERT_OK(indexCatalog(opCtx.get())
                          ->updateRecord(opCtx.get(),
                                         *coll,
                                         doc2,
                                         updatedDoc2,
                                         nullptr,
                                         RecordId{2},
                                         &keysInsertedOut,
                                         &keysDeletedOut));
            ASSERT_EQ(keysInsertedOut, 1);
            ASSERT_EQ(keysDeletedOut, 1);
            wuow.commit();
        }
    }
};

class IndexCatalogTests : public unittest::OldStyleSuiteSpecification {
public:
    IndexCatalogTests() : OldStyleSuiteSpecification("indexcatalogtests") {}
    void setupTests() override {
        add<IndexIteratorTests>();
        add<RefreshEntry>();
        add<PrepareUniqueIndexRecords>();
        add<PrepareUniqueUpdateRecord>();
    }
};

unittest::OldStyleSuiteInitializer<IndexCatalogTests> indexCatalogTests;

}  // namespace IndexCatalogTests
}  // namespace mongo
