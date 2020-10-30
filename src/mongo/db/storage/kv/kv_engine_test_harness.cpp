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

class DurableCatalogImplTest : public unittest::Test {
protected:
    void setUp() override {
        helper = KVHarnessHelper::create();
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
        Lock::DBLock dbLk(opCtx, ns.db(), MODE_IX);
        auto swEntry = catalog->_addEntry(opCtx, ns, options, prefix);
        ASSERT_OK(swEntry.getStatus());
        return swEntry.getValue().catalogId;
    }

    Status dropCollection(OperationContext* opCtx,
                          RecordId catalogId,
                          DurableCatalogImpl* catalog) {
        Lock::GlobalLock globalLk(opCtx, MODE_IX);
        return catalog->_removeEntry(opCtx, catalogId);
    }

    void putMetaData(OperationContext* opCtx,
                     DurableCatalogImpl* catalog,
                     RecordId catalogId,
                     BSONCollectionCatalogEntry::MetaData& md) {
        Lock::GlobalLock globalLk(opCtx, MODE_IX);
        catalog->putMetaData(opCtx, catalogId, md);
    }

    std::string getIndexIdent(OperationContext* opCtx,
                              DurableCatalogImpl* catalog,
                              RecordId catalogId,
                              StringData idxName) {
        Lock::GlobalLock globalLk(opCtx, MODE_IS);
        return catalog->getIndexIdent(opCtx, catalogId, idxName);
    }

    std::unique_ptr<KVHarnessHelper> helper;
};

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


    std::unique_ptr<Collection> collection;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        collection =
            std::make_unique<CollectionImpl>(&opCtx, ns, RecordId(0), UUID::gen(), std::move(rs));
        uow.commit();
    }

    IndexDescriptor desc("",
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
        const RecordId recordId(6, 4);
        const KeyString::Value keyString =
            KeyString::HeapBuilder(
                sorted->getKeyStringVersion(), BSON("" << 5), sorted->getOrdering(), recordId)
                .release();
        ASSERT_OK(sorted->insert(&opCtx, keyString, true));
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
        ASSERT_OK(engine->dropIdent(opCtx.recoveryUnit(), ident));
        wuow.commit();
    }
}

TEST(KVEngineTestHarness, AllDurableTimestamp) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();

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

/*
 * Pinned oldest with another session
 * | Session 1                   | Session 2                  |
 * |-----------------------------+----------------------------|
 * | Begin                       |                            |
 * | Write A 1                   |                            |
 * | Commit :commit 10           |                            |
 * | Begin :readAt 15            |                            |
 * |                             | Begin                      |
 * |                             | Write A 2                  |
 * | Read A (1)                  |                            |
 * |                             | Commit :commit 20          |
 * | Read A (1)                  |                            |
 * |                             | Begin :readAt 15           |
 * |                             | Read A (1)                 |
 * | Rollback                    |                            |
 * |                             | Rollback                   |
 */
TEST(KVEngineTestHarness, PinningOldestWithAnotherSession) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    // TODO SERVER-48314: Remove after implementing correct behavior on biggie.
    if (engine->isEphemeral())
        return;

    std::string ns = "a.b";
    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
        ASSERT(rs);
    }

    MyOperationContext opCtx1(engine);
    WriteUnitOfWork uow1(&opCtx1);
    StatusWith<RecordId> res = rs->insertRecord(&opCtx1, "abc", 4, Timestamp(10, 10));
    RecordId rid = res.getValue();
    uow1.commit();

    RecordData rd;
    opCtx1.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                  Timestamp(15, 15));

    MyOperationContext opCtx2(engine);
    WriteUnitOfWork uow2(&opCtx2);

    ASSERT(rs->findRecord(&opCtx1, rid, &rd));
    ASSERT_OK(opCtx2.recoveryUnit()->setTimestamp(Timestamp(20, 20)));
    ASSERT_OK(rs->updateRecord(&opCtx2, rid, "updated", 8));

    ASSERT(rs->findRecord(&opCtx1, rid, &rd));
    ASSERT_EQUALS(std::string("abc"), rd.data());

    uow2.commit();

    opCtx1.recoveryUnit()->abandonSnapshot();
    ASSERT(rs->findRecord(&opCtx1, rid, &rd));
    ASSERT_EQUALS(std::string("abc"), rd.data());


    opCtx2.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                  Timestamp(15, 15));
    ASSERT(rs->findRecord(&opCtx2, rid, &rd));
    ASSERT_EQUALS(std::string("abc"), rd.data());
}

/*
 * All durable
 * | Session 1            | Session 2            | GlobalActor                      |
 * |----------------------+----------------------+----------------------------------|
 * | Begin                |                      |                                  |
 * | Commit :commit 10    |                      |                                  |
 * |                      |                      | QueryTimestamp :all_durable (10) |
 * | Begin                |                      |                                  |
 * | Timestamp :commit 20 |                      |                                  |
 * |                      |                      | QueryTimestamp :all_durable (19) |
 * |                      | Begin                |                                  |
 * |                      | Timestamp :commit 30 |                                  |
 * |                      | Commit               |                                  |
 * |                      |                      | QueryTimestamp :all_durable (19) |
 * | Commit               |                      |                                  |
 * |                      |                      | QueryTimestamp :all_durable (30) |
 * | Begin                |                      |                                  |
 * | Timestamp :commit 25 |                      |                                  |
 * |                      |                      | QueryTimestamp :all_durable (30) |
 */
TEST(KVEngineTestHarness, AllDurable) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();

    std::string ns = "a.b";
    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
        ASSERT(rs);
    }

    {
        const Timestamp kInsertTimestamp1 = Timestamp(10, 10);
        const Timestamp kInsertTimestamp2 = Timestamp(20, 20);
        const Timestamp kInsertTimestamp3 = Timestamp(30, 30);
        const Timestamp kInsertTimestamp4 = Timestamp(25, 25);

        Timestamp allDurable = engine->getAllDurableTimestamp();
        MyOperationContext opCtx1(engine);
        WriteUnitOfWork uow1(&opCtx1);
        auto swRid = rs->insertRecord(&opCtx1, "abc", 4, kInsertTimestamp1);
        ASSERT_OK(swRid);
        uow1.commit();

        Timestamp lastAllDurable = allDurable;
        allDurable = engine->getAllDurableTimestamp();
        ASSERT_GTE(allDurable, lastAllDurable);
        ASSERT_LTE(allDurable, kInsertTimestamp1);

        MyOperationContext opCtx2(engine);
        WriteUnitOfWork uow2(&opCtx2);
        swRid = rs->insertRecord(&opCtx2, "abc", 4, kInsertTimestamp2);
        ASSERT_OK(swRid);

        lastAllDurable = allDurable;
        allDurable = engine->getAllDurableTimestamp();
        ASSERT_GTE(allDurable, lastAllDurable);
        ASSERT_LT(allDurable, kInsertTimestamp2);

        MyOperationContext opCtx3(engine);
        WriteUnitOfWork uow3(&opCtx3);
        swRid = rs->insertRecord(&opCtx3, "abc", 4, kInsertTimestamp3);
        ASSERT_OK(swRid);
        uow3.commit();

        lastAllDurable = allDurable;
        allDurable = engine->getAllDurableTimestamp();
        ASSERT_GTE(allDurable, lastAllDurable);
        ASSERT_LT(allDurable, kInsertTimestamp2);

        uow2.commit();

        lastAllDurable = allDurable;
        allDurable = engine->getAllDurableTimestamp();
        ASSERT_GTE(allDurable, lastAllDurable);
        ASSERT_LTE(allDurable, kInsertTimestamp3);

        MyOperationContext opCtx4(engine);
        WriteUnitOfWork uow4(&opCtx4);
        swRid = rs->insertRecord(&opCtx4, "abc", 4, kInsertTimestamp4);
        ASSERT_OK(swRid);

        lastAllDurable = allDurable;
        allDurable = engine->getAllDurableTimestamp();
        ASSERT_GTE(allDurable, lastAllDurable);
        ASSERT_LTE(allDurable, kInsertTimestamp3);
        uow4.commit();
    }
}

/*
 * Basic Timestamp - Single
 * | Session              |
 * |----------------------|
 * | Begin                |
 * | Write A 1            |
 * | Commit :commit 10    |
 * |                      |
 * | Begin :readAt 9      |
 * | Read A (NOT_FOUND)   |
 * | Rollback             |
 * |                      |
 * | Begin :readAt 10     |
 * | Read A (1)           |
 */
TEST(KVEngineTestHarness, BasicTimestampSingle) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    // TODO SERVER-48314: Remove after implementing correct behavior on biggie.
    if (engine->isEphemeral())
        return;

    std::string ns = "a.b";
    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
        ASSERT(rs);
    }

    const Timestamp kReadTimestamp = Timestamp(9, 9);
    const Timestamp kInsertTimestamp = Timestamp(10, 10);

    // Start a read transaction.
    MyOperationContext opCtx1(engine);

    opCtx1.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                  kReadTimestamp);
    ASSERT(!rs->findRecord(&opCtx1, RecordId::min(), nullptr));

    // Insert a record at a later time.
    RecordId rid;
    {
        MyOperationContext opCtx2(engine);
        WriteUnitOfWork wuow(&opCtx2);
        auto swRid = rs->insertRecord(&opCtx2, "abc", 4, kInsertTimestamp);
        ASSERT_OK(swRid);
        rid = swRid.getValue();
        wuow.commit();
    }

    // Should not see the record, even if we abandon the snapshot as the read timestamp is still
    // earlier than the insert timestamp.
    ASSERT(!rs->findRecord(&opCtx1, rid, nullptr));
    opCtx1.recoveryUnit()->abandonSnapshot();
    ASSERT(!rs->findRecord(&opCtx1, rid, nullptr));


    opCtx1.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                  kInsertTimestamp);
    opCtx1.recoveryUnit()->abandonSnapshot();
    RecordData rd;
    ASSERT(rs->findRecord(&opCtx1, rid, &rd));
    ASSERT_EQ(std::string("abc"), rd.data());
}

/*
 * Basic Timestamp - Multiple
 * | Session              |
 * |----------------------|
 * | Begin                |
 * | Timestamp :commit 10 |
 * | Write A 1            |
 * | Timestamp :commit 20 |
 * | Update A 2           |
 * | Commit               |
 * |                      |
 * | Begin :readAt 10     |
 * | Read A (1)           |
 * | Rollback             |
 * |                      |
 * | Begin  :readAt 20    |
 * | Read A (2)           |
 */
TEST(KVEngineTestHarness, BasicTimestampMultiple) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    // TODO SERVER-48314: Remove after implementing correct behavior on biggie.
    if (engine->isEphemeral())
        return;

    std::string ns = "a.b";
    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
        ASSERT(rs);
    }

    const Timestamp t10 = Timestamp(10, 10);
    const Timestamp t20 = Timestamp(20, 20);

    RecordId rid;
    {
        // Initial insert of record.
        MyOperationContext opCtx(engine);
        WriteUnitOfWork wuow(&opCtx);
        auto swRid = rs->insertRecord(&opCtx, "abc", 4, t10);
        ASSERT_OK(swRid);
        rid = swRid.getValue();

        // Update a record at a later time.
        ASSERT_OK(opCtx.recoveryUnit()->setTimestamp(t20));
        auto res = rs->updateRecord(&opCtx, rid, "updated", 8);
        ASSERT_OK(res);
        wuow.commit();
    }

    RecordData rd;
    MyOperationContext opCtx(engine);
    opCtx.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, t10);
    ASSERT(rs->findRecord(&opCtx, rid, &rd));
    ASSERT_EQUALS(std::string("abc"), rd.data());

    opCtx.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, t20);
    opCtx.recoveryUnit()->abandonSnapshot();
    ASSERT(rs->findRecord(&opCtx, rid, &rd));
    ASSERT_EQUALS(std::string("updated"), rd.data());
}

/*
 * Concurrent operations under snapshot isolation blocks visibility
 * | Session 1         | Session 2                            |
 * |-------------------+--------------------------------------|
 * | Begin             |                                      |
 * |                   | Begin :readAt 20 :isolation snapshot |
 * | Write A 1         |                                      |
 * | Commit :commit 10 |                                      |
 * |                   | Read A (NOT_FOUND)                   |
 * |                   | Abandon Snapshot                     |
 * |                   | Read A (1)                           |
 */
TEST(KVEngineTestHarness, SingleReadWithConflict) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    // TODO SERVER-48314: Remove after implementing correct behavior on biggie.
    if (engine->isEphemeral())
        return;

    std::string ns = "a.b";
    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
        ASSERT(rs);
    }

    MyOperationContext opCtx2(engine);
    opCtx2.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                  Timestamp(20, 20));

    MyOperationContext opCtx1(engine);
    WriteUnitOfWork uow1(&opCtx1);
    StatusWith<RecordId> res = rs->insertRecord(&opCtx1, "abc", 4, Timestamp(10, 10));
    ASSERT_OK(res);
    RecordId loc = res.getValue();

    // Cannot find record before commit.
    RecordData rd;
    ASSERT(!rs->findRecord(&opCtx2, loc, &rd));

    // Cannot find record after commit due to snapshot isolation.
    uow1.commit();
    ASSERT(!rs->findRecord(&opCtx2, loc, &rd));

    // Abandon snapshot for visibility.
    opCtx2.recoveryUnit()->abandonSnapshot();

    ASSERT(rs->findRecord(&opCtx2, loc, &rd));
    ASSERT_EQUALS(std::string("abc"), rs->dataFor(&opCtx2, loc).data());
}

/*
 * Item Not Found - Read timestamp hides visibility
 * | Session              |
 * |----------------------|
 * | Begin                |
 * | Write A 1            |
 * | Commit :commit 10    |
 * |                      |
 * | Begin :readAt 9      |
 * | Read A (NOT_FOUND)   |
 * | Write A 1 (NOT_FOUND)|
 */
DEATH_TEST_REGEX(KVEngineTestHarness, SnapshotHidesVisibility, ".*item not found.*") {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    // TODO: Remove after implementing correct behavior on biggie.
    if (engine->isEphemeral())
        invariant(false, "item not found");

    std::string ns = "a.b";
    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
        ASSERT(rs);
    }

    MyOperationContext opCtx1(engine);
    WriteUnitOfWork uow1(&opCtx1);
    StatusWith<RecordId> res = rs->insertRecord(&opCtx1, "abc", 4, Timestamp(10, 10));
    ASSERT_OK(res);
    RecordId loc = res.getValue();
    uow1.commit();

    // Snapshot was taken before the insert and will not find the record even after the commit.
    RecordData rd;
    MyOperationContext opCtx2(engine);
    opCtx2.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                  Timestamp(9, 9));
    ASSERT(!rs->findRecord(&opCtx2, loc, &rd));

    // Trying to write in an outdated snapshot will cause item not found.
    WriteUnitOfWork uow2(&opCtx2);
    auto swRid = rs->updateRecord(&opCtx2, loc, "updated", 8);
    uow2.commit();
}

/*
 * Insert
 * | Session                |
 * |------------------------|
 * | Begin                  |
 * | Write A 1              |
 * | Timestamp :commit 10   |
 * | Write Oplog            |
 * | Commit                 |
 * |                        |
 * | Begin :readAt 9        |
 * | Read A (NOT_FOUND)     |
 * | Read Oplog (NOT_FOUND) |
 * | Rollback               |
 * |                        |
 * | Begin :readAt 10       |
 * | Read A (1)             |
 * | Read Oplog (FOUND)     |
 */
TEST(KVEngineTestHarness, SingleReadWithConflictWithOplog) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    // TODO SERVER-48314: Remove after implementing correct behavior on biggie.
    if (engine->isEphemeral())
        return;

    std::string ns = "a.b";
    std::unique_ptr<RecordStore> collectionRs;
    std::unique_ptr<RecordStore> oplogRs;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
        collectionRs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
        ASSERT(collectionRs);

        CollectionOptions options;
        options.capped = true;
        options.cappedSize = 10240;
        options.cappedMaxDocs = -1;

        NamespaceString oplogNss("local.oplog.rs");
        ASSERT_OK(engine->createRecordStore(&opCtx, oplogNss.ns(), "ident", options));
        oplogRs = engine->getRecordStore(&opCtx, oplogNss.ns(), "ident", options);
        ASSERT(oplogRs);
    }

    RecordData rd;
    RecordId locCollection;
    RecordId locOplog;
    const Timestamp t9(9, 9);
    const Timestamp t10(10, 10);
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);

        // Insert into collectionRs.
        StatusWith<RecordId> res = collectionRs->insertRecord(&opCtx, "abc", 4, t10);
        ASSERT_OK(res);
        locCollection = res.getValue();

        // Insert into oplogRs.
        auto t11Doc = BSON("ts" << t10);

        ASSERT_EQ(invariant(oplogRs->insertRecord(
                      &opCtx, t11Doc.objdata(), t11Doc.objsize(), Timestamp::min())),
                  RecordId(10, 10));
        locOplog = RecordId(10, 10);
        uow.commit();
    }

    MyOperationContext opCtx(engine);
    opCtx.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, t9);
    ASSERT(!collectionRs->findRecord(&opCtx, locCollection, &rd));
    ASSERT(!oplogRs->findRecord(&opCtx, locOplog, &rd));

    opCtx.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, t10);
    opCtx.recoveryUnit()->abandonSnapshot();
    ASSERT(collectionRs->findRecord(&opCtx, locCollection, &rd));
    ASSERT(oplogRs->findRecord(&opCtx, locOplog, &rd));
}

/*
 * Pinned oldest timestamp - Read
 * | Session                     | GlobalActor                |
 * |-----------------------------+----------------------------|
 * | Begin                       |                            |
 * | Write A 1                   |                            |
 * | Commit :commit 10           |                            |
 * |                             |                            |
 * | Begin :readAt 15            |                            |
 * | Read A (1)                  |                            |
 * | Rollback                    |                            |
 * |                             | GlobalTimestamp :oldest 20 |
 * | Begin :readAt 15            |                            |
 * | Read A (DB exception)       |                            |
 */
TEST(KVEngineTestHarness, PinningOldestTimestampWithReadConflict) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    // TODO SERVER-48314: Remove after implementing correct behavior on biggie.
    if (engine->isEphemeral())
        return;

    std::string ns = "a.b";
    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
        ASSERT(rs);
    }

    MyOperationContext opCtx(engine);
    WriteUnitOfWork uow(&opCtx);
    StatusWith<RecordId> res = rs->insertRecord(&opCtx, "abc", 4, Timestamp(10, 10));
    RecordId rid = res.getValue();
    uow.commit();

    RecordData rd;
    opCtx.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                 Timestamp(15, 15));
    ASSERT(rs->findRecord(&opCtx, rid, &rd));

    engine->setOldestTimestamp(Timestamp(20, 20), false);

    opCtx.recoveryUnit()->abandonSnapshot();
    ASSERT_THROWS_CODE(rs->findRecord(&opCtx, rid, &rd), DBException, ErrorCodes::SnapshotTooOld);
}


/*
 * Pinned oldest timestamp - Write
 * | Session                     | GlobalActor                |
 * |-----------------------------+----------------------------|
 * |                             | GlobalTimestamp :oldest 2  |
 * | Begin                       |                            |
 * | Write A 1                   |                            |
 * | Commit :commit 2 (WCE)      |                            |
 */
DEATH_TEST_REGEX(KVEngineTestHarness,
                 PinningOldestTimestampWithWriteConflict,
                 ".*commit timestamp.*is less than the oldest timestamp.*") {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    // TODO SERVER-48314: Remove after implementing correct behavior on biggie.
    if (engine->isEphemeral())
        invariant(false, "commit timestamp is less than the oldest timestamp");

    std::string ns = "a.b";
    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
        ASSERT(rs);
    }

    {
        // A write transaction cannot insert records before the oldest timestamp.
        engine->setOldestTimestamp(Timestamp(2, 2), false);
        MyOperationContext opCtx2(engine);
        WriteUnitOfWork uow2(&opCtx2);
        StatusWith<RecordId> res = rs->insertRecord(&opCtx2, "abc", 4, Timestamp(1, 1));
        uow2.commit();
    }
}

/*
 * Rolling Back To Last Stable
 * | Session                     | GlobalActor                |
 * |-----------------------------+----------------------------|
 * | Begin                       |                            |
 * | Write A 1                   |                            |
 * | Timestamp: commit 1         |                            |
 * |                             | Last Stable Timetamp: 1    |
 * | Begin                       |                            |
 * | Write B 2                   |                            |
 * | Timestamp: commit 3         |                            |
 * |                             | Recover to Last Stable     |
 * | Read A (1)                  |                            |
 * | Read B (NOT_FOUND)          |                            |
 */
TEST(KVEngineTestHarness, RollingBackToLastStable) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    // TODO SERVER-48314: Remove after implementing correct behavior on biggie.
    if (engine->isEphemeral())
        return;

    // The initial data timestamp has to be set to take stable checkpoints.
    engine->setInitialDataTimestamp(Timestamp(1, 1));
    std::string ns = "a.b";
    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
        ASSERT(rs);
    }

    RecordId ridA;
    {
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        auto res = rs->insertRecord(&opCtx, "abc", 4, Timestamp(1, 1));
        ASSERT_OK(res);
        ridA = res.getValue();
        uow.commit();
        ASSERT_EQUALS(1, rs->numRecords(&opCtx));
    }

    {
        // Set the stable timestamp to (1, 1) as it can't be set higher than the all durable
        // timestamp, which is (1, 1) in this case.
        ASSERT(!engine->getLastStableRecoveryTimestamp());
        ASSERT_EQUALS(engine->getAllDurableTimestamp(), Timestamp(1, 1));
        engine->setStableTimestamp(Timestamp(1, 1), false);
        ASSERT(!engine->getLastStableRecoveryTimestamp());

        // Force a checkpoint to be taken. This should advance the last stable timestamp.
        MyOperationContext opCtx(engine);
        engine->flushAllFiles(&opCtx, false);
        ASSERT_EQ(engine->getLastStableRecoveryTimestamp(), Timestamp(1, 1));
    }

    RecordId ridB;
    {
        // Insert a record after the stable timestamp.
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> swRid = rs->insertRecord(&opCtx, "def", 4, Timestamp(3, 3));
        ASSERT_OK(swRid);
        ridB = swRid.getValue();
        ASSERT_EQUALS(2, rs->numRecords(&opCtx));
        uow.commit();
    }

    {
        // Rollback to the last stable timestamp.
        MyOperationContext opCtx(engine);
        StatusWith<Timestamp> swTimestamp = engine->recoverToStableTimestamp(&opCtx);
        ASSERT_EQ(swTimestamp.getValue(), Timestamp(1, 1));

        // Verify that we can find record A and can't find the record B inserted at Timestamp(3, 3)
        // in the collection any longer. 'numRecords' will still show two as it's the fast count and
        // doesn't get reflected during the rollback.
        RecordData rd;
        opCtx.recoveryUnit()->abandonSnapshot();
        ASSERT(rs->findRecord(&opCtx, ridA, &rd));
        ASSERT_EQ(std::string("abc"), rd.data());
        ASSERT_FALSE(rs->findRecord(&opCtx, ridB, nullptr));
        ASSERT_EQUALS(2, rs->numRecords(&opCtx));
    }
}

/*
 * Commit behind stable
 * | Session                         | GlobalActor                |
 * |---------------------------------+----------------------------|
 * |                                 | GlobalTimestamp :stable 2  |
 * | Begin                           |                            |
 * | Write A 1                       |                            |
 * | Timestamp :commit 1  (ROLLBACK) |                            |
 */
DEATH_TEST_REGEX(KVEngineTestHarness,
                 CommitBehindStable,
                 ".*commit timestamp.*is less than the stable timestamp.*") {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    // TODO SERVER-48314: Remove after implementing correct behavior on biggie.
    if (engine->isEphemeral())
        invariant(false, "commit timestamp is less than the stable timestamp");

    // The initial data timestamp has to be set to take stable checkpoints.
    engine->setInitialDataTimestamp(Timestamp(1, 1));
    std::string ns = "a.b";
    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
        ASSERT(rs);
    }

    {
        // Set the stable timestamp to (2, 2).
        ASSERT(!engine->getLastStableRecoveryTimestamp());
        engine->setStableTimestamp(Timestamp(2, 2), false);
        ASSERT(!engine->getLastStableRecoveryTimestamp());

        // Force a checkpoint to be taken. This should advance the last stable timestamp.
        MyOperationContext opCtx(engine);
        engine->flushAllFiles(&opCtx, false);
        ASSERT_EQ(engine->getLastStableRecoveryTimestamp(), Timestamp(2, 2));
    }

    {
        // Committing a behind the stable timestamp is not allowed.
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        auto swRid = rs->insertRecord(&opCtx, "abc", 4, Timestamp(1, 1));
        uow.commit();
    }
}

/*
 * Commit at stable
 * | Session                         | GlobalActor                |
 * |---------------------------------+----------------------------|
 * |                                 | GlobalTimestamp :stable 2  |
 * | Begin                           |                            |
 * | Write A 1                       |                            |
 * | Timestamp :commit 2             |                            |
 */
TEST(KVEngineTestHarness, CommitAtStable) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create());
    KVEngine* engine = helper->getEngine();
    // TODO SERVER-48314: Remove after implementing correct behavior on biggie.
    if (engine->isEphemeral())
        return;

    // The initial data timestamp has to be set to take stable checkpoints.
    engine->setInitialDataTimestamp(Timestamp(1, 1));
    std::string ns = "a.b";
    std::unique_ptr<RecordStore> rs;
    {
        MyOperationContext opCtx(engine);
        ASSERT_OK(engine->createRecordStore(&opCtx, ns, ns, CollectionOptions()));
        rs = engine->getRecordStore(&opCtx, ns, ns, CollectionOptions());
        ASSERT(rs);
    }

    {
        // Set the stable timestamp to (2, 2).
        ASSERT(!engine->getLastStableRecoveryTimestamp());
        engine->setStableTimestamp(Timestamp(2, 2), false);
        ASSERT(!engine->getLastStableRecoveryTimestamp());

        // Force a checkpoint to be taken. This should advance the last stable timestamp.
        MyOperationContext opCtx(engine);
        engine->flushAllFiles(&opCtx, false);
        ASSERT_EQ(engine->getLastStableRecoveryTimestamp(), Timestamp(2, 2));
    }

    RecordId rid;
    {
        // For a non-prepared transaction, the commit timestamp can be equal to the stable
        // timestamp.
        MyOperationContext opCtx(engine);
        WriteUnitOfWork uow(&opCtx);
        auto swRid = rs->insertRecord(&opCtx, "abc", 4, Timestamp(2, 2));
        ASSERT_OK(swRid);
        rid = swRid.getValue();
        uow.commit();
    }

    {
        // Rollback to the last stable timestamp.
        MyOperationContext opCtx(engine);
        StatusWith<Timestamp> swTimestamp = engine->recoverToStableTimestamp(&opCtx);
        ASSERT_EQ(swTimestamp.getValue(), Timestamp(2, 2));

        // Transaction with timestamps equal to lastStable will not be rolled back.
        opCtx.recoveryUnit()->abandonSnapshot();
        RecordData data;
        ASSERT_TRUE(rs->findRecord(&opCtx, rid, &data));
        ASSERT_EQUALS(1, rs->numRecords(&opCtx));
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
        Lock::GlobalLock globalLk(opCtx, MODE_IX);

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
        putMetaData(opCtx, catalog.get(), catalogId, md);
        uow.commit();
    }

    std::string idxIndent;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        idxIndent = getIndexIdent(opCtx, catalog.get(), catalogId, "foo");
    }

    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        ASSERT_EQUALS(idxIndent, getIndexIdent(opCtx, catalog.get(), catalogId, "foo"));
        ASSERT_TRUE(
            catalog->isUserDataIdent(getIndexIdent(opCtx, catalog.get(), catalogId, "foo")));
    }

    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);

        BSONCollectionCatalogEntry::MetaData md;
        md.ns = "a.b";
        putMetaData(opCtx, catalog.get(), catalogId, md);  // remove index

        BSONCollectionCatalogEntry::IndexMetaData imd;
        imd.spec = BSON("name"
                        << "foo");
        imd.ready = false;
        imd.multikey = false;
        imd.prefix = KVPrefix::kNotPrefixed;
        imd.isBackgroundSecondaryBuild = false;
        md.indexes.push_back(imd);
        putMetaData(opCtx, catalog.get(), catalogId, md);
        uow.commit();
    }

    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        ASSERT_NOT_EQUALS(idxIndent, getIndexIdent(opCtx, catalog.get(), catalogId, "foo"));
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
        putMetaData(opCtx, catalog.get(), catalogId, md);
        ASSERT_STRING_CONTAINS(getIndexIdent(opCtx, catalog.get(), catalogId, "foo"), "a/");
        ASSERT_TRUE(
            catalog->isUserDataIdent(getIndexIdent(opCtx, catalog.get(), catalogId, "foo")));
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
        putMetaData(opCtx, catalog.get(), catalogId, md);
        ASSERT_STRING_CONTAINS(getIndexIdent(opCtx, catalog.get(), catalogId, "foo"), "index/");
        ASSERT_TRUE(
            catalog->isUserDataIdent(getIndexIdent(opCtx, catalog.get(), catalogId, "foo")));
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
        putMetaData(opCtx, catalog.get(), catalogId, md);
        ASSERT_STRING_CONTAINS(getIndexIdent(opCtx, catalog.get(), catalogId, "foo"), "a/index/");
        ASSERT_TRUE(
            catalog->isUserDataIdent(getIndexIdent(opCtx, catalog.get(), catalogId, "foo")));
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

    IndexDescriptor desc("",
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

std::unique_ptr<KVHarnessHelper> KVHarnessHelper::create() {
    return basicFactory();
};

void KVHarnessHelper::registerFactory(std::function<std::unique_ptr<KVHarnessHelper>()> factory) {
    basicFactory = std::move(factory);
};

}  // namespace mongo
