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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/local_catalog/durable_catalog_entry_metadata.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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

class MDBCatalogTest : public ServiceContextTest {
protected:
    void setUp() override {
        helper = KVHarnessHelper::create(getServiceContext());
        invariant(hasGlobalServiceContext());
    }

    ClientAndCtx makeClientAndCtx(const std::string& clientName) {
        auto client = getGlobalServiceContext()->getService()->makeClient(clientName);
        auto opCtx = client->makeOperationContext();
        shard_role_details::setRecoveryUnit(opCtx.get(),
                                            helper->getEngine()->newRecoveryUnit(),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        return {std::move(client), std::move(opCtx)};
    }

    // Callers are responsible for managing the lifetime of 'catalogRS'.
    std::unique_ptr<MDBCatalog> createMDBCatalog(RecordStore* catalogRS) {
        return std::make_unique<MDBCatalog>(catalogRS, helper->getEngine());
    }

    std::unique_ptr<RecordStore> createCatalogRS() {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        KVEngine* engine = helper->getEngine();
        auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
        ASSERT_OK(
            engine->createRecordStore(provider,
                                      NamespaceString::createNamespaceString_forTest("catalog"),
                                      "collection-catalog",
                                      RecordStore::Options{}));

        return engine->getRecordStore(opCtx,
                                      NamespaceString::createNamespaceString_forTest("catalog"),
                                      "collection-catalog",
                                      RecordStore::Options{},
                                      UUID::gen());
    }

    // Adds a new entry to the MDBCatalog without initializing a backing RecordStore.
    RecordId addNewCatalogEntry(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const CollectionOptions& collectionOptions,
                                MDBCatalog* mdbCatalog) {
        Lock::DBLock dbLk(opCtx, nss.dbName(), MODE_IX);

        const auto ident = ident::generateNewCollectionIdent(nss.dbName(), false, false);
        auto md = durable_catalog::internal::createMetaDataForNewCollection(nss, collectionOptions);
        auto rawMDBCatalogEntry = durable_catalog::internal::buildRawMDBCatalogEntry(
            ident, BSONObj() /* idxIdent */, md, NamespaceStringUtil::serializeForCatalog(nss));

        // An 'orphaned' entry is one without a RecordStore explicitly created to back it.
        auto swEntry = mdbCatalog->addEntry(
            opCtx, ident, nss, rawMDBCatalogEntry, mdbCatalog->reserveCatalogId(opCtx));
        ASSERT_OK(swEntry.getStatus());
        return swEntry.getValue().catalogId;
    }

    StatusWith<MDBCatalog::EntryIdentifier> addEntry(OperationContext* opCtx,
                                                     const std::string& ident,
                                                     const NamespaceString& nss,
                                                     const BSONObj& catalogEntryObj,
                                                     const RecordId& catalogId,
                                                     MDBCatalog* mdbCatalog) {
        Lock::DBLock dbLk(opCtx, nss.dbName(), MODE_IX);
        return mdbCatalog->addEntry(opCtx, ident, nss, catalogEntryObj, catalogId);
    }

    Status dropCollection(OperationContext* opCtx, RecordId catalogId, MDBCatalog* catalog) {
        Lock::GlobalLock globalLk(opCtx, MODE_IX);
        return catalog->removeEntry(opCtx, catalogId);
    }

    void putMetaData(OperationContext* opCtx,
                     MDBCatalog* mdbCatalog,
                     RecordId catalogId,
                     durable_catalog::CatalogEntryMetaData& md,
                     boost::optional<BSONObj> indexIdents = boost::none) {
        Lock::GlobalLock globalLk(opCtx, MODE_IX);
        durable_catalog::putMetaData(opCtx, catalogId, md, mdbCatalog, indexIdents);
    }

    std::string getIndexIdent(OperationContext* opCtx,
                              MDBCatalog* catalog,
                              RecordId catalogId,
                              StringData idxName) {
        Lock::GlobalLock globalLk(opCtx, MODE_IS);
        return catalog->getIndexIdent(opCtx, catalogId, idxName);
    }

    std::unique_ptr<KVHarnessHelper> helper;
};

namespace {

std::function<std::unique_ptr<KVHarnessHelper>(ServiceContext*)> basicFactory =
    [](ServiceContext*) -> std::unique_ptr<KVHarnessHelper> {
    fassertFailed(40355);
};

class KVEngineTestHarness : public ServiceContextTest {
protected:
    const NamespaceString kNss =
        NamespaceString::createNamespaceString_forTest("defaultDB.defaultColl");
    const std::string kIdent = "collection-defaultIdent";
    const UUID kUUID = UUID::gen();
    const RecordStore::Options kRecordStoreOptions{};

    ServiceContext::UniqueOperationContext _makeOperationContext(KVEngine* engine) {
        auto opCtx = makeOperationContext();
        shard_role_details::setRecoveryUnit(opCtx.get(),
                                            engine->newRecoveryUnit(),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        return opCtx;
    }

    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
    _makeOperationContexts(KVEngine* engine, unsigned num) {
        std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
            opCtxs;
        opCtxs.reserve(num);

        for (unsigned i = 0; i < num; ++i) {
            auto client = getServiceContext()->getService()->makeClient(std::to_string(i));

            auto opCtx = client->makeOperationContext();
            shard_role_details::setRecoveryUnit(
                opCtx.get(),
                engine->newRecoveryUnit(),
                WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
            opCtxs.emplace_back(std::move(client), std::move(opCtx));
        }

        return opCtxs;
    }

    std::unique_ptr<RecordStore> newRecordStore(KVEngine* engine,
                                                const NamespaceString& nss,
                                                StringData ident,
                                                const RecordStore::Options& recordStoreOptions,
                                                boost::optional<UUID> uuid) {
        auto opCtx = _makeOperationContext(engine);
        auto& provider = rss::ReplicatedStorageService::get(opCtx.get()).getPersistenceProvider();
        ASSERT_OK(engine->createRecordStore(provider, nss, ident, recordStoreOptions));
        auto rs = engine->getRecordStore(opCtx.get(), nss, ident, recordStoreOptions, uuid);
        ASSERT(rs);
        return rs;
    }

    std::unique_ptr<RecordStore> newRecordStore(KVEngine* engine) {
        return newRecordStore(engine, kNss, kIdent, kRecordStoreOptions, kUUID);
    }
};

TEST_F(KVEngineTestHarness, SimpleRS1) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    std::unique_ptr<RecordStore> rs = newRecordStore(engine);
    const auto& ident = rs->getIdent();
    RecordId loc;
    {
        auto opCtx = _makeOperationContext(engine);
        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> res = rs->insertRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), "abc", 4, Timestamp());
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        uow.commit();
    }

    {
        auto opCtx = _makeOperationContext(engine);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(
            std::string("abc"),
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .data());
    }

    {
        auto opCtx = _makeOperationContext(engine);
        std::vector<std::string> all =
            engine->getAllIdents(*shard_role_details::getRecoveryUnit(opCtx.get()));

        // This includes the _mdb_catalog.
        ASSERT_EQUALS(2U, all.size());
        ASSERT_EQUALS(ident, all[1]);
    }
}

TEST_F(KVEngineTestHarness, Restart1) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    // 'loc' holds location of "abc" and is referenced after restarting engine.
    RecordId loc;
    {
        // Using default RecordStore parameters before and after restart.
        std::unique_ptr<RecordStore> rs =
            newRecordStore(engine, kNss, kIdent, kRecordStoreOptions, kUUID);

        {
            auto opCtx = _makeOperationContext(engine);
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 "abc",
                                 4,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }

        {
            auto opCtx = _makeOperationContext(engine);
            Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
            ASSERT_EQUALS(
                std::string("abc"),
                rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                    .data());
        }
    }

    engine = helper->restartEngine();

    {
        std::unique_ptr<RecordStore> rs;
        auto opCtx = _makeOperationContext(engine);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        rs = engine->getRecordStore(opCtx.get(), kNss, kIdent, kRecordStoreOptions, kUUID);
        ASSERT_EQUALS(
            std::string("abc"),
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .data());
    }
}

TEST_F(KVEngineTestHarness, SimpleSorted1) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);
    std::string indexName = "name";
    auto spec = BSON("v" << static_cast<int>(IndexConfig::kLatestIndexVersion) << "key"
                         << BSON("a" << 1) << "name" << indexName);
    auto ordering = Ordering::allAscending();
    IndexConfig config{false /* isIdIndex */,
                       false /* unique */,
                       IndexConfig::kLatestIndexVersion,
                       spec,
                       indexName,
                       ordering};
    std::unique_ptr<SortedDataInterface> sorted;
    {
        auto opCtx = _makeOperationContext(engine);
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        auto& provider = rss::ReplicatedStorageService::get(opCtx.get()).getPersistenceProvider();
        ASSERT_OK(engine->createSortedDataInterface(provider,
                                                    ru,
                                                    kNss,
                                                    kUUID,
                                                    kIdent,
                                                    config,
                                                    boost::none /* storageEngineIndexOptions */));
        sorted = engine->getSortedDataInterface(
            opCtx.get(), ru, kNss, kUUID, kIdent, config, kRecordStoreOptions.keyFormat);
        ASSERT(sorted);
    }

    {
        auto opCtx = _makeOperationContext(engine);
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_X);
        WriteUnitOfWork uow(opCtx.get());
        const RecordId recordId(6, 4);
        const key_string::Value keyString =
            key_string::HeapBuilder(
                sorted->getKeyStringVersion(), BSON("" << 5), sorted->getOrdering(), recordId)
                .release();
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx.get(), ru, keyString, true));
        uow.commit();
    }

    {
        auto opCtx = _makeOperationContext(engine);
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get(), ru));
    }
}

TEST_F(KVEngineTestHarness, TemporaryRecordStoreSimple) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);

    std::string ident = "collection-temptemp";
    std::unique_ptr<RecordStore> rs;
    {
        auto opCtx = _makeOperationContext(engine);
        rs = engine->makeTemporaryRecordStore(
            *shard_role_details::getRecoveryUnit(opCtx.get()), ident, KeyFormat::Long);
        ASSERT(rs);
    }

    RecordId loc;
    {
        auto opCtx = _makeOperationContext(engine);
        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> res = rs->insertRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), "abc", 4, Timestamp());
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        uow.commit();
    }

    {
        auto opCtx = _makeOperationContext(engine);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(
            std::string("abc"),
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .data());

        std::vector<std::string> all =
            engine->getAllIdents(*shard_role_details::getRecoveryUnit(opCtx.get()));

        // This includes the _mdb_catalog.
        ASSERT_EQUALS(2U, all.size());
        ASSERT_EQUALS(ident, all[1]);

        // Dropping a collection might fail if we haven't checkpointed the data
        engine->checkpoint();

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(engine->dropIdent(
            *shard_role_details::getRecoveryUnit(opCtx.get()), ident, /*identHasSizeInfo=*/true));
        wuow.commit();
    }
}

TEST_F(KVEngineTestHarness, AllDurableTimestamp) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    std::unique_ptr<RecordStore> rs = newRecordStore(engine);
    auto opCtxs = _makeOperationContexts(engine, 2);

    Timestamp t51(5, 1);
    Timestamp t52(5, 2);
    Timestamp t61(6, 1);

    ASSERT_EQ(engine->getAllDurableTimestamp(), Timestamp(StorageEngine::kMinimumTimestamp));

    auto opCtx1 = opCtxs[0].second.get();
    WriteUnitOfWork uow1(opCtx1);
    ASSERT_OK(
        rs->insertRecord(opCtx1, *shard_role_details::getRecoveryUnit(opCtx1), "abc", 4, t51));

    ASSERT_EQ(engine->getAllDurableTimestamp(), Timestamp(StorageEngine::kMinimumTimestamp));

    auto opCtx2 = opCtxs[1].second.get();
    WriteUnitOfWork uow2(opCtx2);
    ASSERT_OK(
        rs->insertRecord(opCtx2, *shard_role_details::getRecoveryUnit(opCtx2), "abc", 4, t61));
    uow2.commit();

    ASSERT_EQ(engine->getAllDurableTimestamp(), t51 - 1);

    ASSERT_OK(
        rs->insertRecord(opCtx1, *shard_role_details::getRecoveryUnit(opCtx1), "abc", 4, t52));

    ASSERT_EQ(engine->getAllDurableTimestamp(), t51 - 1);

    uow1.commit();

    ASSERT_EQ(engine->getAllDurableTimestamp(), t61);
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
TEST_F(KVEngineTestHarness, PinningOldestWithAnotherSession) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    std::unique_ptr<RecordStore> rs = newRecordStore(engine);
    auto opCtxs = _makeOperationContexts(engine, 2);

    auto opCtx1 = opCtxs[0].second.get();
    Lock::GlobalLock globalLk1(opCtx1, MODE_IX);
    WriteUnitOfWork uow1(opCtx1);
    StatusWith<RecordId> res = rs->insertRecord(
        opCtx1, *shard_role_details::getRecoveryUnit(opCtx1), "abc", 4, Timestamp(10, 10));
    RecordId rid = res.getValue();
    uow1.commit();

    RecordData rd;
    shard_role_details::getRecoveryUnit(opCtx1)->setTimestampReadSource(
        RecoveryUnit::ReadSource::kProvided, Timestamp(15, 15));

    auto opCtx2 = opCtxs[1].second.get();
    Lock::GlobalLock globalLk2(opCtx2, MODE_IX);
    WriteUnitOfWork uow2(opCtx2);

    ASSERT(rs->findRecord(opCtx1, *shard_role_details::getRecoveryUnit(opCtx1), rid, &rd));
    ASSERT_OK(shard_role_details::getRecoveryUnit(opCtx2)->setTimestamp(Timestamp(20, 20)));
    ASSERT_OK(
        rs->updateRecord(opCtx2, *shard_role_details::getRecoveryUnit(opCtx2), rid, "updated", 8));

    ASSERT(rs->findRecord(opCtx1, *shard_role_details::getRecoveryUnit(opCtx1), rid, &rd));
    ASSERT_EQUALS(std::string("abc"), rd.data());

    uow2.commit();

    shard_role_details::getRecoveryUnit(opCtx1)->abandonSnapshot();
    ASSERT(rs->findRecord(opCtx1, *shard_role_details::getRecoveryUnit(opCtx1), rid, &rd));
    ASSERT_EQUALS(std::string("abc"), rd.data());


    shard_role_details::getRecoveryUnit(opCtx2)->setTimestampReadSource(
        RecoveryUnit::ReadSource::kProvided, Timestamp(15, 15));
    ASSERT(rs->findRecord(opCtx2, *shard_role_details::getRecoveryUnit(opCtx2), rid, &rd));
    ASSERT_EQUALS(std::string("abc"), rd.data());
}

/*
 * All durable
 * | Session 1            | Session 2            | GlobalActor                      |
 * |----------------------+----------------------+----------------------------------|
 * | Begin                |                      |                                  |
 * | Timestamp :commit 10 |                      |                                  |
 * | Commit               |                      |                                  |
 * |                      |                      | QueryTimestamp :all_durable (10) |
 * | Begin                |                      |                                  |
 * | Timestamp :commit 20 |                      |                                  |
 * |                      |                      | QueryTimestamp :all_durable (10) |
 * |                      | Begin                |                                  |
 * |                      | Timestamp :commit 30 |                                  |
 * |                      | Commit               |                                  |
 * |                      |                      | QueryTimestamp :all_durable (19) |
 * | Commit               |                      |                                  |
 * |                      |                      | QueryTimestamp :all_durable (30) |
 * | Begin                |                      |                                  |
 * | Timestamp :commit 25 |                      |                                  |
 * |                      |                      | QueryTimestamp :all_durable (24) |
 */
TEST_F(KVEngineTestHarness, AllDurable) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    std::unique_ptr<RecordStore> rs = newRecordStore(engine);
    auto opCtxs = _makeOperationContexts(engine, 4);

    const Timestamp kInsertTimestamp1 = Timestamp(10, 10);
    const Timestamp kInsertTimestamp2 = Timestamp(20, 20);
    const Timestamp kInsertTimestamp3 = Timestamp(30, 30);
    const Timestamp kInsertTimestamp4 = Timestamp(25, 25);

    auto opCtx1 = opCtxs[0].second.get();
    WriteUnitOfWork uow1(opCtx1);
    auto swRid = rs->insertRecord(
        opCtx1, *shard_role_details::getRecoveryUnit(opCtx1), "abc", 4, kInsertTimestamp1);
    ASSERT_OK(swRid);
    uow1.commit();

    ASSERT_EQ(engine->getAllDurableTimestamp(), kInsertTimestamp1);

    auto opCtx2 = opCtxs[1].second.get();
    WriteUnitOfWork uow2(opCtx2);
    swRid = rs->insertRecord(
        opCtx2, *shard_role_details::getRecoveryUnit(opCtx2), "abc", 4, kInsertTimestamp2);
    ASSERT_OK(swRid);

    ASSERT_EQ(engine->getAllDurableTimestamp(), kInsertTimestamp1);

    auto opCtx3 = opCtxs[2].second.get();
    WriteUnitOfWork uow3(opCtx3);
    swRid = rs->insertRecord(
        opCtx3, *shard_role_details::getRecoveryUnit(opCtx3), "abc", 4, kInsertTimestamp3);
    ASSERT_OK(swRid);
    uow3.commit();

    ASSERT_EQ(engine->getAllDurableTimestamp(), kInsertTimestamp2 - 1);

    uow2.commit();

    ASSERT_EQ(engine->getAllDurableTimestamp(), kInsertTimestamp3);

    auto opCtx4 = opCtxs[3].second.get();
    WriteUnitOfWork uow4(opCtx4);
    swRid = rs->insertRecord(
        opCtx4, *shard_role_details::getRecoveryUnit(opCtx4), "abc", 4, kInsertTimestamp4);
    ASSERT_OK(swRid);

    ASSERT_EQ(engine->getAllDurableTimestamp(), kInsertTimestamp4 - 1);

    uow4.commit();
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
TEST_F(KVEngineTestHarness, BasicTimestampSingle) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    std::unique_ptr<RecordStore> rs = newRecordStore(engine);

    const Timestamp kReadTimestamp = Timestamp(9, 9);
    const Timestamp kInsertTimestamp = Timestamp(10, 10);

    auto opCtxs = _makeOperationContexts(engine, 2);

    // Start a read transaction.
    auto opCtx1 = opCtxs[0].second.get();
    Lock::GlobalLock globalLk(opCtx1, MODE_IS);

    shard_role_details::getRecoveryUnit(opCtx1)->setTimestampReadSource(
        RecoveryUnit::ReadSource::kProvided, kReadTimestamp);
    ASSERT(!rs->findRecord(
        opCtx1, *shard_role_details::getRecoveryUnit(opCtx1), RecordId::minLong(), nullptr));

    // Insert a record at a later time.
    RecordId rid;
    {
        auto opCtx2 = opCtxs[1].second.get();
        Lock::GlobalLock globalLk(opCtx2, MODE_IX);
        WriteUnitOfWork wuow(opCtx2);
        auto swRid = rs->insertRecord(
            opCtx2, *shard_role_details::getRecoveryUnit(opCtx2), "abc", 4, kInsertTimestamp);
        ASSERT_OK(swRid);
        rid = swRid.getValue();
        wuow.commit();
    }

    // Should not see the record, even if we abandon the snapshot as the read timestamp is still
    // earlier than the insert timestamp.
    ASSERT(!rs->findRecord(opCtx1, *shard_role_details::getRecoveryUnit(opCtx1), rid, nullptr));
    shard_role_details::getRecoveryUnit(opCtx1)->abandonSnapshot();
    ASSERT(!rs->findRecord(opCtx1, *shard_role_details::getRecoveryUnit(opCtx1), rid, nullptr));


    shard_role_details::getRecoveryUnit(opCtx1)->setTimestampReadSource(
        RecoveryUnit::ReadSource::kProvided, kInsertTimestamp);
    shard_role_details::getRecoveryUnit(opCtx1)->abandonSnapshot();
    RecordData rd;
    ASSERT(rs->findRecord(opCtx1, *shard_role_details::getRecoveryUnit(opCtx1), rid, &rd));
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
TEST_F(KVEngineTestHarness, BasicTimestampMultiple) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    std::unique_ptr<RecordStore> rs = newRecordStore(engine);

    const Timestamp t10 = Timestamp(10, 10);
    const Timestamp t20 = Timestamp(20, 20);

    RecordId rid;
    {
        // Initial insert of record.
        auto opCtx = _makeOperationContext(engine);
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        WriteUnitOfWork wuow(opCtx.get());
        auto swRid = rs->insertRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), "abc", 4, t10);
        ASSERT_OK(swRid);
        rid = swRid.getValue();

        // Update a record at a later time.
        ASSERT_OK(shard_role_details::getRecoveryUnit(opCtx.get())->setTimestamp(t20));
        auto res = rs->updateRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), rid, "updated", 8);
        ASSERT_OK(res);
        wuow.commit();
    }

    RecordData rd;
    auto opCtx = _makeOperationContext(engine);
    Lock::GlobalLock globalLk(opCtx.get(), MODE_S);
    shard_role_details::getRecoveryUnit(opCtx.get())
        ->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, t10);
    ASSERT(
        rs->findRecord(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), rid, &rd));
    ASSERT_EQUALS(std::string("abc"), rd.data());

    shard_role_details::getRecoveryUnit(opCtx.get())
        ->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, t20);
    shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();
    ASSERT(
        rs->findRecord(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), rid, &rd));
    ASSERT_EQUALS(std::string("updated"), rd.data());
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
DEATH_TEST_REGEX_F(KVEngineTestHarness, SnapshotHidesVisibility, ".*item not found.*") {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    std::unique_ptr<RecordStore> rs = newRecordStore(engine);

    auto opCtxs = _makeOperationContexts(engine, 2);

    auto opCtx1 = opCtxs[0].second.get();
    Lock::GlobalLock globalLk1(opCtx1, MODE_IX);
    WriteUnitOfWork uow1(opCtx1);
    StatusWith<RecordId> res = rs->insertRecord(
        opCtx1, *shard_role_details::getRecoveryUnit(opCtx1), "abc", 4, Timestamp(10, 10));
    ASSERT_OK(res);
    RecordId loc = res.getValue();
    uow1.commit();

    // Snapshot was taken before the insert and will not find the record even after the commit.
    auto opCtx2 = opCtxs[1].second.get();
    Lock::GlobalLock globalLk2(opCtx2, MODE_IX);
    shard_role_details::getRecoveryUnit(opCtx2)->setTimestampReadSource(
        RecoveryUnit::ReadSource::kProvided, Timestamp(9, 9));
    RecordData rd;
    ASSERT(!rs->findRecord(opCtx2, *shard_role_details::getRecoveryUnit(opCtx2), loc, &rd));

    // Trying to write in an outdated snapshot will cause item not found.
    WriteUnitOfWork uow2(opCtx2);
    auto swRid =
        rs->updateRecord(opCtx2, *shard_role_details::getRecoveryUnit(opCtx2), loc, "updated", 8);
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
TEST_F(KVEngineTestHarness, SingleReadWithConflictWithOplog) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();

    std::unique_ptr<RecordStore> collectionRs;
    std::unique_ptr<RecordStore> oplogRs;
    {
        collectionRs = newRecordStore(engine);

        RecordStore::Options oplogRecordStoreOptions;
        oplogRecordStoreOptions.isCapped = true;
        oplogRecordStoreOptions.isOplog = true;
        oplogRecordStoreOptions.oplogMaxSize = 10240;
        NamespaceString oplogNss = NamespaceString::createNamespaceString_forTest("local.oplog.rs");
        oplogRs = newRecordStore(
            engine, oplogNss, "collection-oplog", oplogRecordStoreOptions, UUID::gen());
    }

    RecordData rd;
    RecordId locCollection;
    RecordId locOplog;
    const Timestamp t9(9, 9);
    const Timestamp t10(10, 10);
    {
        auto opCtx = _makeOperationContext(engine);
        WriteUnitOfWork uow(opCtx.get());

        // Insert into collectionRs.
        StatusWith<RecordId> res = collectionRs->insertRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), "abc", 4, t10);
        ASSERT_OK(res);
        locCollection = res.getValue();

        // Insert into oplogRs.
        auto t11Doc = BSON("ts" << t10);

        ASSERT_EQ(invariant(oplogRs->insertRecord(opCtx.get(),
                                                  *shard_role_details::getRecoveryUnit(opCtx.get()),
                                                  t11Doc.objdata(),
                                                  t11Doc.objsize(),
                                                  Timestamp::min())),
                  RecordId(10, 10));
        locOplog = RecordId(10, 10);
        uow.commit();
    }

    auto opCtx = _makeOperationContext(engine);
    Lock::GlobalLock globalLk(opCtx.get(), MODE_S);
    shard_role_details::getRecoveryUnit(opCtx.get())
        ->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, t9);
    ASSERT(!collectionRs->findRecord(
        opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), locCollection, &rd));
    ASSERT(!oplogRs->findRecord(
        opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), locOplog, &rd));

    shard_role_details::getRecoveryUnit(opCtx.get())
        ->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, t10);
    shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();
    ASSERT(collectionRs->findRecord(
        opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), locCollection, &rd));
    ASSERT(oplogRs->findRecord(
        opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), locOplog, &rd));
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
TEST_F(KVEngineTestHarness, PinningOldestTimestampWithReadConflict) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    std::unique_ptr<RecordStore> rs = newRecordStore(engine);
    auto opCtx = _makeOperationContext(engine);

    Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
    WriteUnitOfWork uow(opCtx.get());
    StatusWith<RecordId> res = rs->insertRecord(opCtx.get(),
                                                *shard_role_details::getRecoveryUnit(opCtx.get()),
                                                "abc",
                                                4,
                                                Timestamp(10, 10));
    RecordId rid = res.getValue();
    uow.commit();

    RecordData rd;
    shard_role_details::getRecoveryUnit(opCtx.get())
        ->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, Timestamp(15, 15));
    ASSERT(
        rs->findRecord(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), rid, &rd));

    engine->setOldestTimestamp(Timestamp(20, 20), false);

    shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();
    ASSERT_THROWS_CODE(
        rs->findRecord(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), rid, &rd),
        DBException,
        ErrorCodes::SnapshotTooOld);
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
DEATH_TEST_REGEX_F(KVEngineTestHarness,
                   PinningOldestTimestampWithWriteConflict,
                   "Fatal assertion.*39001") {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();
    std::unique_ptr<RecordStore> rs = newRecordStore(engine);

    {
        // A write transaction cannot insert records before the oldest timestamp.
        engine->setOldestTimestamp(Timestamp(2, 2), false);
        auto opCtx = _makeOperationContext(engine);
        WriteUnitOfWork uow2(opCtx.get());
        StatusWith<RecordId> res =
            rs->insertRecord(opCtx.get(),
                             *shard_role_details::getRecoveryUnit(opCtx.get()),
                             "abc",
                             4,
                             Timestamp(1, 1));
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
TEST_F(KVEngineTestHarness, RollingBackToLastStable) {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();

    // The initial data timestamp has to be set to take stable checkpoints.
    engine->setInitialDataTimestamp(Timestamp(1, 1));
    std::unique_ptr<RecordStore> rs = newRecordStore(engine);

    RecordId ridA;
    {
        auto opCtx = _makeOperationContext(engine);
        WriteUnitOfWork uow(opCtx.get());
        auto res = rs->insertRecord(opCtx.get(),
                                    *shard_role_details::getRecoveryUnit(opCtx.get()),
                                    "abc",
                                    4,
                                    Timestamp(1, 1));
        ASSERT_OK(res);
        ridA = res.getValue();
        uow.commit();
        ASSERT_EQUALS(1, rs->numRecords());
    }

    {
        // Set the stable timestamp to (1, 1) as it can't be set higher than the all durable
        // timestamp, which is (1, 1) in this case.
        ASSERT(!engine->getLastStableRecoveryTimestamp());
        ASSERT_EQUALS(engine->getAllDurableTimestamp(), Timestamp(1, 1));
        engine->setStableTimestamp(Timestamp(1, 1), false);
        ASSERT(!engine->getLastStableRecoveryTimestamp());

        // Force a checkpoint to be taken. This should advance the last stable timestamp.
        auto opCtx = _makeOperationContext(engine);
        engine->flushAllFiles(opCtx.get(), false);
        ASSERT_EQ(engine->getLastStableRecoveryTimestamp(), Timestamp(1, 1));
    }

    RecordId ridB;
    {
        // Insert a record after the stable timestamp.
        auto opCtx = _makeOperationContext(engine);
        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> swRid =
            rs->insertRecord(opCtx.get(),
                             *shard_role_details::getRecoveryUnit(opCtx.get()),
                             "def",
                             4,
                             Timestamp(3, 3));
        ASSERT_OK(swRid);
        ridB = swRid.getValue();
        ASSERT_EQUALS(2, rs->numRecords());
        uow.commit();
    }

    {
        // Rollback to the last stable timestamp.
        auto opCtx = _makeOperationContext(engine);
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        StatusWith<Timestamp> swTimestamp = engine->recoverToStableTimestamp(*opCtx.get());
        ASSERT_EQ(swTimestamp.getValue(), Timestamp(1, 1));

        // Verify that we can find record A and can't find the record B inserted at Timestamp(3, 3)
        // in the collection any longer. 'numRecords' will still show two as it's the fast count and
        // doesn't get reflected during the rollback.
        RecordData rd;
        shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();
        ASSERT(rs->findRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), ridA, &rd));
        ASSERT_EQ(std::string("abc"), rd.data());
        ASSERT_FALSE(rs->findRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), ridB, nullptr));
        ASSERT_EQUALS(2, rs->numRecords());
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
DEATH_TEST_REGEX_F(KVEngineTestHarness, CommitBehindStable, "Fatal assertion.*39001") {
    std::unique_ptr<KVHarnessHelper> helper(KVHarnessHelper::create(getServiceContext()));
    KVEngine* engine = helper->getEngine();

    // The initial data timestamp has to be set to take stable checkpoints.
    engine->setInitialDataTimestamp(Timestamp(1, 1));
    std::unique_ptr<RecordStore> rs = newRecordStore(engine);

    {
        // Set the stable timestamp to (2, 2).
        ASSERT(!engine->getLastStableRecoveryTimestamp());
        engine->setStableTimestamp(Timestamp(2, 2), false);
        ASSERT(!engine->getLastStableRecoveryTimestamp());

        // Force a checkpoint to be taken. This should advance the last stable timestamp.
        auto opCtx = _makeOperationContext(engine);
        engine->flushAllFiles(opCtx.get(), false);
        ASSERT_EQ(engine->getLastStableRecoveryTimestamp(), Timestamp(2, 2));
    }

    {
        // Committing at or behind the stable timestamp is not allowed.
        auto opCtx = _makeOperationContext(engine);
        WriteUnitOfWork uow(opCtx.get());
        auto swRid = rs->insertRecord(opCtx.get(),
                                      *shard_role_details::getRecoveryUnit(opCtx.get()),
                                      "abc",
                                      4,
                                      Timestamp(1, 1));
        uow.commit();
    }
}

TEST_F(MDBCatalogTest, Coll1) {
    std::unique_ptr<RecordStore> catalogRS = createCatalogRS();
    std::unique_ptr<MDBCatalog> catalog = createMDBCatalog(catalogRS.get());
    RecordId catalogId;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        catalogId = addNewCatalogEntry(opCtx,
                                       NamespaceString::createNamespaceString_forTest("a.b"),
                                       CollectionOptions(),
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
        catalog = createMDBCatalog(catalogRS.get());
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
        newCatalogId = addNewCatalogEntry(opCtx,
                                          NamespaceString::createNamespaceString_forTest("a.b"),
                                          CollectionOptions(),
                                          catalog.get());
        uow.commit();
    }
    ASSERT_NOT_EQUALS(ident, catalog->getEntry(newCatalogId).ident);
}

TEST_F(MDBCatalogTest, Idx1) {
    std::unique_ptr<RecordStore> catalogRS = createCatalogRS();
    std::unique_ptr<MDBCatalog> catalog = createMDBCatalog(catalogRS.get());

    RecordId catalogId;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        catalogId = addNewCatalogEntry(opCtx,
                                       NamespaceString::createNamespaceString_forTest("a.b"),
                                       CollectionOptions(),
                                       catalog.get());
        ASSERT_NOT_EQUALS("a.b", catalog->getEntry(catalogId).ident);
        ASSERT_TRUE(ident::isCollectionOrIndexIdent(catalog->getEntry(catalogId).ident));
        uow.commit();
    }

    std::string idxIdent;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);

        durable_catalog::CatalogEntryMetaData md;
        md.nss = NamespaceString::createNamespaceString_forTest(boost::none, "a.b");

        durable_catalog::CatalogEntryMetaData::IndexMetaData imd;
        imd.spec = BSON("name" << "foo");
        imd.ready = false;
        imd.multikey = false;
        idxIdent = ident::generateNewIndexIdent(md.nss.dbName(), false, false);
        md.indexes.push_back(imd);
        putMetaData(opCtx, catalog.get(), catalogId, md, BSON("foo" << idxIdent));
        uow.commit();
    }

    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        ASSERT_EQUALS(idxIdent, getIndexIdent(opCtx, catalog.get(), catalogId, "foo"));
        ASSERT_TRUE(
            ident::isCollectionOrIndexIdent(getIndexIdent(opCtx, catalog.get(), catalogId, "foo")));
    }

    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);

        durable_catalog::CatalogEntryMetaData md;
        md.nss = NamespaceString::createNamespaceString_forTest(boost::none, "a.b");
        putMetaData(opCtx, catalog.get(), catalogId, md);  // remove index

        durable_catalog::CatalogEntryMetaData::IndexMetaData imd;
        imd.spec = BSON("name" << "foo");
        imd.ready = false;
        imd.multikey = false;
        md.indexes.push_back(imd);
        putMetaData(opCtx,
                    catalog.get(),
                    catalogId,
                    md,
                    BSON("foo" << ident::generateNewIndexIdent(md.nss.dbName(), false, false)));
        uow.commit();
    }

    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        ASSERT_NOT_EQUALS(idxIdent, getIndexIdent(opCtx, catalog.get(), catalogId, "foo"));
    }
}

TEST_F(MDBCatalogTest, BackupImplemented) {
    KVEngine* engine = helper->getEngine();
    ASSERT(engine);
    ASSERT_OK(engine->beginBackup());
    engine->endBackup();
}

TEST_F(MDBCatalogTest, AddRemoveAddRollBack) {
    std::unique_ptr<RecordStore> catalogRS = createCatalogRS();
    std::unique_ptr<MDBCatalog> catalog = createMDBCatalog(catalogRS.get());

    RecordId catalogId;
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        catalogId = addNewCatalogEntry(opCtx, nss, CollectionOptions(), catalog.get());
        auto entry = catalog->getEntry(catalogId);
        auto rawEntry = catalog->getRawCatalogEntry(opCtx, catalogId);
        ASSERT_OK(catalog->removeEntry(opCtx, catalogId));
        ASSERT_OK(addEntry(opCtx, entry.ident, entry.nss, rawEntry, catalogId, catalog.get()));
    }
    ASSERT_FALSE(catalog->getEntry_forTest(catalogId).has_value());
}

TEST_F(MDBCatalogTest, AddRemoveAddCommit) {
    std::unique_ptr<RecordStore> catalogRS = createCatalogRS();
    std::unique_ptr<MDBCatalog> catalog = createMDBCatalog(catalogRS.get());

    RecordId catalogId;
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork wuow(opCtx);
        catalogId = addNewCatalogEntry(opCtx, nss, CollectionOptions(), catalog.get());
        auto entry = catalog->getEntry(catalogId);
        auto rawEntry = catalog->getRawCatalogEntry(opCtx, catalogId);
        ASSERT_OK(catalog->removeEntry(opCtx, catalogId));
        ASSERT_OK(addEntry(opCtx, entry.ident, entry.nss, rawEntry, catalogId, catalog.get()));
        wuow.commit();
    }
    ASSERT_TRUE(catalog->getEntry_forTest(catalogId).has_value());
}

TEST_F(MDBCatalogTest, EntryIncludesTenantIdInMultitenantEnv) {
    gMultitenancySupport = true;
    std::unique_ptr<RecordStore> catalogRS = createCatalogRS();
    std::unique_ptr<MDBCatalog> catalog = createMDBCatalog(catalogRS.get());

    // Insert an entry into the MDBCatalog, and ensure the tenantId is stored on the nss in the
    // entry.
    RecordId catalogId;
    auto tenantId = TenantId(OID::gen());
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(tenantId, "a.b");
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        WriteUnitOfWork uow(opCtx);
        catalogId = addNewCatalogEntry(opCtx, nss, CollectionOptions(), catalog.get());
        uow.commit();
    }
    ASSERT_EQUALS(nss.tenantId(), catalog->getEntry(catalogId).nss.tenantId());
    ASSERT_EQUALS(nss, catalog->getEntry(catalogId).nss);

    // Re-initialize the MDBCatalog (as if it read from disk). Ensure the tenantId is still
    // stored on the nss in the entry.
    std::string ident = catalog->getEntry(catalogId).ident;
    {
        auto clientAndCtx = makeClientAndCtx("opCtx");
        auto opCtx = clientAndCtx.opCtx();
        Lock::GlobalLock globalLk(opCtx, MODE_IX);

        WriteUnitOfWork uow(opCtx);
        createMDBCatalog(catalogRS.get());
        catalog->init(opCtx);
        uow.commit();
    }
    ASSERT_EQUALS(ident, catalog->getEntry(catalogId).ident);
    ASSERT_EQUALS(nss.tenantId(), catalog->getEntry(catalogId).nss.tenantId());
    ASSERT_EQUALS(nss, catalog->getEntry(catalogId).nss);

    gMultitenancySupport = false;
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
