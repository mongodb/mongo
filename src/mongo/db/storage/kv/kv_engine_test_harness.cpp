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
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/durable_catalog_impl.h"
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

class ClientAndCtx {
public:
    ClientAndCtx(ServiceContext::UniqueClient client, ServiceContext::UniqueOperationContext opCtx)
        : _client(std::move(client)), _opCtx(std::move(opCtx)) {}

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    Client* client() {
        return _client.get();
    }

    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
};

class DurableCatalogImplTest : public ServiceContextTest {
protected:
    void setUp() override {
        helper = KVHarnessHelper::create(getServiceContext());
        invariant(hasGlobalServiceContext());
    }

    ClientAndCtx makeClientAndCtx(const std::string& clientName) {
        auto client = getGlobalServiceContext()->makeClient(clientName);
        auto opCtx = client->makeOperationContext();
        opCtx->setRecoveryUnit(
            std::unique_ptr<RecoveryUnit>(helper->getEngine()->newRecoveryUnit()),
            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        return {std::move(client), std::move(opCtx)};
    }

    RecordId newCollection(OperationContext* opCtx,
                           const NamespaceString& ns,
                           const CollectionOptions& options,
                           KVPrefix prefix,
                           DurableCatalogImpl* catalog) {
        auto swEntry = catalog->_addEntry(opCtx, ns, options, prefix);
        ASSERT_OK(swEntry.getStatus());
        return swEntry.getValue().catalogId;
    }

    Status renameCollection(OperationContext* opCtx,
                            RecordId catalogId,
                            StringData toNS,
                            bool stayTemp,
                            DurableCatalogImpl* catalog) {
        return catalog->_replaceEntry(opCtx, catalogId, NamespaceString(toNS), stayTemp);
    }

    Status dropCollection(OperationContext* opCtx,
                          RecordId catalogId,
                          DurableCatalogImpl* catalog) {
        return catalog->_removeEntry(opCtx, catalogId);
    }

    std::unique_ptr<KVHarnessHelper> helper;
};

namespace {

std::function<std::unique_ptr<KVHarnessHelper>(ServiceContext*)> basicFactory =
    [](ServiceContext*) -> std::unique_ptr<KVHarnessHelper> { fassertFailed(40355); };

class KVEngineTestHarness : public ServiceContextTest {
protected:
    ServiceContext::UniqueOperationContext _makeOperationContext(KVEngine* engine) {
        auto opCtx = makeOperationContext();
        opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(engine->newRecoveryUnit()),
                               WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        opCtx->swapLockState(std::make_unique<LockerNoop>(), WithLock::withoutLock());
        return opCtx;
    }

    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
    _makeOperationContexts(KVEngine* engine, unsigned num) {
        std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
            opCtxs;
        opCtxs.reserve(num);

        for (unsigned i = 0; i < num; ++i) {
            auto client = getServiceContext()->makeClient(std::to_string(i));

            auto opCtx = client->makeOperationContext();
            opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(engine->newRecoveryUnit()),
                                   WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
            opCtx->swapLockState(std::make_unique<LockerNoop>(), WithLock::withoutLock());

            opCtxs.emplace_back(std::move(client), std::move(opCtx));
        }

        return opCtxs;
    }
};

const std::unique_ptr<ClockSource> clock = std::make_unique<ClockSourceMock>();

TEST_F(KVEngineTestHarness, SimpleRS1) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    std::string ns = "a.b";
    std::unique_ptr<RecordStore> rs;
    {
        auto opCtx = _makeOperationContext(engine);
        ASSERT_OK(engine->createRecordStore(opCtx.get(), ns, ns, CollectionOptions()));
        rs = engine->getRecordStore(opCtx.get(), ns, ns, CollectionOptions());
        ASSERT(rs);
    }


    RecordId loc;
    {
        auto opCtx = _makeOperationContext(engine);
        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "abc", 4, Timestamp());
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        uow.commit();
    }

    {
        auto opCtx = _makeOperationContext(engine);
        ASSERT_EQUALS(std::string("abc"), rs->dataFor(opCtx.get(), loc).data());
    }

    {
        auto opCtx = _makeOperationContext(engine);
        std::vector<std::string> all = engine->getAllIdents(opCtx.get());
        ASSERT_EQUALS(1U, all.size());
        ASSERT_EQUALS(ns, all[0]);
    }
}

TEST_F(KVEngineTestHarness, Restart1) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    std::string ns = "a.b";

    // 'loc' holds location of "abc" and is referenced after restarting engine.
    RecordId loc;
    {
        std::unique_ptr<RecordStore> rs;
        {
            auto opCtx = _makeOperationContext(engine);
            ASSERT_OK(engine->createRecordStore(opCtx.get(), ns, ns, CollectionOptions()));
            rs = engine->getRecordStore(opCtx.get(), ns, ns, CollectionOptions());
            ASSERT(rs);
        }

        {
            auto opCtx = _makeOperationContext(engine);
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "abc", 4, Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }

        {
            auto opCtx = _makeOperationContext(engine);
            ASSERT_EQUALS(std::string("abc"), rs->dataFor(opCtx.get(), loc).data());
        }
    }

    engine = helper->restartEngine();

    {
        std::unique_ptr<RecordStore> rs;
        auto opCtx = _makeOperationContext(engine);
        rs = engine->getRecordStore(opCtx.get(), ns, ns, CollectionOptions());
        ASSERT_EQUALS(std::string("abc"), rs->dataFor(opCtx.get(), loc).data());
    }
}


TEST_F(KVEngineTestHarness, SimpleSorted1) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    std::string ident = "abc";
    auto ns = NamespaceString("mydb.mycoll");

    std::unique_ptr<RecordStore> rs;
    {
        auto opCtx = _makeOperationContext(engine);
        WriteUnitOfWork uow(opCtx.get());
        ASSERT_OK(
            engine->createRecordStore(opCtx.get(), "catalog", "catalog", CollectionOptions()));
        rs = engine->getRecordStore(opCtx.get(), "catalog", "catalog", CollectionOptions());
        uow.commit();
    }


    std::unique_ptr<CollectionImpl> collection;
    {
        auto opCtx = _makeOperationContext(engine);
        WriteUnitOfWork uow(opCtx.get());
        collection = std::make_unique<CollectionImpl>(
            opCtx.get(), ns, RecordId(0), UUID::gen(), std::move(rs));
        uow.commit();
    }

    IndexDescriptor desc(collection.get(),
                         "",
                         BSON("v" << static_cast<int>(IndexDescriptor::kLatestIndexVersion) << "key"
                                  << BSON("a" << 1)));
    std::unique_ptr<SortedDataInterface> sorted;
    {
        auto opCtx = _makeOperationContext(engine);
        ASSERT_OK(
            engine->createSortedDataInterface(opCtx.get(), CollectionOptions(), ident, &desc));
        sorted = engine->getSortedDataInterface(opCtx.get(), ident, &desc);
        ASSERT(sorted);
    }

    {
        auto opCtx = _makeOperationContext(engine);
        WriteUnitOfWork uow(opCtx.get());
        const RecordId recordId(6, 4);
        const KeyString::Value keyString =
            KeyString::HeapBuilder(
                sorted->getKeyStringVersion(), BSON("" << 5), sorted->getOrdering(), recordId)
                .release();
        ASSERT_OK(sorted->insert(opCtx.get(), keyString, true));
        uow.commit();
    }

    {
        auto opCtx = _makeOperationContext(engine);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }
}

TEST_F(KVEngineTestHarness, TemporaryRecordStoreSimple) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    std::string ident = "temptemp";
    std::unique_ptr<RecordStore> rs;
    {
        auto opCtx = _makeOperationContext(engine);
        rs = engine->makeTemporaryRecordStore(opCtx.get(), ident);
        ASSERT(rs);
    }

    RecordId loc;
    {
        auto opCtx = _makeOperationContext(engine);
        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "abc", 4, Timestamp());
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        uow.commit();
    }

    {
        auto opCtx = _makeOperationContext(engine);
        ASSERT_EQUALS(std::string("abc"), rs->dataFor(opCtx.get(), loc).data());

        std::vector<std::string> all = engine->getAllIdents(opCtx.get());
        ASSERT_EQUALS(1U, all.size());
        ASSERT_EQUALS(ident, all[0]);

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(engine->dropIdent(opCtx.get(), opCtx->recoveryUnit(), ident));
        wuow.commit();
    }
}

TEST_F(KVEngineTestHarness, AllDurableTimestamp) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    if (!engine->supportsDocLocking())
        return;

    std::unique_ptr<RecordStore> rs;
    {
        auto opCtx = _makeOperationContext(engine);
        WriteUnitOfWork uow(opCtx.get());
        CollectionOptions options;
        options.capped = true;
        options.cappedSize = 10240;
        options.cappedMaxDocs = -1;

        NamespaceString oplogNss("local.oplog.rs");
        ASSERT_OK(engine->createRecordStore(opCtx.get(), oplogNss.ns(), "ident", options));
        rs = engine->getRecordStore(opCtx.get(), oplogNss.ns(), "ident", options);
        ASSERT(rs);
    }
    {
        auto opCtxs = _makeOperationContexts(engine, 2);

        Timestamp t11(1, 1);
        Timestamp t12(1, 2);
        Timestamp t21(2, 1);

        auto t11Doc = BSON("ts" << t11);
        auto t12Doc = BSON("ts" << t12);
        auto t21Doc = BSON("ts" << t21);

        Timestamp allDurable = engine->getAllDurableTimestamp();
        auto opCtx1 = opCtxs[0].second.get();
        WriteUnitOfWork uow1(opCtx1);
        ASSERT_EQ(invariant(rs->insertRecord(
                      opCtx1, t11Doc.objdata(), t11Doc.objsize(), Timestamp::min())),
                  RecordId(1, 1));

        Timestamp lastAllDurable = allDurable;
        allDurable = engine->getAllDurableTimestamp();
        ASSERT_GTE(allDurable, lastAllDurable);
        ASSERT_LT(allDurable, t11);

        auto opCtx2 = opCtxs[1].second.get();
        WriteUnitOfWork uow2(opCtx2);
        ASSERT_EQ(invariant(rs->insertRecord(
                      opCtx2, t21Doc.objdata(), t21Doc.objsize(), Timestamp::min())),
                  RecordId(2, 1));
        uow2.commit();

        lastAllDurable = allDurable;
        allDurable = engine->getAllDurableTimestamp();
        ASSERT_GTE(allDurable, lastAllDurable);
        ASSERT_LT(allDurable, t11);

        ASSERT_EQ(invariant(rs->insertRecord(
                      opCtx1, t12Doc.objdata(), t12Doc.objsize(), Timestamp::min())),
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
    KVEngine* engine = helper->getEngine();

    std::unique_ptr<RecordStore> rs;
    std::unique_ptr<DurableCatalogImpl> catalog;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        ASSERT_OK(engine->createRecordStore(opCtx, "catalog", "catalog", CollectionOptions()));
        rs = engine->getRecordStore(opCtx, "catalog", "catalog", CollectionOptions());
        catalog = std::make_unique<DurableCatalogImpl>(rs.get(), false, false, nullptr);
        uow.commit();
    }

    RecordId catalogId;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        catalogId = newCollection(opCtx,
                                  NamespaceString("a.b"),
                                  CollectionOptions(),
                                  KVPrefix::kNotPrefixed,
                                  catalog.get());
        ASSERT_NOT_EQUALS("a.b", catalog->getEntry(catalogId).ident);
        uow.commit();
    }

    std::string ident = catalog->getEntry(catalogId).ident;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        catalog = std::make_unique<DurableCatalogImpl>(rs.get(), false, false, nullptr);
        catalog->init(opCtx);
        uow.commit();
    }
    ASSERT_EQUALS(ident, catalog->getEntry(catalogId).ident);

    RecordId newCatalogId;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        dropCollection(opCtx, catalogId, catalog.get()).transitional_ignore();
        newCatalogId = newCollection(opCtx,
                                     NamespaceString("a.b"),
                                     CollectionOptions(),
                                     KVPrefix::kNotPrefixed,
                                     catalog.get());
        uow.commit();
    }
    ASSERT_NOT_EQUALS(ident, catalog->getEntry(newCatalogId).ident);
}

TEST_F(DurableCatalogImplTest, Idx1) {
    KVEngine* engine = helper->getEngine();

    std::unique_ptr<RecordStore> rs;
    std::unique_ptr<DurableCatalogImpl> catalog;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        ASSERT_OK(engine->createRecordStore(opCtx, "catalog", "catalog", CollectionOptions()));
        rs = engine->getRecordStore(opCtx, "catalog", "catalog", CollectionOptions());
        catalog = std::make_unique<DurableCatalogImpl>(rs.get(), false, false, nullptr);
        uow.commit();
    }

    RecordId catalogId;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        catalogId = newCollection(opCtx,
                                  NamespaceString("a.b"),
                                  CollectionOptions(),
                                  KVPrefix::kNotPrefixed,
                                  catalog.get());
        ASSERT_NOT_EQUALS("a.b", catalog->getEntry(catalogId).ident);
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getEntry(catalogId).ident));
        uow.commit();
    }

    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);

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
        catalog->putMetaData(opCtx, catalogId, md);
        uow.commit();
    }

    std::string idxIndent;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        idxIndent = catalog->getIndexIdent(opCtx, catalogId, "foo");
    }

    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        ASSERT_EQUALS(idxIndent, catalog->getIndexIdent(opCtx, catalogId, "foo"));
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getIndexIdent(opCtx, catalogId, "foo")));
    }

    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);

        BSONCollectionCatalogEntry::MetaData md;
        md.ns = "a.b";
        catalog->putMetaData(opCtx, catalogId, md);  // remove index

        BSONCollectionCatalogEntry::IndexMetaData imd;
        imd.spec = BSON("name"
                        << "foo");
        imd.ready = false;
        imd.multikey = false;
        imd.prefix = KVPrefix::kNotPrefixed;
        imd.isBackgroundSecondaryBuild = false;
        md.indexes.push_back(imd);
        catalog->putMetaData(opCtx, catalogId, md);
        uow.commit();
    }

    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        ASSERT_NOT_EQUALS(idxIndent, catalog->getIndexIdent(opCtx, catalogId, "foo"));
    }
}

TEST_F(DurableCatalogImplTest, DirectoryPerDb1) {
    KVEngine* engine = helper->getEngine();

    std::unique_ptr<RecordStore> rs;
    std::unique_ptr<DurableCatalogImpl> catalog;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        ASSERT_OK(engine->createRecordStore(opCtx, "catalog", "catalog", CollectionOptions()));
        rs = engine->getRecordStore(opCtx, "catalog", "catalog", CollectionOptions());
        catalog = std::make_unique<DurableCatalogImpl>(rs.get(), true, false, nullptr);
        uow.commit();
    }

    RecordId catalogId;
    {  // collection
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        catalogId = newCollection(opCtx,
                                  NamespaceString("a.b"),
                                  CollectionOptions(),
                                  KVPrefix::kNotPrefixed,
                                  catalog.get());
        ASSERT_STRING_CONTAINS(catalog->getEntry(catalogId).ident, "a/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getEntry(catalogId).ident));
        uow.commit();
    }

    {  // index
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);

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
        catalog->putMetaData(opCtx, catalogId, md);
        ASSERT_STRING_CONTAINS(catalog->getIndexIdent(opCtx, catalogId, "foo"), "a/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getIndexIdent(opCtx, catalogId, "foo")));
        uow.commit();
    }
}

TEST_F(DurableCatalogImplTest, Split1) {
    KVEngine* engine = helper->getEngine();

    std::unique_ptr<RecordStore> rs;
    std::unique_ptr<DurableCatalogImpl> catalog;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        ASSERT_OK(engine->createRecordStore(opCtx, "catalog", "catalog", CollectionOptions()));
        rs = engine->getRecordStore(opCtx, "catalog", "catalog", CollectionOptions());
        catalog = std::make_unique<DurableCatalogImpl>(rs.get(), false, true, nullptr);
        uow.commit();
    }

    RecordId catalogId;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        catalogId = newCollection(opCtx,
                                  NamespaceString("a.b"),
                                  CollectionOptions(),
                                  KVPrefix::kNotPrefixed,
                                  catalog.get());
        ASSERT_STRING_CONTAINS(catalog->getEntry(catalogId).ident, "collection/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getEntry(catalogId).ident));
        uow.commit();
    }

    {  // index
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);

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
        catalog->putMetaData(opCtx, catalogId, md);
        ASSERT_STRING_CONTAINS(catalog->getIndexIdent(opCtx, catalogId, "foo"), "index/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getIndexIdent(opCtx, catalogId, "foo")));
        uow.commit();
    }
}

TEST_F(DurableCatalogImplTest, DirectoryPerAndSplit1) {
    KVEngine* engine = helper->getEngine();

    std::unique_ptr<RecordStore> rs;
    std::unique_ptr<DurableCatalogImpl> catalog;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        ASSERT_OK(engine->createRecordStore(opCtx, "catalog", "catalog", CollectionOptions()));
        rs = engine->getRecordStore(opCtx, "catalog", "catalog", CollectionOptions());
        catalog = std::make_unique<DurableCatalogImpl>(rs.get(), true, true, nullptr);
        uow.commit();
    }

    RecordId catalogId;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        catalogId = newCollection(opCtx,
                                  NamespaceString("a.b"),
                                  CollectionOptions(),
                                  KVPrefix::kNotPrefixed,
                                  catalog.get());
        ASSERT_STRING_CONTAINS(catalog->getEntry(catalogId).ident, "a/collection/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getEntry(catalogId).ident));
        uow.commit();
    }

    {  // index
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);

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
        catalog->putMetaData(opCtx, catalogId, md);
        ASSERT_STRING_CONTAINS(catalog->getIndexIdent(opCtx, catalogId, "foo"), "a/index/");
        ASSERT_TRUE(catalog->isUserDataIdent(catalog->getIndexIdent(opCtx, catalogId, "foo")));
        uow.commit();
    }
}

TEST_F(DurableCatalogImplTest, BackupImplemented) {
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        ASSERT_OK(engine->beginBackup(opCtx));
        engine->endBackup(opCtx);
    }
}

DEATH_TEST_REGEX_F(DurableCatalogImplTest,
                   TerminateOnNonNumericIndexVersion,
                   "Fatal assertion.*50942") {
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    std::string ident = "abc";
    auto ns = NamespaceString("mydb.mycoll");

    std::unique_ptr<RecordStore> rs;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        ASSERT_OK(engine->createRecordStore(opCtx, "catalog", "catalog", CollectionOptions()));
        rs = engine->getRecordStore(opCtx, "catalog", "catalog", CollectionOptions());
        uow.commit();
    }

    std::unique_ptr<CollectionImpl> collection;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        collection =
            std::make_unique<CollectionImpl>(opCtx, ns, RecordId(0), UUID::gen(), std::move(rs));
        uow.commit();
    }

    IndexDescriptor desc(collection.get(),
                         "",
                         BSON("v"
                              << "1"
                              << "key" << BSON("a" << 1)));
    std::unique_ptr<SortedDataInterface> sorted;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        ASSERT_OK(engine->createSortedDataInterface(opCtx, CollectionOptions(), ident, &desc));
        sorted = engine->getSortedDataInterface(opCtx, ident, &desc);
        ASSERT(sorted);
    }
}

}  // namespace

std::unique_ptr<KVHarnessHelper> KVHarnessHelper::create(ServiceContext* svcCtx) {
    return basicFactory(svcCtx);
};

void KVHarnessHelper::registerFactory(
    std::function<std::unique_ptr<KVHarnessHelper>(ServiceContext*)> factory) {
    basicFactory = std::move(factory);
};

}  // namespace mongo
