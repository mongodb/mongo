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

#include "mongo/db/storage/kv/kv_engine_test_harness.h"

#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/durable_catalog_test_fixture.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

std::function<std::unique_ptr<KVHarnessHelper>()> basicFactory =
    []() -> std::unique_ptr<KVHarnessHelper> { fassertFailed(40355); };

class MyOperationContext : public OperationContextNoop {
public:
    MyOperationContext(KVEngine* engine) : OperationContextNoop(engine->newRecoveryUnit()) {}
};

const std::unique_ptr<ClockSource> clock = std::make_unique<ClockSourceMock>();

TEST(KVEngineTestHarness, SimpleRS1) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    std::string ns = "a.b";
    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
        ASSERT(rs);
    }


    RecordId loc;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res = rs->insertRecord(&opCtx, "abc", 4, Timestamp());
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        ASSERT_EQUALS(std::string("abc"), rs->dataFor(&opCtx, loc).data());
    }

    {
        MyOperationContext opCtx(engine);
        std::vector<std::string> all = engine->getAllIdents(&opCtx);
        ASSERT_EQUALS(1U, all.size());
        ASSERT_EQUALS(ns, all[0]);
    }
}

TEST(KVEngineTestHarness, Restart1) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    std::string ns = "a.b";

    // 'loc' holds location of "abc" and is referenced after restarting engine.
    RecordId loc;
    {
        std::unique_ptr<RecordStore> rs;
        {
            MyOperationContext opCtx(engine);
            ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
            rs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
            ASSERT(rs);
        }

        {
            MyOperationContext opCtx(engine);
            WriteUnitOfWork uow(&opCtx);
            StatusWith<RecordId> res = rs->insertRecord(&opCtx, "abc", 4, Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }

        {
            MyOperationContext opCtx(engine);
            ASSERT_EQUALS(std::string("abc"), rs->dataFor(&opCtx, loc).data());
        }
    }

    engine = helper->restartEngine();

    {
        std::unique_ptr<RecordStore> rs;
        MyOperationContext opCtx(engine);
        rs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
        ASSERT_EQUALS(std::string("abc"), rs->dataFor(&opCtx, loc).data());
    }
}


TEST(KVEngineTestHarness, SimpleSorted1) {
    setGlobalServiceContext(ServiceContext::make());
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    std::string ident = "abc";
    auto ns = NamespaceString("mydb.mycoll");

    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(engine->createRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, "catalog", "catalog", CollectionOptions());
        uow.commit();
    }


    std::unique_ptr<CollectionImpl> collection;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        collection = std::make_unique<CollectionImpl>(&opCtx, ns, UUID::gen(), std::move(rs));
        uow.commit();
    }

    IndexDescriptor desc(collection.get(),
                         "",
                         BSON("v" << static_cast<int>(IndexDescriptor::kLatestIndexVersion) << "key"
                                  << BSON("a" << 1)));
    std::unique_ptr<SortedDataInterface> sorted;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createSortedDataInterface(&opCtx, CollectionOptions(), ident, &desc));
        sorted = engine->getSortedDataInterface(&opCtx, ident, &desc);
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

TEST(KVEngineTestHarness, TemporaryRecordStoreSimple) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    std::string ident = "temptemp";
    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        rs = engine->makeTemporaryRecordStore(&opCtx, ident);
        ASSERT(rs);
    }

    RecordId loc;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res = rs->insertRecord(&opCtx, "abc", 4, Timestamp());
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        ASSERT_EQUALS(std::string("abc"), rs->dataFor(&opCtx, loc).data());

        std::vector<std::string> all = engine->getAllIdents(&opCtx);
        ASSERT_EQUALS(1U, all.size());
        ASSERT_EQUALS(ident, all[0]);

        WriteUnitOfWork wuow(&opCtx);
        ASSERT_OK(engine->dropIdent(&opCtx, ident));
        wuow.commit();
    }
}

TEST(KVEngineTestHarness, AllDurableTimestamp) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    if (!engine->supportsDocLocking())
        return;

    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        CollectionOptions options;
        options.capped = true;
        options.cappedSize = 10240;
        options.cappedMaxDocs = -1;

        NamespaceString oplogNss("local.oplog.rs");
        ASSERT_OK(engine->createRecordStore(&opCtx, oplogNss.ns(), "ident", options));
        rs = engine->getRecordStore(&opCtx, oplogNss.ns(), "ident", options);
        ASSERT(rs);
    }
    {
        Timestamp t11(1, 1);
        Timestamp t12(1, 2);
        Timestamp t21(2, 1);

        auto t11Doc = BSON("ts" << t11);
        auto t12Doc = BSON("ts" << t12);
        auto t21Doc = BSON("ts" << t21);

        Timestamp allDurable = engine->getAllDurableTimestamp();
        MyOperationContext opCtx1(engine);
        WriteUnitOfWork uow1(&opCtx1);
        ASSERT_EQ(invariant(rs->insertRecord(
                      &opCtx1, t11Doc.objdata(), t11Doc.objsize(), Timestamp::min())),
                  RecordId(1, 1));

        Timestamp lastAllDurable = allDurable;
        allDurable = engine->getAllDurableTimestamp();
        ASSERT_GTE(allDurable, lastAllDurable);
        ASSERT_LT(allDurable, t11);

        MyOperationContext opCtx2(engine);
        WriteUnitOfWork uow2(&opCtx2);
        ASSERT_EQ(invariant(rs->insertRecord(
                      &opCtx2, t21Doc.objdata(), t21Doc.objsize(), Timestamp::min())),
                  RecordId(2, 1));
        uow2.commit();

        lastAllDurable = allDurable;
        allDurable = engine->getAllDurableTimestamp();
        ASSERT_GTE(allDurable, lastAllDurable);
        ASSERT_LT(allDurable, t11);

        ASSERT_EQ(invariant(rs->insertRecord(
                      &opCtx1, t12Doc.objdata(), t12Doc.objsize(), Timestamp::min())),
                  RecordId(1, 2));

        lastAllDurable = allDurable;
        allDurable = engine->getAllDurableTimestamp();
        ASSERT_GTE(allDurable, lastAllDurable);
        ASSERT_LT(allDurable, t11);

        uow1.commit();

        lastAllDurable = allDurable;
        allDurable = engine->getAllDurableTimestamp();
        ASSERT_GTE(allDurable, lastAllDurable);
        ASSERT_LTE(allDurable, t21);
    }
}

TEST_F(DurableCatalogImplTest, Coll1) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();

    std::unique_ptr<RecordStore> rs;
    std::unique_ptr<DurableCatalogImpl> catalog;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(engine->createRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, "catalog", "catalog", CollectionOptions());
        catalog = std::make_unique<DurableCatalogImpl>(rs.get(), false, false, nullptr);
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(newCollection(&opCtx,
                                NamespaceString("a.b"),
                                CollectionOptions(),
                                KVPrefix::kNotPrefixed,
                                catalog.get()));
        ASSERT_NOT_EQUALS("a.b", catalog->getCollectionIdent(NamespaceString("a.b")));
        uow.commit();
    }

    std::string ident = catalog->getCollectionIdent(NamespaceString("a.b"));
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        catalog = std::make_unique<DurableCatalogImpl>(rs.get(), false, false, nullptr);
        catalog->init(&opCtx);
        uow.commit();
    }
    ASSERT_EQUALS(ident, catalog->getCollectionIdent(NamespaceString("a.b")));

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        dropCollection(&opCtx, "a.b", catalog.get()).transitional_ignore();
        newCollection(&opCtx,
                      NamespaceString("a.b"),
                      CollectionOptions(),
                      KVPrefix::kNotPrefixed,
                      catalog.get())
            .transitional_ignore();
        uow.commit();
    }
    ASSERT_NOT_EQUALS(ident, catalog->getCollectionIdent(NamespaceString("a.b")));
}

TEST_F(DurableCatalogImplTest, Idx1) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();

    std::unique_ptr<RecordStore> rs;
    std::unique_ptr<DurableCatalogImpl> catalog;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(engine->createRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, "catalog", "catalog", CollectionOptions());
        catalog = std::make_unique<DurableCatalogImpl>(rs.get(), false, false, nullptr);
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(newCollection(&opCtx,
                                NamespaceString("a.b"),
                                CollectionOptions(),
                                KVPrefix::kNotPrefixed,
                                catalog.get()));
        ASSERT_NOT_EQUALS("a.b", catalog->getCollectionIdent(NamespaceString("a.b")));
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getCollectionIdent(NamespaceString("a.b"))));
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);

        BSONCollectionCatalogEntry::MetaData md;
        md.ns = "a.b";

        BSONCollectionCatalogEntry::IndexMetaData imd;
        imd.spec = BSON("name"
                        << "foo");
        imd.ready = false;
        imd.multikey = false;
        imd.prefix = KVPrefix::kNotPrefixed;
        imd.isBackgroundSecondaryBuild = false;
        md.indexes.push_back(imd);
        catalog->putMetaData(&opCtx, NamespaceString("a.b"), md);
        uow.commit();
    }

    std::string idxIndent;
    {
        MyOperationContext opCtx(engine);
        idxIndent = catalog->getIndexIdent(&opCtx, NamespaceString("a.b"), "foo");
    }

    {
        MyOperationContext opCtx(engine);
        ASSERT_EQUALS(idxIndent, catalog->getIndexIdent(&opCtx, NamespaceString("a.b"), "foo"));
        ASSERT_TRUE(catalog->isUserDataIdent(
            catalog->getIndexIdent(&opCtx, NamespaceString("a.b"), "foo")));
    }

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);

        BSONCollectionCatalogEntry::MetaData md;
        md.ns = "a.b";
        catalog->putMetaData(&opCtx, NamespaceString("a.b"), md);  // remove index

        BSONCollectionCatalogEntry::IndexMetaData imd;
        imd.spec = BSON("name"
                        << "foo");
        imd.ready = false;
        imd.multikey = false;
        imd.prefix = KVPrefix::kNotPrefixed;
        imd.isBackgroundSecondaryBuild = false;
        md.indexes.push_back(imd);
        catalog->putMetaData(&opCtx, NamespaceString("a.b"), md);
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        ASSERT_NOT_EQUALS(idxIndent, catalog->getIndexIdent(&opCtx, NamespaceString("a.b"), "foo"));
    }
}

TEST_F(DurableCatalogImplTest, DirectoryPerDb1) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();

    std::unique_ptr<RecordStore> rs;
    std::unique_ptr<DurableCatalogImpl> catalog;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(engine->createRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, "catalog", "catalog", CollectionOptions());
        catalog = std::make_unique<DurableCatalogImpl>(rs.get(), true, false, nullptr);
        uow.commit();
    }

    {  // collection
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(newCollection(&opCtx,
                                NamespaceString("a.b"),
                                CollectionOptions(),
                                KVPrefix::kNotPrefixed,
                                catalog.get()));
        ASSERT_STRING_CONTAINS(catalog->getCollectionIdent(NamespaceString("a.b")), "a/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getCollectionIdent(NamespaceString("a.b"))));
        uow.commit();
    }

    {  // index
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);

        BSONCollectionCatalogEntry::MetaData md;
        md.ns = "a.b";

        BSONCollectionCatalogEntry::IndexMetaData imd;
        imd.spec = BSON("name"
                        << "foo");
        imd.ready = false;
        imd.multikey = false;
        imd.prefix = KVPrefix::kNotPrefixed;
        imd.isBackgroundSecondaryBuild = false;
        md.indexes.push_back(imd);
        catalog->putMetaData(&opCtx, NamespaceString("a.b"), md);
        ASSERT_STRING_CONTAINS(catalog->getIndexIdent(&opCtx, NamespaceString("a.b"), "foo"), "a/");
        ASSERT_TRUE(catalog->isUserDataIdent(
            catalog->getIndexIdent(&opCtx, NamespaceString("a.b"), "foo")));
        uow.commit();
    }
}

TEST_F(DurableCatalogImplTest, Split1) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();

    std::unique_ptr<RecordStore> rs;
    std::unique_ptr<DurableCatalogImpl> catalog;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(engine->createRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, "catalog", "catalog", CollectionOptions());
        catalog = std::make_unique<DurableCatalogImpl>(rs.get(), false, true, nullptr);
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(newCollection(&opCtx,
                                NamespaceString("a.b"),
                                CollectionOptions(),
                                KVPrefix::kNotPrefixed,
                                catalog.get()));
        ASSERT_STRING_CONTAINS(catalog->getCollectionIdent(NamespaceString("a.b")), "collection/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getCollectionIdent(NamespaceString("a.b"))));
        uow.commit();
    }

    {  // index
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);

        BSONCollectionCatalogEntry::MetaData md;
        md.ns = "a.b";

        BSONCollectionCatalogEntry::IndexMetaData imd;
        imd.spec = BSON("name"
                        << "foo");
        imd.ready = false;
        imd.multikey = false;
        imd.prefix = KVPrefix::kNotPrefixed;
        imd.isBackgroundSecondaryBuild = false;
        md.indexes.push_back(imd);
        catalog->putMetaData(&opCtx, NamespaceString("a.b"), md);
        ASSERT_STRING_CONTAINS(catalog->getIndexIdent(&opCtx, NamespaceString("a.b"), "foo"),
                               "index/");
        ASSERT_TRUE(catalog->isUserDataIdent(
            catalog->getIndexIdent(&opCtx, NamespaceString("a.b"), "foo")));
        uow.commit();
    }
}

TEST_F(DurableCatalogImplTest, DirectoryPerAndSplit1) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();

    std::unique_ptr<RecordStore> rs;
    std::unique_ptr<DurableCatalogImpl> catalog;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(engine->createRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, "catalog", "catalog", CollectionOptions());
        catalog = std::make_unique<DurableCatalogImpl>(rs.get(), true, true, nullptr);
        uow.commit();
    }

    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(newCollection(&opCtx,
                                NamespaceString("a.b"),
                                CollectionOptions(),
                                KVPrefix::kNotPrefixed,
                                catalog.get()));
        ASSERT_STRING_CONTAINS(catalog->getCollectionIdent(NamespaceString("a.b")),
                               "a/collection/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getCollectionIdent(NamespaceString("a.b"))));
        uow.commit();
    }

    {  // index
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);

        BSONCollectionCatalogEntry::MetaData md;
        md.ns = "a.b";

        BSONCollectionCatalogEntry::IndexMetaData imd;
        imd.spec = BSON("name"
                        << "foo");
        imd.ready = false;
        imd.multikey = false;
        imd.prefix = KVPrefix::kNotPrefixed;
        imd.isBackgroundSecondaryBuild = false;
        md.indexes.push_back(imd);
        catalog->putMetaData(&opCtx, NamespaceString("a.b"), md);
        ASSERT_STRING_CONTAINS(catalog->getIndexIdent(&opCtx, NamespaceString("a.b"), "foo"),
                               "a/index/");
        ASSERT_TRUE(catalog->isUserDataIdent(
            catalog->getIndexIdent(&opCtx, NamespaceString("a.b"), "foo")));
        uow.commit();
    }
}

TEST_F(DurableCatalogImplTest, RestartForPrefixes) {
    storageGlobalParams.groupCollections = true;
    ON_BLOCK_EXIT([&] { storageGlobalParams.groupCollections = false; });

    KVPrefix abCollPrefix = KVPrefix::getNextPrefix(NamespaceString("a.b"));
    KVPrefix fooIndexPrefix = KVPrefix::getNextPrefix(NamespaceString("a.b"));

    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    {
        std::unique_ptr<RecordStore> rs;
        std::unique_ptr<DurableCatalogImpl> catalog;
        {
            MyOperationContext opCtx(engine);
            WriteUnitOfWork uow(&opCtx);
            ASSERT_OK(engine->createRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
            rs = engine->getRecordStore(&opCtx, "catalog", "catalog", CollectionOptions());
            catalog = std::make_unique<DurableCatalogImpl>(rs.get(), false, false, nullptr);
            uow.commit();
        }

        {
            MyOperationContext opCtx(engine);
            WriteUnitOfWork uow(&opCtx);
            ASSERT_OK(newCollection(
                &opCtx, NamespaceString("a.b"), CollectionOptions(), abCollPrefix, catalog.get()));
            ASSERT_NOT_EQUALS("a.b", catalog->getCollectionIdent(NamespaceString("a.b")));
            ASSERT_TRUE(
                catalog->isUserDataIdent(catalog->getCollectionIdent(NamespaceString("a.b"))));
            uow.commit();
        }

        {
            MyOperationContext opCtx(engine);
            WriteUnitOfWork uow(&opCtx);

            BSONCollectionCatalogEntry::MetaData md;
            md.ns = "a.b";

            BSONCollectionCatalogEntry::IndexMetaData imd;
            imd.spec = BSON("name"
                            << "foo");
            imd.ready = false;
            imd.multikey = false;
            imd.prefix = fooIndexPrefix;
            imd.isBackgroundSecondaryBuild = false;
            md.indexes.push_back(imd);
            md.prefix = abCollPrefix;
            catalog->putMetaData(&opCtx, NamespaceString("a.b"), md);
            uow.commit();
        }
    }

    engine = helper->restartEngine();
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        std::unique_ptr<RecordStore> rs =
            engine->getRecordStore(&opCtx, "catalog", "catalog", CollectionOptions());
        auto catalog = std::make_unique<DurableCatalogImpl>(rs.get(), false, false, nullptr);
        catalog->init(&opCtx);

        const BSONCollectionCatalogEntry::MetaData md =
            catalog->getMetaData(&opCtx, NamespaceString("a.b"));
        ASSERT_EQ("a.b", md.ns);
        ASSERT_EQ(abCollPrefix, md.prefix);
        ASSERT_EQ(fooIndexPrefix, md.indexes[md.findIndexOffset("foo")].prefix);
    }
}

TEST_F(DurableCatalogImplTest, BackupImplemented) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->beginBackup(&opCtx));
        engine->endBackup(&opCtx);
    }
}

DEATH_TEST_F(DurableCatalogImplTest, TerminateOnNonNumericIndexVersion, "Fatal Assertion 50942") {
    setGlobalServiceContext(ServiceContext::make());
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    std::string ident = "abc";
    auto ns = NamespaceString("mydb.mycoll");

    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        ASSERT_OK(engine->createRecordStore(&opCtx, "catalog", "catalog", CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, "catalog", "catalog", CollectionOptions());
        uow.commit();
    }

    std::unique_ptr<CollectionImpl> collection;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        collection = std::make_unique<CollectionImpl>(&opCtx, ns, UUID::gen(), std::move(rs));
        uow.commit();
    }

    IndexDescriptor desc(collection.get(),
                         "",
                         BSON("v"
                              << "1"
                              << "key" << BSON("a" << 1)));
    std::unique_ptr<SortedDataInterface> sorted;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createSortedDataInterface(&opCtx, CollectionOptions(), ident, &desc));
        sorted = engine->getSortedDataInterface(&opCtx, ident, &desc);
        ASSERT(sorted);
    }
}

}  // namespace

std::unique_ptr<KVHarnessHelper> KVHarnessHelper::create() {
    return basicFactory();
};

void KVHarnessHelper::registerFactory(std::function<std::unique_ptr<KVHarnessHelper>()> factory) {
    basicFactory = std::move(factory);
};

}  // namespace mongo
