/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <memory>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/catalog_control.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/capped_visibility.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

class CappedCollectionTest : public ServiceContextMongoDTest {
public:
    CappedCollectionTest() : ServiceContextMongoDTest(Options().engine("wiredTiger")) {}

protected:
    void setUp() override {
        // Set up mongod.
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();
        _storage = std::make_unique<repl::StorageInterfaceImpl>();

        // Set up ReplicationCoordinator and ensure that we are primary.
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));

        // Set up oplog collection. If the WT storage engine is used, the oplog collection is
        // expected to exist when fetching the next opTime (LocalOplogInfo::getNextOpTimes) to use
        // for a write.
        {
            auto opCtx = newOperationContext();
            repl::createOplog(opCtx.get());
        }
    }


    void makeCapped(NamespaceString nss, long long cappedSize = 8192) {
        CollectionOptions options;
        options.capped = true;
        options.cappedSize = cappedSize;  // Maximum size of capped collection in bytes.
        bool createIdIndex = false;
        auto opCtx = newOperationContext();
        ASSERT_OK(storageInterface()->createCollection(opCtx.get(), nss, options, createIdIndex));
    }

    ServiceContext::UniqueOperationContext newOperationContext() {
        return Client::getCurrent()->makeOperationContext();
    }

    typedef std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
        ClientAndCtx;

    ClientAndCtx makeClientAndCtx(const std::string& clientName) {
        auto client = getServiceContext()->getService()->makeClient(clientName);
        auto opCtx = client->makeOperationContext();
        return std::make_pair(std::move(client), std::move(opCtx));
    }

    repl::StorageInterface* storageInterface() {
        return _storage.get();
    }

    std::unique_ptr<repl::StorageInterface> _storage;
};

template <typename T>
void assertSwError(StatusWith<T> sw, ErrorCodes::Error code) {
    ASSERT_EQ(sw.getStatus().code(), code);
}

Status insertBSON(OperationContext* opCtx, const NamespaceString& nss, RecordId id) {
    AutoGetCollection ac(opCtx, nss, MODE_IX);
    const CollectionPtr& coll = ac.getCollection();
    BSONObj obj = BSON("a" << 1);
    WriteUnitOfWork wuow(opCtx);

    auto cappedObserver = coll->getCappedVisibilityObserver();
    cappedObserver->registerWriter(shard_role_details::getRecoveryUnit(opCtx));

    coll->registerCappedInsert(opCtx, id);
    auto status =
        collection_internal::insertDocument(opCtx, coll, InsertStatement(obj, id), nullptr);
    if (!status.isOK()) {
        return status;
    }
    wuow.commit();
    return Status::OK();
}

Status _insertBSON(OperationContext* opCtx, const CollectionPtr& coll, RecordId id) {
    BSONObj obj = BSON("a" << 1);
    auto cappedObserver = coll->getCappedVisibilityObserver();
    cappedObserver->registerWriter(shard_role_details::getRecoveryUnit(opCtx));
    coll->registerCappedInsert(opCtx, id);
    return collection_internal::insertDocument(opCtx, coll, InsertStatement(obj, id), nullptr);
}

Status _insertOplogBSON(OperationContext* opCtx, const CollectionPtr& coll, RecordId id) {
    BSONObj obj = BSON("ts" << Timestamp(id.getLong()));
    return collection_internal::insertDocument(opCtx, coll, InsertStatement(obj, id), nullptr);
}

TEST_F(CappedCollectionTest, InsertOutOfOrder) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("local.non.oplog");
    makeCapped(nss);
    {
        // RecordId's are inserted out-of-order.
        auto opCtx = newOperationContext();
        ASSERT_OK(insertBSON(opCtx.get(), nss, RecordId(1)));
        ASSERT_OK(insertBSON(opCtx.get(), nss, RecordId(3)));
        ASSERT_OK(insertBSON(opCtx.get(), nss, RecordId(2)));
    }

    {
        auto opCtx = newOperationContext();
        AutoGetCollectionForRead acfr(opCtx.get(), nss);
        const CollectionPtr& coll = acfr.getCollection();
        auto cursor = coll->getCursor(opCtx.get());
        ASSERT_EQ(cursor->next()->id, RecordId(1));
        ASSERT_EQ(cursor->next()->id, RecordId(2));
        ASSERT_EQ(cursor->next()->id, RecordId(3));
        ASSERT(!cursor->next());
    }
}

TEST_F(CappedCollectionTest, OplogOrder) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("local.non.oplog");
    makeCapped(nss);

    auto id1 = RecordId(1);

    {  // first insert a document
        ServiceContext::UniqueOperationContext opCtx(newOperationContext());
        ASSERT_OK(insertBSON(opCtx.get(), nss, id1));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(newOperationContext());
        AutoGetCollectionForRead ac(opCtx.get(), nss);
        const CollectionPtr& coll = ac.getCollection();
        auto cursor = coll->getCursor(opCtx.get());
        auto record = cursor->seekExact(id1);
        ASSERT(record);
        ASSERT_EQ(id1, record->id);
        ASSERT(!cursor->next());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(newOperationContext());
        AutoGetCollectionForRead ac(opCtx.get(), nss);
        const CollectionPtr& coll = ac.getCollection();
        auto cursor = coll->getCursor(opCtx.get());
        auto record = cursor->seek(RecordId(id1.getLong() + 1),
                                   SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT_FALSE(record);
    }

    {
        // now we insert 2 docs, but commit the 2nd one first.
        // we make sure we can't find the 2nd until the first is committed.
        auto [earlyClient, earlyCtx] = makeClientAndCtx("earlyReader");
        AutoGetCollectionForRead ac(earlyCtx.get(), nss);
        const CollectionPtr& coll = ac.getCollection();

        auto earlyCursor = coll->getCursor(earlyCtx.get());
        ASSERT_EQ(earlyCursor->seekExact(id1)->id, id1);
        coll.yield();
        earlyCursor->save();
        shard_role_details::getRecoveryUnit(earlyCtx.get())->abandonSnapshot();

        auto [c1, t1] = makeClientAndCtx("t1");
        AutoGetCollection ac1(t1.get(), nss, MODE_IX);
        WriteUnitOfWork w1(t1.get());
        auto id2 = RecordId(20);
        ASSERT_OK(_insertBSON(t1.get(), ac1.getCollection(), id2));
        // do not commit yet

        auto id3 = RecordId(30);
        {  // create 2nd doc
            auto t2 = newOperationContext();
            AutoGetCollection ac2(t2.get(), nss, MODE_IX);
            {
                WriteUnitOfWork w2(t2.get());
                ASSERT_OK(_insertBSON(t2.get(), ac2.getCollection(), id3));
                w2.commit();
            }
        }

        {  // Other operations should not be able to see 2nd doc until w1 commits.
            coll.restore();
            earlyCursor->restore();
            ASSERT(!earlyCursor->next());
        }

        {
            auto [c2, t2] = makeClientAndCtx("t2");
            AutoGetCollectionForRead ac2(t2.get(), nss);
            auto cursor = ac2.getCollection()->getCursor(t2.get());
            auto record = cursor->seekExact(id1);
            ASSERT(record);
            ASSERT_EQ(id1, record->id);
            ASSERT(!cursor->next());
        }

        {
            auto [c2, t2] = makeClientAndCtx("t2");
            AutoGetCollectionForRead ac2(t2.get(), nss);
            auto cursor = coll->getCursor(t2.get());
            auto record = cursor->seek(id2, SeekableRecordCursor::BoundInclusion::kInclude);
            ASSERT_FALSE(record);
        }

        {
            auto [c2, t2] = makeClientAndCtx("t2");
            AutoGetCollectionForRead ac2(t2.get(), nss);
            auto cursor = coll->getCursor(t2.get());
            auto record = cursor->seek(id3, SeekableRecordCursor::BoundInclusion::kInclude);
            ASSERT_FALSE(record);
        }

        w1.commit();
    }

    {  // now all 3 docs should be visible
        auto opCtx = newOperationContext();
        AutoGetCollectionForRead ac(opCtx.get(), nss);
        const CollectionPtr& coll = ac.getCollection();
        auto cursor = coll->getCursor(opCtx.get());
        auto record = cursor->seekExact(id1);
        ASSERT_EQ(id1, record->id);
        ASSERT(cursor->next());
        ASSERT(cursor->next());
        ASSERT(!cursor->next());
    }

    // Rollback the last two writes entries, then insert entries with older RecordIds to ensure that
    // the visibility rules aren't violated.
    {
        auto opCtx = newOperationContext();
        AutoGetCollectionForRead ac(opCtx.get(), nss);
        const CollectionPtr& coll = ac.getCollection();
        coll->getRecordStore()->capped()->truncateAfter(
            opCtx.get(), id1, /*inclusive*/ false, [](auto _1, auto _2, auto _3) {});
    }

    {
        // Now we insert 2 docs with timestamps earlier than before, but commit the 2nd one first.
        // We make sure we can't find the 2nd until the first is committed.
        auto [earlyClient, earlyCtx] = makeClientAndCtx("earlyReader");
        AutoGetCollectionForRead ac(earlyCtx.get(), nss);
        const CollectionPtr& coll = ac.getCollection();
        auto earlyCursor = coll->getCursor(earlyCtx.get());
        ASSERT_EQ(earlyCursor->seekExact(id1)->id, id1);
        coll.yield();
        earlyCursor->save();
        shard_role_details::getRecoveryUnit(earlyCtx.get())->abandonSnapshot();

        auto [c1, t1] = makeClientAndCtx("t1");
        AutoGetCollection ac1(t1.get(), nss, MODE_IX);
        WriteUnitOfWork w1(t1.get());
        auto id2 = RecordId(2);
        ASSERT_OK(_insertBSON(t1.get(), ac1.getCollection(), id2));

        // do not commit yet

        auto id3 = RecordId(3);
        {  // create 2nd doc
            auto t2 = newOperationContext();
            AutoGetCollection ac2(t2.get(), nss, MODE_IX);
            {
                WriteUnitOfWork w2(t2.get());
                ASSERT_OK(_insertBSON(t2.get(), ac2.getCollection(), id3));
                w2.commit();
            }
        }

        // Other operations should not be able to see 2nd doc until w1 commits.
        coll.restore();
        ASSERT(earlyCursor->restore());
        ASSERT(!earlyCursor->next());
        {
            auto [c2, t2] = makeClientAndCtx("t2");
            AutoGetCollectionForRead ac2(t2.get(), nss);
            auto cursor = coll->getCursor(t2.get());
            auto record = cursor->seekExact(id1);
            ASSERT(record);
            ASSERT_EQ(id1, record->id);
            ASSERT(!cursor->next());
        }

        {
            auto [c2, t2] = makeClientAndCtx("t2");
            AutoGetCollectionForRead ac2(t2.get(), nss);
            auto cursor = coll->getCursor(t2.get());
            auto record = cursor->seek(id2, SeekableRecordCursor::BoundInclusion::kInclude);
            ASSERT_FALSE(record);
        }

        {
            auto [c2, t2] = makeClientAndCtx("t2");
            AutoGetCollectionForRead ac2(t2.get(), nss);
            auto cursor = coll->getCursor(t2.get());
            auto record = cursor->seek(id3, SeekableRecordCursor::BoundInclusion::kInclude);
            ASSERT_FALSE(record);
        }

        w1.commit();
    }

    {  // now all 3 docs should be visible
        ServiceContext::UniqueOperationContext opCtx(newOperationContext());
        AutoGetCollectionForRead ac(opCtx.get(), nss);
        const CollectionPtr& coll = ac.getCollection();
        auto cursor = coll->getCursor(opCtx.get());
        auto record = cursor->seekExact(id1);
        ASSERT_EQ(id1, record->id);
        ASSERT(cursor->next());
        ASSERT(cursor->next());
        ASSERT(!cursor->next());
    }
}

TEST_F(CappedCollectionTest, VisibilityAfterRestart) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("local.non.oplog");
    makeCapped(nss);

    {
        auto opCtx = newOperationContext();
        ASSERT_OK(insertBSON(opCtx.get(), nss, RecordId(1)));
        ASSERT_OK(insertBSON(opCtx.get(), nss, RecordId(2)));
        ASSERT_OK(insertBSON(opCtx.get(), nss, RecordId(3)));
        ASSERT_OK(insertBSON(opCtx.get(), nss, RecordId(4)));
    }
    const auto lastCommittedId = RecordId(4);

    // Simulate restart / clean start. Force catalog close and reopen, causing in-memory catalog
    // structures to be reset.
    {
        auto opCtx = newOperationContext();
        auto stableTimestamp =
            storageInterface()->getLastStableRecoveryTimestamp(getServiceContext());

        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        auto catalogState = catalog::closeCatalog(opCtx.get());
        catalog::openCatalog(opCtx.get(), catalogState, *stableTimestamp);
    }

    const auto uncommittedId = RecordId(5);

    auto fp = globalFailPointRegistry().find("hangAfterEstablishCappedSnapshot");
    fp->setMode(FailPoint::alwaysOn);
    stdx::thread concurrentReader([&] {
        // Instantiate AutoGetCollection before uncommitted write.
        auto [c2, t2] = makeClientAndCtx("t2");
        AutoGetCollectionForReadLockFree acr(t2.get(), nss);
        auto cursor = acr.getCollection()->getCursor(t2.get());
        auto record = cursor->seekExact(lastCommittedId);
        ASSERT(record);
        ASSERT_EQ(lastCommittedId, record->id);
        ASSERT(!cursor->next());
    });

    fp->waitForTimesEntered(1);

    // Make uncommitted write (hole).
    auto [c1, t1] = makeClientAndCtx("t1");
    AutoGetCollection ac1(t1.get(), nss, MODE_IX);
    WriteUnitOfWork w1(t1.get());
    ASSERT_OK(_insertBSON(t1.get(), ac1.getCollection(), uncommittedId));
    // do not commit yet

    const auto pastHoleId = RecordId(6);
    {  // Insert after hole.
        auto t2 = newOperationContext();
        AutoGetCollection ac2(t2.get(), nss, MODE_IX);
        {
            WriteUnitOfWork w2(t2.get());
            ASSERT_OK(_insertBSON(t2.get(), ac2.getCollection(), pastHoleId));
            w2.commit();
        }
    }

    fp->setMode(FailPoint::off);
    concurrentReader.join();

    // Close the hole.
    w1.commit();
    {
        auto [c2, t2] = makeClientAndCtx("t2");
        AutoGetCollectionForReadLockFree acr(t2.get(), nss);
        auto cursor = acr.getCollection()->getCursor(t2.get());
        auto record = cursor->seekExact(lastCommittedId);
        ASSERT(record);
        ASSERT_EQ(lastCommittedId, record->id);

        auto next = cursor->next();
        ASSERT(next);
        ASSERT_EQ(uncommittedId, next->id);

        next = cursor->next();
        ASSERT(next);
        ASSERT_EQ(pastHoleId, next->id);
    }
}

TEST_F(CappedCollectionTest, SeekOplogWithReadTimestamp) {
    NamespaceString nss = NamespaceString::kRsOplogNamespace;
    const auto oneSec = Timestamp(1, 0).asULL();
    {
        auto [c1, t1] = makeClientAndCtx("t1");
        WriteUnitOfWork wuow(t1.get());
        AutoGetCollection ac(t1.get(), nss, MODE_IX);
        const CollectionPtr& oplog = ac.getCollection();
        ASSERT_OK(_insertOplogBSON(t1.get(), oplog, RecordId(oneSec + 2)));
        ASSERT_OK(_insertOplogBSON(t1.get(), oplog, RecordId(oneSec + 4)));
        ASSERT_OK(_insertOplogBSON(t1.get(), oplog, RecordId(oneSec + 6)));
        ASSERT_OK(_insertOplogBSON(t1.get(), oplog, RecordId(oneSec + 8)));
        wuow.commit();
    }

#define checkSeek(cursor, recordNum, expectedInclusiveNum, expectedExclusiveNum)    \
    do {                                                                            \
        auto record = cursor->seek(RecordId(oneSec + recordNum),                    \
                                   SeekableRecordCursor::BoundInclusion::kInclude); \
        if (expectedInclusiveNum > 0) {                                             \
            ASSERT(record);                                                         \
            ASSERT_EQ(expectedInclusiveNum, record->id.getLong() - oneSec);         \
        } else {                                                                    \
            ASSERT(!record);                                                        \
        }                                                                           \
        record = cursor->seek(RecordId(oneSec + recordNum),                         \
                              SeekableRecordCursor::BoundInclusion::kExclude);      \
        if (expectedExclusiveNum > 0) {                                             \
            ASSERT(record);                                                         \
            ASSERT_EQ(expectedExclusiveNum, record->id.getLong() - oneSec);         \
        } else {                                                                    \
            ASSERT(!record);                                                        \
        }                                                                           \
    } while (0);

    // Forward, no read timestamp.
    {
        auto [c2, t2] = makeClientAndCtx("t2");
        AutoGetCollectionForReadLockFree acr(t2.get(), nss);
        shard_role_details::getRecoveryUnit(t2.get())->setOplogVisibilityTs(boost::none);
        auto cursor = acr.getCollection()->getCursor(t2.get());
        checkSeek(cursor, 1, 2, 2);
        checkSeek(cursor, 2, 2, 4);
        checkSeek(cursor, 3, 4, 4);
        checkSeek(cursor, 4, 4, 6);
        checkSeek(cursor, 5, 6, 6);
        checkSeek(cursor, 6, 6, 8);
        checkSeek(cursor, 7, 8, 8);
        checkSeek(cursor, 8, 8, -1);
        checkSeek(cursor, 9, -1, -1);
    }
    // Backward, no read timestamp.
    {
        auto [c2, t2] = makeClientAndCtx("t2");
        AutoGetCollectionForReadLockFree acr(t2.get(), nss);
        auto cursor = acr.getCollection()->getCursor(t2.get(), false);
        checkSeek(cursor, 1, -1, -1);
        checkSeek(cursor, 2, 2, -1);
        checkSeek(cursor, 3, 2, 2);
        checkSeek(cursor, 4, 4, 2);
        checkSeek(cursor, 5, 4, 4);
        checkSeek(cursor, 6, 6, 4);
        checkSeek(cursor, 7, 6, 6);
        checkSeek(cursor, 8, 8, 6);
        checkSeek(cursor, 9, 8, 8);
    }
    // Forward, with read timestamp.
    {
        auto [c2, t2] = makeClientAndCtx("t2");
        shard_role_details::getRecoveryUnit(t2.get())->setTimestampReadSource(
            RecoveryUnit::ReadSource::kProvided, Timestamp(1, 6));
        AutoGetCollectionForReadLockFree acr(t2.get(), nss);
        shard_role_details::getRecoveryUnit(t2.get())->setOplogVisibilityTs(boost::none);
        auto cursor = acr.getCollection()->getCursor(t2.get());
        checkSeek(cursor, 1, 2, 2);
        checkSeek(cursor, 2, 2, 4);
        checkSeek(cursor, 3, 4, 4);
        checkSeek(cursor, 4, 4, 6);
        checkSeek(cursor, 5, 6, 6);
        checkSeek(cursor, 6, 6, -1);
        checkSeek(cursor, 7, -1, -1);
        checkSeek(cursor, 8, -1, -1);
        checkSeek(cursor, 9, -1, -1);
    }
    // Backward, with read timestamp.
    {
        auto [c2, t2] = makeClientAndCtx("t2");
        shard_role_details::getRecoveryUnit(t2.get())->setTimestampReadSource(
            RecoveryUnit::ReadSource::kProvided, Timestamp(1, 6));
        AutoGetCollectionForReadLockFree acr(t2.get(), nss);
        auto cursor = acr.getCollection()->getCursor(t2.get(), false);
        checkSeek(cursor, 1, -1, -1);
        checkSeek(cursor, 2, 2, -1);
        checkSeek(cursor, 3, 2, 2);
        checkSeek(cursor, 4, 4, 2);
        checkSeek(cursor, 5, 4, 4);
        checkSeek(cursor, 6, 6, 4);
        checkSeek(cursor, 7, 6, 6);
        checkSeek(cursor, 8, 6, 6);
        checkSeek(cursor, 9, 6, 6);
    }
}

}  // namespace
}  // namespace mongo
