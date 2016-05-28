// indexcatalogtests.cpp : index_catalog.{h,cpp} unit tests.

/**
 *    Copyright (C) 2013 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/dbtests/dbtests.h"

namespace IndexCatalogTests {

static const char* const _ns = "unittests.indexcatalog";

class IndexIteratorTests {
public:
    IndexIteratorTests() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        ScopedTransaction transaction(&txn, MODE_IX);
        Lock::DBLock lk(txn.lockState(), nsToDatabaseSubstring(_ns), MODE_X);
        OldClientContext ctx(&txn, _ns);
        WriteUnitOfWork wuow(&txn);

        _db = ctx.db();
        _coll = _db->createCollection(&txn, _ns);
        _catalog = _coll->getIndexCatalog();
        wuow.commit();
    }

    ~IndexIteratorTests() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        ScopedTransaction transaction(&txn, MODE_IX);
        Lock::DBLock lk(txn.lockState(), nsToDatabaseSubstring(_ns), MODE_X);
        OldClientContext ctx(&txn, _ns);
        WriteUnitOfWork wuow(&txn);

        _db->dropCollection(&txn, _ns);
        wuow.commit();
    }

    void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        OldClientWriteContext ctx(&txn, _ns);

        int numFinishedIndexesStart = _catalog->numIndexesReady(&txn);

        dbtests::createIndex(&txn, _ns, BSON("x" << 1));
        dbtests::createIndex(&txn, _ns, BSON("y" << 1));

        ASSERT_TRUE(_catalog->numIndexesReady(&txn) == numFinishedIndexesStart + 2);

        IndexCatalog::IndexIterator ii = _catalog->getIndexIterator(&txn, false);
        int indexesIterated = 0;
        bool foundIndex = false;
        while (ii.more()) {
            IndexDescriptor* indexDesc = ii.next();
            indexesIterated++;
            BSONObjIterator boit(indexDesc->infoObj());
            while (boit.more() && !foundIndex) {
                BSONElement e = boit.next();
                if (str::equals(e.fieldName(), "name") && str::equals(e.valuestrsafe(), "y_1")) {
                    foundIndex = true;
                    break;
                }
            }
        }

        ASSERT_TRUE(indexesIterated == _catalog->numIndexesReady(&txn));
        ASSERT_TRUE(foundIndex);
    }

private:
    IndexCatalog* _catalog;
    Collection* _coll;
    Database* _db;
};

/**
 * Test for IndexCatalog::refreshEntry().
 */
class RefreshEntry {
public:
    RefreshEntry() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        ScopedTransaction transaction(&txn, MODE_IX);
        Lock::DBLock lk(txn.lockState(), nsToDatabaseSubstring(_ns), MODE_X);
        OldClientContext ctx(&txn, _ns);
        WriteUnitOfWork wuow(&txn);

        _db = ctx.db();
        _coll = _db->createCollection(&txn, _ns);
        _catalog = _coll->getIndexCatalog();
        wuow.commit();
    }

    ~RefreshEntry() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        ScopedTransaction transaction(&txn, MODE_IX);
        Lock::DBLock lk(txn.lockState(), nsToDatabaseSubstring(_ns), MODE_X);
        OldClientContext ctx(&txn, _ns);
        WriteUnitOfWork wuow(&txn);

        _db->dropCollection(&txn, _ns);
        wuow.commit();
    }

    void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        OldClientWriteContext ctx(&txn, _ns);
        const std::string indexName = "x_1";

        ASSERT_OK(dbtests::createIndexFromSpec(&txn,
                                               _ns,
                                               BSON("name" << indexName << "ns" << _ns << "key"
                                                           << BSON("x" << 1)
                                                           << "expireAfterSeconds"
                                                           << 5)));

        const IndexDescriptor* desc = _catalog->findIndexByName(&txn, indexName);
        ASSERT(desc);
        ASSERT_EQUALS(5, desc->infoObj()["expireAfterSeconds"].numberLong());

        // Change value of "expireAfterSeconds" on disk.
        {
            WriteUnitOfWork wuow(&txn);
            _coll->getCatalogEntry()->updateTTLSetting(&txn, "x_1", 10);
            wuow.commit();
        }

        // Verify that the catalog does not yet know of the change.
        desc = _catalog->findIndexByName(&txn, indexName);
        ASSERT_EQUALS(5, desc->infoObj()["expireAfterSeconds"].numberLong());

        {
            // Notify the catalog of the change.
            WriteUnitOfWork wuow(&txn);
            desc = _catalog->refreshEntry(&txn, desc);
            wuow.commit();
        }

        // Test that the catalog reflects the change.
        ASSERT_EQUALS(10, desc->infoObj()["expireAfterSeconds"].numberLong());
    }

private:
    IndexCatalog* _catalog;
    Collection* _coll;
    Database* _db;
};

class IndexCatalogTests : public Suite {
public:
    IndexCatalogTests() : Suite("indexcatalogtests") {}
    void setupTests() {
        add<IndexIteratorTests>();
        add<RefreshEntry>();
    }
};

SuiteInstance<IndexCatalogTests> indexCatalogTests;
}
