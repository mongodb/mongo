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
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/dbtests/dbtests.h"

namespace IndexCatalogTests {
namespace {
const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;
}  // namespace

static const NamespaceString _nss("unittests.indexcatalog");

class IndexIteratorTests {
public:
    IndexIteratorTests() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        Lock::DBLock lk(&opCtx, _nss.db(), MODE_X);
        OldClientContext ctx(&opCtx, _nss.ns());
        WriteUnitOfWork wuow(&opCtx);

        _db = ctx.db();
        _coll = _db->createCollection(&opCtx, _nss);
        _catalog = _coll->getIndexCatalog();
        wuow.commit();
    }

    ~IndexIteratorTests() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        Lock::DBLock lk(&opCtx, _nss.db(), MODE_X);
        OldClientContext ctx(&opCtx, _nss.ns());
        WriteUnitOfWork wuow(&opCtx);

        _db->dropCollection(&opCtx, _nss).transitional_ignore();
        wuow.commit();
    }

    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        dbtests::WriteContextForTests ctx(&opCtx, _nss.ns());

        int numFinishedIndexesStart = _catalog->numIndexesReady(&opCtx);

        dbtests::createIndex(&opCtx, _nss.ns(), BSON("x" << 1)).transitional_ignore();
        dbtests::createIndex(&opCtx, _nss.ns(), BSON("y" << 1)).transitional_ignore();

        ASSERT_TRUE(_catalog->numIndexesReady(&opCtx) == numFinishedIndexesStart + 2);

        std::unique_ptr<IndexCatalog::IndexIterator> ii = _catalog->getIndexIterator(&opCtx, false);
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

        ASSERT_TRUE(indexesIterated == _catalog->numIndexesReady(&opCtx));
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
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        Lock::DBLock lk(&opCtx, _nss.db(), MODE_X);
        OldClientContext ctx(&opCtx, _nss.ns());
        WriteUnitOfWork wuow(&opCtx);

        _db = ctx.db();
        _coll = _db->createCollection(&opCtx, _nss);
        _catalog = _coll->getIndexCatalog();
        wuow.commit();
    }

    ~RefreshEntry() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        Lock::DBLock lk(&opCtx, _nss.db(), MODE_X);
        OldClientContext ctx(&opCtx, _nss.ns());
        WriteUnitOfWork wuow(&opCtx);

        _db->dropCollection(&opCtx, _nss).transitional_ignore();
        wuow.commit();
    }

    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        dbtests::WriteContextForTests ctx(&opCtx, _nss.ns());
        const std::string indexName = "x_1";

        ASSERT_OK(dbtests::createIndexFromSpec(
            &opCtx,
            _nss.ns(),
            BSON("name" << indexName << "ns" << _nss.ns() << "key" << BSON("x" << 1) << "v"
                        << static_cast<int>(kIndexVersion) << "expireAfterSeconds" << 5)));

        const IndexDescriptor* desc = _catalog->findIndexByName(&opCtx, indexName);
        ASSERT(desc);
        ASSERT_EQUALS(5, desc->infoObj()["expireAfterSeconds"].numberLong());

        // Change value of "expireAfterSeconds" on disk.
        {
            WriteUnitOfWork wuow(&opCtx);
            opCtx.getServiceContext()->getStorageEngine()->getCatalog()->updateTTLSetting(
                &opCtx, _nss, "x_1", 10);
            wuow.commit();
        }

        // Verify that the catalog does not yet know of the change.
        desc = _catalog->findIndexByName(&opCtx, indexName);
        ASSERT_EQUALS(5, desc->infoObj()["expireAfterSeconds"].numberLong());

        {
            // Notify the catalog of the change.
            WriteUnitOfWork wuow(&opCtx);
            desc = _catalog->refreshEntry(&opCtx, desc);
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
}  // namespace IndexCatalogTests
