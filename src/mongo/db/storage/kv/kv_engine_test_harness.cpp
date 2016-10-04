// kv_engine_test_harness.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/storage/kv/kv_engine_test_harness.h"

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/kv/kv_catalog.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {

using std::unique_ptr;
using std::string;

namespace {
class MyOperationContext : public OperationContextNoop {
public:
    MyOperationContext(KVEngine* engine) : OperationContextNoop(engine->newRecoveryUnit()) {}
};

const std::unique_ptr<ClockSource> clock = stdx::make_unique<ClockSourceMock>();
}

TEST(KVEngineTestHarness, SimpleRS1) {
    unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    string ns = "a.b";
    unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
        rs.reset(engine->getRecordStore(&opCtx, ns, ns, CollectionOptions()));
        ASSERT(rs);
    }


    RecordId loc;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res = rs->insertRecord(&opCtx, "abc", 4, false);
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        ASSERT_EQUALS(string("abc"), rs->dataFor(&opCtx, loc).data());
    }

    {
        MyOperationContext opCtx(engine);
        std::vector<std::string> all = engine->getAllIdents(&opCtx);
        ASSERT_EQUALS(1U, all.size());
        ASSERT_EQUALS(ns, all[0]);
    }
}

TEST(KVEngineTestHarness, Restart1) {
    unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    string ns = "a.b";

    // 'loc' holds location of "abc" and is referenced after restarting engine.
    RecordId loc;
    {
        unique_ptr<RecordStore> rs;
        {
            MyOperationContext opCtx(engine);
            ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
            rs.reset(engine->getRecordStore(&opCtx, ns, ns, CollectionOptions()));
            ASSERT(rs);
        }

        {
            MyOperationContext opCtx(engine);
            WriteUnitOfWork uow(&opCtx);
            StatusWith<RecordId> res = rs->insertRecord(&opCtx, "abc", 4, false);
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }

        {
            MyOperationContext opCtx(engine);
            ASSERT_EQUALS(string("abc"), rs->dataFor(&opCtx, loc).data());
        }
    }

    engine = helper->restartEngine();

    {
        unique_ptr<RecordStore> rs;
        MyOperationContext opCtx(engine);
        rs.reset(engine->getRecordStore(&opCtx, ns, ns, CollectionOptions()));
        ASSERT_EQUALS(string("abc"), rs->dataFor(&opCtx, loc).data());
    }
}


TEST(KVEngineTestHarness, SimpleSorted1) {
    unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    string ident = "abc";
    IndexDescriptor desc(NULL, "", BSON("key" << BSON("a" << 1)));
    unique_ptr<SortedDataInterface> sorted;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createSortedDataInterface(&opCtx, ident, &desc));
        sorted.reset(engine->getSortedDataInterface(&opCtx, ident, &desc));
        ASSERT(sorted);
    }

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(sorted->insert(&opCtx, BSON("" << 5), RecordId(6, 4), true));
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        ASSERT_EQUALS(1, sorted->numEntries(&opCtx));
    }
}

TEST(KVCatalogTest, Coll1) {
    unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();

    unique_ptr<RecordStore> rs;
    unique_ptr<KVCatalog> catalog;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(engine->createRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        rs.reset(engine->getRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        catalog.reset(new KVCatalog(rs.get(), true, false, false));
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(catalog->newCollection(&opCtx, "a.b", CollectionOptions()));
        ASSERT_NOT_EQUALS("a.b", catalog->getCollectionIdent("a.b"));
        uow.commit();
    }

    string ident = catalog->getCollectionIdent("a.b");
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        catalog.reset(new KVCatalog(rs.get(), true, false, false));
        catalog->init(&opCtx);
        uow.commit();
    }
    ASSERT_EQUALS(ident, catalog->getCollectionIdent("a.b"));

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        catalog->dropCollection(&opCtx, "a.b");
        catalog->newCollection(&opCtx, "a.b", CollectionOptions());
        uow.commit();
    }
    ASSERT_NOT_EQUALS(ident, catalog->getCollectionIdent("a.b"));
}


TEST(KVCatalogTest, Idx1) {
    unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();

    unique_ptr<RecordStore> rs;
    unique_ptr<KVCatalog> catalog;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(engine->createRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        rs.reset(engine->getRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        catalog.reset(new KVCatalog(rs.get(), true, false, false));
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(catalog->newCollection(&opCtx, "a.b", CollectionOptions()));
        ASSERT_NOT_EQUALS("a.b", catalog->getCollectionIdent("a.b"));
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getCollectionIdent("a.b")));
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);

        BSONCollectionCatalogEntry::MetaData md;
        md.ns = "a.b";
        md.indexes.push_back(BSONCollectionCatalogEntry::IndexMetaData(BSON("name"
                                                                            << "foo"),
                                                                       false,
                                                                       RecordId(),
                                                                       false));
        catalog->putMetaData(&opCtx, "a.b", md);
        uow.commit();
    }

    string idxIndent;
    {
        MyOperationContext opCtx(engine);
        idxIndent = catalog->getIndexIdent(&opCtx, "a.b", "foo");
    }

    {
        MyOperationContext opCtx(engine);
        ASSERT_EQUALS(idxIndent, catalog->getIndexIdent(&opCtx, "a.b", "foo"));
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getIndexIdent(&opCtx, "a.b", "foo")));
    }

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);

        BSONCollectionCatalogEntry::MetaData md;
        md.ns = "a.b";
        catalog->putMetaData(&opCtx, "a.b", md);  // remove index
        md.indexes.push_back(BSONCollectionCatalogEntry::IndexMetaData(BSON("name"
                                                                            << "foo"),
                                                                       false,
                                                                       RecordId(),
                                                                       false));
        catalog->putMetaData(&opCtx, "a.b", md);
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        ASSERT_NOT_EQUALS(idxIndent, catalog->getIndexIdent(&opCtx, "a.b", "foo"));
    }
}

TEST(KVCatalogTest, DirectoryPerDb1) {
    unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();

    unique_ptr<RecordStore> rs;
    unique_ptr<KVCatalog> catalog;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(engine->createRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        rs.reset(engine->getRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        catalog.reset(new KVCatalog(rs.get(), true, true, false));
        uow.commit();
    }

    {  // collection
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(catalog->newCollection(&opCtx, "a.b", CollectionOptions()));
        ASSERT_STRING_CONTAINS(catalog->getCollectionIdent("a.b"), "a/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getCollectionIdent("a.b")));
        uow.commit();
    }

    {  // index
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);

        BSONCollectionCatalogEntry::MetaData md;
        md.ns = "a.b";
        md.indexes.push_back(BSONCollectionCatalogEntry::IndexMetaData(BSON("name"
                                                                            << "foo"),
                                                                       false,
                                                                       RecordId(),
                                                                       false));
        catalog->putMetaData(&opCtx, "a.b", md);
        ASSERT_STRING_CONTAINS(catalog->getIndexIdent(&opCtx, "a.b", "foo"), "a/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getIndexIdent(&opCtx, "a.b", "foo")));
        uow.commit();
    }
}

TEST(KVCatalogTest, Split1) {
    unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();

    unique_ptr<RecordStore> rs;
    unique_ptr<KVCatalog> catalog;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(engine->createRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        rs.reset(engine->getRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        catalog.reset(new KVCatalog(rs.get(), true, false, true));
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(catalog->newCollection(&opCtx, "a.b", CollectionOptions()));
        ASSERT_STRING_CONTAINS(catalog->getCollectionIdent("a.b"), "collection/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getCollectionIdent("a.b")));
        uow.commit();
    }

    {  // index
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);

        BSONCollectionCatalogEntry::MetaData md;
        md.ns = "a.b";
        md.indexes.push_back(BSONCollectionCatalogEntry::IndexMetaData(BSON("name"
                                                                            << "foo"),
                                                                       false,
                                                                       RecordId(),
                                                                       false));
        catalog->putMetaData(&opCtx, "a.b", md);
        ASSERT_STRING_CONTAINS(catalog->getIndexIdent(&opCtx, "a.b", "foo"), "index/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getIndexIdent(&opCtx, "a.b", "foo")));
        uow.commit();
    }
}

TEST(KVCatalogTest, DirectoryPerAndSplit1) {
    unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();

    unique_ptr<RecordStore> rs;
    unique_ptr<KVCatalog> catalog;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(engine->createRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        rs.reset(engine->getRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        catalog.reset(new KVCatalog(rs.get(), true, true, true));
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(catalog->newCollection(&opCtx, "a.b", CollectionOptions()));
        ASSERT_STRING_CONTAINS(catalog->getCollectionIdent("a.b"), "a/collection/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getCollectionIdent("a.b")));
        uow.commit();
    }

    {  // index
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);

        BSONCollectionCatalogEntry::MetaData md;
        md.ns = "a.b";
        md.indexes.push_back(BSONCollectionCatalogEntry::IndexMetaData(BSON("name"
                                                                            << "foo"),
                                                                       false,
                                                                       RecordId(),
                                                                       false));
        catalog->putMetaData(&opCtx, "a.b", md);
        ASSERT_STRING_CONTAINS(catalog->getIndexIdent(&opCtx, "a.b", "foo"), "a/index/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getIndexIdent(&opCtx, "a.b", "foo")));
        uow.commit();
    }
}
}
