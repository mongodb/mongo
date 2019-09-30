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

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/idempotency_test_fixture.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/platform/mutex.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/scopeguard.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace repl {
namespace {

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
OplogEntry makeOplogEntry(OpTypeEnum opType, NamespaceString nss, OptionalCollectionUUID uuid) {
    return OplogEntry(OpTime(Timestamp(1, 1), 1),  // optime
                      boost::none,                 // hash
                      opType,                      // opType
                      nss,                         // namespace
                      uuid,                        // uuid
                      boost::none,                 // fromMigrate
                      OplogEntry::kOplogVersion,   // version
                      BSON("_id" << 0),            // o
                      boost::none,                 // o2
                      {},                          // sessionInfo
                      boost::none,                 // upsert
                      Date_t(),                    // wall clock time
                      boost::none,                 // statement id
                      boost::none,   // optime of previous write within same transaction
                      boost::none,   // pre-image optime
                      boost::none);  // post-image optime
}

/**
 * Testing-only SyncTail.
 */
class SyncTailForTest : public SyncTail {
public:
    SyncTailForTest();
};

SyncTailForTest::SyncTailForTest()
    : SyncTail(nullptr,  // observer
               nullptr,  // storage interface
               repl::OplogApplier::Options(repl::OplogApplication::Mode::kInitialSync)) {}

/**
 * Creates collection options suitable for oplog.
 */
CollectionOptions createOplogCollectionOptions() {
    CollectionOptions options;
    options.capped = true;
    options.cappedSize = 64 * 1024 * 1024LL;
    options.autoIndexId = CollectionOptions::NO;
    return options;
}

/**
 * Create test collection.
 * Returns collection.
 */
void createCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const CollectionOptions& options) {
    writeConflictRetry(opCtx, "createCollection", nss.ns(), [&] {
        Lock::DBLock dblk(opCtx, nss.db(), MODE_X);
        OldClientContext ctx(opCtx, nss.ns());
        auto db = ctx.db();
        ASSERT_TRUE(db);
        mongo::WriteUnitOfWork wuow(opCtx);
        auto coll = db->createCollection(opCtx, nss, options);
        ASSERT_TRUE(coll);
        wuow.commit();
    });
}


/**
 * Create test collection with UUID.
 */
auto createCollectionWithUuid(OperationContext* opCtx, const NamespaceString& nss) {
    CollectionOptions options;
    options.uuid = UUID::gen();
    createCollection(opCtx, nss, options);
    return options.uuid.get();
}

/**
 * Create test database.
 */
void createDatabase(OperationContext* opCtx, StringData dbName) {
    Lock::GlobalWrite globalLock(opCtx);
    bool justCreated;
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->openDb(opCtx, dbName, &justCreated);
    ASSERT_TRUE(db);
    ASSERT_TRUE(justCreated);
}

/**
 * Returns true if collection exists.
 */
bool collectionExists(OperationContext* opCtx, const NamespaceString& nss) {
    return AutoGetCollectionForRead(opCtx, nss).getCollection() != nullptr;
}

auto parseFromOplogEntryArray(const BSONObj& obj, int elem) {
    BSONElement tsArray;
    Status status =
        bsonExtractTypedField(obj, OpTime::kTimestampFieldName, BSONType::Array, &tsArray);
    ASSERT_OK(status);

    BSONElement termArray;
    status = bsonExtractTypedField(obj, OpTime::kTermFieldName, BSONType::Array, &termArray);
    ASSERT_OK(status);

    return OpTime(tsArray.Array()[elem].timestamp(), termArray.Array()[elem].Long());
};

TEST_F(SyncTailTest, SyncApplyInsertDocumentDatabaseMissing) {
    NamespaceString nss("test.t");
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, {});
    ASSERT_THROWS(_syncApplyWrapper(_opCtx.get(), &op, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
}

TEST_F(SyncTailTest, SyncApplyDeleteDocumentDatabaseMissing) {
    NamespaceString otherNss("test.othername");
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, {});
    _testSyncApplyCrudOperation(ErrorCodes::OK, op, false);
}

TEST_F(SyncTailTest, SyncApplyInsertDocumentCollectionLookupByUUIDFails) {
    const NamespaceString nss("test.t");
    createDatabase(_opCtx.get(), nss.db());
    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kInsert, otherNss, kUuid);
    ASSERT_THROWS(_syncApplyWrapper(_opCtx.get(), &op, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
}

TEST_F(SyncTailTest, SyncApplyDeleteDocumentCollectionLookupByUUIDFails) {
    const NamespaceString nss("test.t");
    createDatabase(_opCtx.get(), nss.db());
    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, kUuid);
    _testSyncApplyCrudOperation(ErrorCodes::OK, op, false);
}

TEST_F(SyncTailTest, SyncApplyInsertDocumentCollectionMissing) {
    const NamespaceString nss("test.t");
    createDatabase(_opCtx.get(), nss.db());
    // Even though the collection doesn't exist, this is handled in the actual application function,
    // which in the case of this test just ignores such errors. This tests mostly that we don't
    // implicitly create the collection and lock the database in MODE_X.
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, {});
    ASSERT_THROWS(_syncApplyWrapper(_opCtx.get(), &op, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
    ASSERT_FALSE(collectionExists(_opCtx.get(), nss));
}

TEST_F(SyncTailTest, SyncApplyDeleteDocumentCollectionMissing) {
    const NamespaceString nss("test.t");
    createDatabase(_opCtx.get(), nss.db());
    // Even though the collection doesn't exist, this is handled in the actual application function,
    // which in the case of this test just ignores such errors. This tests mostly that we don't
    // implicitly create the collection and lock the database in MODE_X.
    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, {});
    _testSyncApplyCrudOperation(ErrorCodes::OK, op, false);
    ASSERT_FALSE(collectionExists(_opCtx.get(), nss));
}

TEST_F(SyncTailTest, SyncApplyInsertDocumentCollectionExists) {
    const NamespaceString nss("test.t");
    createCollection(_opCtx.get(), nss, {});
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, {});
    _testSyncApplyCrudOperation(ErrorCodes::OK, op, true);
}

TEST_F(SyncTailTest, SyncApplyDeleteDocumentCollectionExists) {
    const NamespaceString nss("test.t");
    createCollection(_opCtx.get(), nss, {});
    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, {});
    _testSyncApplyCrudOperation(ErrorCodes::OK, op, false);
}

TEST_F(SyncTailTest, SyncApplyInsertDocumentCollectionLockedByUUID) {
    const NamespaceString nss("test.t");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    // Test that the collection to lock is determined by the UUID and not the 'ns' field.
    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kInsert, otherNss, uuid);
    _testSyncApplyCrudOperation(ErrorCodes::OK, op, true);
}

TEST_F(SyncTailTest, SyncApplyDeleteDocumentCollectionLockedByUUID) {
    const NamespaceString nss("test.t");
    CollectionOptions options;
    options.uuid = kUuid;
    createCollection(_opCtx.get(), nss, options);

    // Test that the collection to lock is determined by the UUID and not the 'ns' field.
    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, options.uuid);
    _testSyncApplyCrudOperation(ErrorCodes::OK, op, false);
}

TEST_F(SyncTailTest, SyncApplyCommand) {
    NamespaceString nss("test.t");
    auto op =
        BSON("op"
             << "c"
             << "ns" << nss.getCommandNS().ns() << "wall" << Date_t() << "o"
             << BSON("create" << nss.coll()) << "ts" << Timestamp(1, 1) << "ui" << UUID::gen());
    bool applyCmdCalled = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            Collection*,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&) {
        applyCmdCalled = true;
        ASSERT_TRUE(opCtx);
        ASSERT_TRUE(opCtx->lockState()->isDbLockedForMode(nss.db(), MODE_X));
        ASSERT_EQUALS(nss, collNss);
        return Status::OK();
    };
    auto entry = OplogEntry(op);
    ASSERT_OK(_syncApplyWrapper(_opCtx.get(), &entry, OplogApplication::Mode::kInitialSync));
    ASSERT_TRUE(applyCmdCalled);
}

TEST_F(SyncTailTest, MultiSyncApplyUsesSyncApplyToApplyOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss);

    MultiApplier::OperationPtrs ops = {&op};
    WorkerMultikeyPathInfo pathInfo;
    SyncTail syncTail(nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kSecondary));
    ASSERT_OK(multiSyncApply(_opCtx.get(), &ops, &syncTail, &pathInfo));
    // Collection should be created after syncApply() processes operation.
    ASSERT_TRUE(AutoGetCollectionForReadCommand(_opCtx.get(), nss).getCollection());
}

void testWorkerMultikeyPaths(OperationContext* opCtx,
                             const OplogEntry& op,
                             unsigned long numPaths) {
    SyncTail syncTail(nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kSecondary));
    WorkerMultikeyPathInfo pathInfo;
    MultiApplier::OperationPtrs ops = {&op};
    ASSERT_OK(multiSyncApply(opCtx, &ops, &syncTail, &pathInfo));
    ASSERT_EQ(pathInfo.size(), numPaths);
}

TEST_F(SyncTailTest, MultiSyncApplyAddsWorkerMultikeyPathInfoOnInsert) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());

    {
        auto op = makeCreateCollectionOplogEntry(
            {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("uuid" << kUuid));
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }
    {
        auto keyPattern = BSON("a" << 1);
        auto op = makeCreateIndexOplogEntry(
            {Timestamp(Seconds(2), 0), 1LL}, nss, "a_1", keyPattern, kUuid);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }
    {
        auto doc = BSON("_id" << 1 << "a" << BSON_ARRAY(4 << 5));
        auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(3), 0), 1LL}, nss, doc);
        testWorkerMultikeyPaths(_opCtx.get(), op, 1UL);
    }
}

TEST_F(SyncTailTest, MultiSyncApplyAddsMultipleWorkerMultikeyPathInfo) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());

    {
        auto op = makeCreateCollectionOplogEntry(
            {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("uuid" << kUuid));
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }

    {
        auto keyPattern = BSON("a" << 1);
        auto op = makeCreateIndexOplogEntry(
            {Timestamp(Seconds(2), 0), 1LL}, nss, "a_1", keyPattern, kUuid);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }

    {
        auto keyPattern = BSON("b" << 1);
        auto op = makeCreateIndexOplogEntry(
            {Timestamp(Seconds(3), 0), 1LL}, nss, "b_1", keyPattern, kUuid);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }

    {
        auto docA = BSON("_id" << 1 << "a" << BSON_ARRAY(4 << 5));
        auto opA = makeInsertDocumentOplogEntry({Timestamp(Seconds(4), 0), 1LL}, nss, docA);
        auto docB = BSON("_id" << 2 << "b" << BSON_ARRAY(6 << 7));
        auto opB = makeInsertDocumentOplogEntry({Timestamp(Seconds(5), 0), 1LL}, nss, docB);
        SyncTail syncTail(
            nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kSecondary));
        WorkerMultikeyPathInfo pathInfo;
        MultiApplier::OperationPtrs ops = {&opA, &opB};
        ASSERT_OK(multiSyncApply(_opCtx.get(), &ops, &syncTail, &pathInfo));
        ASSERT_EQ(pathInfo.size(), 2UL);
    }
}

TEST_F(SyncTailTest, MultiSyncApplyDoesNotAddWorkerMultikeyPathInfoOnCreateIndex) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());

    {
        auto op = makeCreateCollectionOplogEntry(
            {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("uuid" << kUuid));
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }

    {
        auto doc = BSON("_id" << 1 << "a" << BSON_ARRAY(4 << 5));
        auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss, doc);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }

    {
        auto keyPattern = BSON("a" << 1);
        auto op = makeCreateIndexOplogEntry(
            {Timestamp(Seconds(3), 0), 1LL}, nss, "a_1", keyPattern, kUuid);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }

    {
        auto doc = BSON("_id" << 2 << "a" << BSON_ARRAY(6 << 7));
        auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(4), 0), 1LL}, nss, doc);
        testWorkerMultikeyPaths(_opCtx.get(), op, 0UL);
    }
}

TEST_F(SyncTailTest, MultiSyncApplyFailsWhenCollectionCreationTriesToMakeUUID) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_SECONDARY));
    NamespaceString nss("foo." + _agent.getSuiteName() + "_" + _agent.getTestName());

    auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss);
    SyncTail syncTail(nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kSecondary));
    MultiApplier::OperationPtrs ops = {&op};
    ASSERT_EQUALS(ErrorCodes::InvalidOptions,
                  multiSyncApply(_opCtx.get(), &ops, &syncTail, nullptr));
}

TEST_F(SyncTailTest, MultiSyncApplyDisablesDocumentValidationWhileApplyingOperations) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn =
        [&](OperationContext* opCtx, const NamespaceString&, const std::vector<BSONObj>&) {
            onInsertsCalled = true;
            ASSERT_FALSE(opCtx->writesAreReplicated());
            ASSERT_FALSE(opCtx->lockState()->shouldConflictWithSecondaryBatchApplication());
            ASSERT_TRUE(documentValidationDisabled(opCtx));
            return Status::OK();
        };
    createCollectionWithUuid(_opCtx.get(), nss);
    auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0));
    ASSERT_OK(runOpSteadyState(op));
    ASSERT(onInsertsCalled);
}

TEST_F(SyncTailTest, MultiSyncApplyPassesThroughSyncApplyErrorAfterFailingToApplyOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    // Delete operation without _id in 'o' field.
    auto op = makeDeleteDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, {});
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, runOpSteadyState(op));
}

TEST_F(SyncTailTest, MultiSyncApplyPassesThroughSyncApplyException) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn =
        [&](OperationContext* opCtx, const NamespaceString&, const std::vector<BSONObj>&) {
            onInsertsCalled = true;
            uasserted(ErrorCodes::OperationFailed, "");
            MONGO_UNREACHABLE;
        };
    createCollectionWithUuid(_opCtx.get(), nss);
    auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0));
    ASSERT_EQUALS(ErrorCodes::OperationFailed, runOpSteadyState(op));
    ASSERT(onInsertsCalled);
}

TEST_F(SyncTailTest, MultiSyncApplySortsOperationsStablyByNamespaceBeforeApplying) {
    NamespaceString nss1("test.t1");
    NamespaceString nss2("test.t2");
    NamespaceString nss3("test.t3");

    const Seconds s(1);
    unsigned int i = 1;
    auto op1 = makeInsertDocumentOplogEntry({Timestamp(s, i++), 1LL}, nss1, BSON("_id" << 1));
    auto op2 = makeInsertDocumentOplogEntry({Timestamp(s, i++), 1LL}, nss1, BSON("_id" << 2));
    auto op3 = makeInsertDocumentOplogEntry({Timestamp(s, i++), 1LL}, nss2, BSON("_id" << 3));
    auto op4 = makeInsertDocumentOplogEntry({Timestamp(s, i++), 1LL}, nss3, BSON("_id" << 4));

    std::vector<NamespaceString> nssInserted;
    std::vector<BSONObj> docsInserted;
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            onInsertsCalled = true;
            for (const auto& doc : docs) {
                nssInserted.push_back(nss);
                docsInserted.push_back(doc);
            }
        };

    createCollectionWithUuid(_opCtx.get(), nss1);
    createCollectionWithUuid(_opCtx.get(), nss2);
    createCollectionWithUuid(_opCtx.get(), nss3);

    ASSERT_OK(runOpsSteadyState({op4, op1, op3, op2}));

    ASSERT_EQUALS(4U, nssInserted.size());
    ASSERT_EQUALS(nss1, nssInserted[0]);
    ASSERT_EQUALS(nss1, nssInserted[1]);
    ASSERT_EQUALS(nss2, nssInserted[2]);
    ASSERT_EQUALS(nss3, nssInserted[3]);

    ASSERT_EQUALS(4U, docsInserted.size());
    ASSERT_BSONOBJ_EQ(op1.getObject(), docsInserted[0]);
    ASSERT_BSONOBJ_EQ(op2.getObject(), docsInserted[1]);
    ASSERT_BSONOBJ_EQ(op3.getObject(), docsInserted[2]);
    ASSERT_BSONOBJ_EQ(op4.getObject(), docsInserted[3]);

    ASSERT(onInsertsCalled);
}

TEST_F(SyncTailTest, MultiSyncApplyGroupsInsertOperationByNamespaceBeforeApplying) {
    int seconds = 1;
    auto makeOp = [&seconds](const NamespaceString& nss) {
        return makeInsertDocumentOplogEntry(
            {Timestamp(Seconds(seconds), 0), 1LL}, nss, BSON("_id" << seconds++));
    };
    NamespaceString nss1("test." + _agent.getSuiteName() + "_" + _agent.getTestName() + "_1");
    NamespaceString nss2("test." + _agent.getSuiteName() + "_" + _agent.getTestName() + "_2");
    auto createOp1 = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss1);
    auto createOp2 = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss2);
    auto insertOp1a = makeOp(nss1);
    auto insertOp1b = makeOp(nss1);
    auto insertOp2a = makeOp(nss2);
    auto insertOp2b = makeOp(nss2);

    // Each element in 'docsInserted' is a grouped insert operation.
    std::vector<std::vector<BSONObj>> docsInserted;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            docsInserted.push_back(docs);
        };

    MultiApplier::Operations ops = {
        createOp1, createOp2, insertOp1a, insertOp2a, insertOp1b, insertOp2b};
    ASSERT_OK(runOpsSteadyState(ops));

    ASSERT_EQUALS(2U, docsInserted.size());

    // Check grouped insert operations in namespace "nss1".
    const auto& group1 = docsInserted[0];
    ASSERT_EQUALS(2U, group1.size());
    ASSERT_BSONOBJ_EQ(insertOp1a.getObject(), group1[0]);
    ASSERT_BSONOBJ_EQ(insertOp1b.getObject(), group1[1]);

    // Check grouped insert operations in namespace "nss2".
    const auto& group2 = docsInserted[1];
    ASSERT_EQUALS(2U, group2.size());
    ASSERT_BSONOBJ_EQ(insertOp2a.getObject(), group2[0]);
    ASSERT_BSONOBJ_EQ(insertOp2b.getObject(), group2[1]);
}

TEST_F(SyncTailTest, MultiSyncApplyLimitsBatchCountWhenGroupingInsertOperation) {
    int seconds = 1;
    auto makeOp = [&seconds](const NamespaceString& nss) {
        return makeInsertDocumentOplogEntry(
            {Timestamp(Seconds(seconds), 0), 1LL}, nss, BSON("_id" << seconds++));
    };
    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName() + "_1");
    auto createOp = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss);

    // Generate operations to apply:
    // {create}, {insert_1}, {insert_2}, .. {insert_(limit)}, {insert_(limit+1)}
    std::size_t limit = 64;
    MultiApplier::Operations insertOps;
    for (std::size_t i = 0; i < limit + 1; ++i) {
        insertOps.push_back(makeOp(nss));
    }
    MultiApplier::Operations operationsToApply;
    operationsToApply.push_back(createOp);
    std::copy(insertOps.begin(), insertOps.end(), std::back_inserter(operationsToApply));

    // Each element in 'docsInserted' is a grouped insert operation.
    std::vector<std::vector<BSONObj>> docsInserted;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            docsInserted.push_back(docs);
        };

    ASSERT_OK(runOpsSteadyState(operationsToApply));

    // multiSyncApply should combine operations as follows:
    // {create}, {grouped_insert}, {insert_(limit+1)}
    // Ignore {create} since we are only tracking inserts.
    ASSERT_EQUALS(2U, docsInserted.size());

    const auto& groupedInsertDocuments = docsInserted[0];
    ASSERT_EQUALS(limit, groupedInsertDocuments.size());
    for (std::size_t i = 0; i < limit; ++i) {
        const auto& insertOp = insertOps[i];
        ASSERT_BSONOBJ_EQ(insertOp.getObject(), groupedInsertDocuments[i]);
    }

    // (limit + 1)-th insert operations should not be included in group of first (limit) inserts.
    const auto& singleInsertDocumentGroup = docsInserted[1];
    ASSERT_EQUALS(1U, singleInsertDocumentGroup.size());
    ASSERT_BSONOBJ_EQ(insertOps.back().getObject(), singleInsertDocumentGroup[0]);
}

// Create an 'insert' oplog operation of an approximate size in bytes. The '_id' of the oplog entry
// and its optime in seconds are given by the 'id' argument.
OplogEntry makeSizedInsertOp(const NamespaceString& nss, int size, int id) {
    return makeInsertDocumentOplogEntry({Timestamp(Seconds(id), 0), 1LL},
                                        nss,
                                        BSON("_id" << id << "data" << std::string(size, '*')));
};

TEST_F(SyncTailTest, MultiSyncApplyLimitsBatchSizeWhenGroupingInsertOperations) {
    int seconds = 1;
    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto createOp = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss);

    // Create a sequence of insert ops that are too large to fit in one group.
    int maxBatchSize = write_ops::insertVectorMaxBytes;
    int opsPerBatch = 3;
    int opSize = maxBatchSize / opsPerBatch - 500;  // Leave some room for other oplog fields.

    // Create the insert ops.
    MultiApplier::Operations insertOps;
    int numOps = 4;
    for (int i = 0; i < numOps; i++) {
        insertOps.push_back(makeSizedInsertOp(nss, opSize, seconds++));
    }

    MultiApplier::Operations operationsToApply;
    operationsToApply.push_back(createOp);
    std::copy(insertOps.begin(), insertOps.end(), std::back_inserter(operationsToApply));

    // Each element in 'docsInserted' is a grouped insert operation.
    std::vector<std::vector<BSONObj>> docsInserted;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            docsInserted.push_back(docs);
        };

    // Apply the ops.
    ASSERT_OK(runOpsSteadyState(operationsToApply));

    // Applied ops should be as follows:
    // [ {create}, INSERT_GROUP{insert 1, insert 2, insert 3}, {insert 4} ]
    // Ignore {create} since we are only tracking inserts.
    ASSERT_EQUALS(2U, docsInserted.size());

    // Make sure the insert group was created correctly.
    const auto& groupedInsertOpArray = docsInserted[0];
    ASSERT_EQUALS(std::size_t(opsPerBatch), groupedInsertOpArray.size());
    for (int i = 0; i < opsPerBatch; ++i) {
        ASSERT_BSONOBJ_EQ(insertOps[i].getObject(), groupedInsertOpArray[i]);
    }

    // Check that the last op was applied individually.
    const auto& singleInsertDocumentGroup = docsInserted[1];
    ASSERT_EQUALS(1U, singleInsertDocumentGroup.size());
    ASSERT_BSONOBJ_EQ(insertOps[3].getObject(), singleInsertDocumentGroup[0]);
}

TEST_F(SyncTailTest, MultiSyncApplyAppliesOpIndividuallyWhenOpIndividuallyExceedsBatchSize) {
    int seconds = 1;
    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto createOp = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss);

    int maxBatchSize = write_ops::insertVectorMaxBytes;
    // Create an insert op that exceeds the maximum batch size by itself.
    auto insertOpLarge = makeSizedInsertOp(nss, maxBatchSize, seconds++);
    auto insertOpSmall = makeSizedInsertOp(nss, 100, seconds++);

    MultiApplier::Operations operationsToApply = {createOp, insertOpLarge, insertOpSmall};

    // Each element in 'docsInserted' is a grouped insert operation.
    std::vector<std::vector<BSONObj>> docsInserted;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            docsInserted.push_back(docs);
        };

    // Apply the ops.
    ASSERT_OK(runOpsSteadyState(operationsToApply));

    // Applied ops should be as follows:
    // [ {create}, {large insert} {small insert} ]
    // Ignore {create} since we are only tracking inserts.
    ASSERT_EQUALS(2U, docsInserted.size());

    ASSERT_EQUALS(1U, docsInserted[0].size());
    ASSERT_BSONOBJ_EQ(insertOpLarge.getObject(), docsInserted[0][0]);

    ASSERT_EQUALS(1U, docsInserted[1].size());
    ASSERT_BSONOBJ_EQ(insertOpSmall.getObject(), docsInserted[1][0]);
}

TEST_F(SyncTailTest, MultiSyncApplyAppliesInsertOpsIndividuallyWhenUnableToCreateGroupByNamespace) {
    int seconds = 1;
    auto makeOp = [&seconds](const NamespaceString& nss) {
        return makeInsertDocumentOplogEntry(
            {Timestamp(Seconds(seconds), 0), 1LL}, nss, BSON("_id" << seconds++));
    };

    auto testNs = "test." + _agent.getSuiteName() + "_" + _agent.getTestName();

    // Create a sequence of 3 'insert' ops that can't be grouped because they are from different
    // namespaces.
    MultiApplier::Operations operationsToApply = {makeOp(NamespaceString(testNs + "_1")),
                                                  makeOp(NamespaceString(testNs + "_2")),
                                                  makeOp(NamespaceString(testNs + "_3"))};

    for (const auto& oplogEntry : operationsToApply) {
        createCollectionWithUuid(_opCtx.get(), oplogEntry.getNss());
    }

    // Each element in 'docsInserted' is a grouped insert operation.
    std::vector<std::vector<BSONObj>> docsInserted;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            docsInserted.push_back(docs);
        };

    // Apply the ops.
    ASSERT_OK(runOpsSteadyState(operationsToApply));

    // Applied ops should be as follows i.e. no insert grouping:
    // [{insert 1}, {insert 2}, {insert 3}]
    ASSERT_EQ(operationsToApply.size(), docsInserted.size());
    for (std::size_t i = 0; i < operationsToApply.size(); i++) {
        const auto& group = docsInserted[i];
        ASSERT_EQUALS(1U, group.size()) << i;
        ASSERT_BSONOBJ_EQ(operationsToApply[i].getObject(), group[0]);
    }
}

TEST_F(SyncTailTest, MultiSyncApplyFallsBackOnApplyingInsertsIndividuallyWhenGroupedInsertFails) {
    int seconds = 1;
    auto makeOp = [&seconds](const NamespaceString& nss) {
        return makeInsertDocumentOplogEntry(
            {Timestamp(Seconds(seconds), 0), 1LL}, nss, BSON("_id" << seconds++));
    };
    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName() + "_1");
    auto createOp = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss);

    // Generate operations to apply:
    // {create}, {insert_1}, {insert_2}, .. {insert_(limit)}, {insert_(limit+1)}
    std::size_t limit = 64;
    MultiApplier::Operations insertOps;
    for (std::size_t i = 0; i < limit + 1; ++i) {
        insertOps.push_back(makeOp(nss));
    }
    MultiApplier::Operations operationsToApply;
    operationsToApply.push_back(createOp);
    std::copy(insertOps.begin(), insertOps.end(), std::back_inserter(operationsToApply));

    // Each element in 'docsInserted' is a grouped insert operation.
    std::vector<std::vector<BSONObj>> docsInserted;
    std::size_t numFailedGroupedInserts = 0;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            // Reject grouped insert operations.
            if (docs.size() > 1U) {
                numFailedGroupedInserts++;
                uasserted(ErrorCodes::OperationFailed, "grouped inserts not supported");
            }
            docsInserted.push_back(docs);
        };

    ASSERT_OK(runOpsSteadyState(operationsToApply));

    // On failing to apply the grouped insert operation, multiSyncApply should apply the operations
    // as given in "operationsToApply":
    // {create}, {insert_1}, {insert_2}, .. {insert_(limit)}, {insert_(limit+1)}
    // Ignore {create} since we are only tracking inserts.
    ASSERT_EQUALS(limit + 1, docsInserted.size());

    for (std::size_t i = 0; i < limit + 1; ++i) {
        const auto& insertOp = insertOps[i];
        const auto& group = docsInserted[i];
        ASSERT_EQUALS(1U, group.size()) << i;
        ASSERT_BSONOBJ_EQ(insertOp.getObject(), group[0]);
    }

    // Ensure that multiSyncApply does not attempt to group remaining operations in first failed
    // grouped insert operation.
    ASSERT_EQUALS(1U, numFailedGroupedInserts);
}

TEST_F(SyncTailTest, MultiSyncApplyIgnoresUpdateOperationIfDocumentIsMissingFromSyncSource) {
    SyncTailForTest syncTail;
    NamespaceString nss("test.t");
    {
        Lock::GlobalWrite globalLock(_opCtx.get());
        bool justCreated = false;
        auto databaseHolder = DatabaseHolder::get(_opCtx.get());
        auto db = databaseHolder->openDb(_opCtx.get(), nss.db(), &justCreated);
        ASSERT_TRUE(db);
        ASSERT_TRUE(justCreated);
    }
    auto op = makeUpdateDocumentOplogEntry(
        {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0), BSON("_id" << 0 << "x" << 2));
    MultiApplier::OperationPtrs ops = {&op};
    WorkerMultikeyPathInfo pathInfo;
    ASSERT_OK(multiSyncApply(_opCtx.get(), &ops, &syncTail, &pathInfo));

    // Since the document was missing when we cloned data from the sync source, the collection
    // referenced by the failed operation should not be automatically created.
    ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx.get(), nss).getCollection());
}

TEST_F(SyncTailTest, MultiSyncApplySkipsDocumentOnNamespaceNotFoundDuringInitialSync) {
    BSONObj emptyDoc;
    SyncTailForTest syncTail;
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    NamespaceString badNss("local." + _agent.getSuiteName() + "_" + _agent.getTestName() + "bad");
    auto doc1 = BSON("_id" << 1);
    auto doc2 = BSON("_id" << 2);
    auto doc3 = BSON("_id" << 3);
    auto op0 = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss);
    auto op1 = makeInsertDocumentOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss, doc1);
    auto op2 = makeInsertDocumentOplogEntry({Timestamp(Seconds(3), 0), 1LL}, badNss, doc2);
    auto op3 = makeInsertDocumentOplogEntry({Timestamp(Seconds(4), 0), 1LL}, nss, doc3);
    MultiApplier::OperationPtrs ops = {&op0, &op1, &op2, &op3};
    WorkerMultikeyPathInfo pathInfo;
    ASSERT_OK(multiSyncApply(_opCtx.get(), &ops, &syncTail, &pathInfo));

    CollectionReader collectionReader(_opCtx.get(), nss);
    ASSERT_BSONOBJ_EQ(doc1, unittest::assertGet(collectionReader.next()));
    ASSERT_BSONOBJ_EQ(doc3, unittest::assertGet(collectionReader.next()));
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, collectionReader.next().getStatus());
}

TEST_F(SyncTailTest, MultiSyncApplySkipsIndexCreationOnNamespaceNotFoundDuringInitialSync) {
    BSONObj emptyDoc;
    SyncTailForTest syncTail;
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    NamespaceString badNss("local." + _agent.getSuiteName() + "_" + _agent.getTestName() + "bad");
    auto doc1 = BSON("_id" << 1);
    auto keyPattern = BSON("a" << 1);
    auto doc3 = BSON("_id" << 3);
    auto op0 =
        makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, BSON("uuid" << kUuid));
    auto op1 = makeInsertDocumentOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss, doc1);
    auto op2 = makeCreateIndexOplogEntry(
        {Timestamp(Seconds(3), 0), 1LL}, badNss, "a_1", keyPattern, kUuid);
    auto op3 = makeInsertDocumentOplogEntry({Timestamp(Seconds(4), 0), 1LL}, nss, doc3);
    MultiApplier::OperationPtrs ops = {&op0, &op1, &op2, &op3};
    WorkerMultikeyPathInfo pathInfo;
    ASSERT_OK(multiSyncApply(_opCtx.get(), &ops, &syncTail, &pathInfo));

    CollectionReader collectionReader(_opCtx.get(), nss);
    ASSERT_BSONOBJ_EQ(doc1, unittest::assertGet(collectionReader.next()));
    ASSERT_BSONOBJ_EQ(doc3, unittest::assertGet(collectionReader.next()));
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, collectionReader.next().getStatus());

    // 'badNss' collection should not be implicitly created while attempting to create an index.
    ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx.get(), badNss).getCollection());
}

TEST_F(IdempotencyTest, Geo2dsphereIndexFailedOnUpdate) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));
    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto insertOp = insert(fromjson("{_id: 1, loc: 'hi'}"));
    auto updateOp = update(1, fromjson("{$set: {loc: [1, 2]}}"));
    auto indexOp =
        buildIndex(fromjson("{loc: '2dsphere'}"), BSON("2dsphereIndexVersion" << 3), kUuid);

    auto ops = {insertOp, updateOp, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 16755);
}

TEST_F(IdempotencyTest, Geo2dsphereIndexFailedOnIndexing) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));
    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto indexOp =
        buildIndex(fromjson("{loc: '2dsphere'}"), BSON("2dsphereIndexVersion" << 3), kUuid);
    auto dropIndexOp = dropIndex("loc_index", kUuid);
    auto insertOp = insert(fromjson("{_id: 1, loc: 'hi'}"));

    auto ops = {indexOp, dropIndexOp, insertOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 16755);
}

TEST_F(IdempotencyTest, Geo2dIndex) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));
    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto insertOp = insert(fromjson("{_id: 1, loc: [1]}"));
    auto updateOp = update(1, fromjson("{$set: {loc: [1, 2]}}"));
    auto indexOp = buildIndex(fromjson("{loc: '2d'}"), BSONObj(), kUuid);

    auto ops = {insertOp, updateOp, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 13068);
}

TEST_F(IdempotencyTest, UniqueKeyIndex) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));
    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto insertOp = insert(fromjson("{_id: 1, x: 5}"));
    auto updateOp = update(1, fromjson("{$set: {x: 6}}"));
    auto insertOp2 = insert(fromjson("{_id: 2, x: 5}"));
    auto indexOp = buildIndex(fromjson("{x: 1}"), fromjson("{unique: true}"), kUuid);

    auto ops = {insertOp, updateOp, insertOp2, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), ErrorCodes::DuplicateKey);
}

TEST_F(IdempotencyTest, ParallelArrayError) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    ASSERT_OK(runOpInitialSync(insert(fromjson("{_id: 1}"))));

    auto updateOp1 = update(1, fromjson("{$set: {x: [1, 2]}}"));
    auto updateOp2 = update(1, fromjson("{$set: {x: 1}}"));
    auto updateOp3 = update(1, fromjson("{$set: {y: [3, 4]}}"));
    auto indexOp = buildIndex(fromjson("{x: 1, y: 1}"), BSONObj(), kUuid);

    auto ops = {updateOp1, updateOp2, updateOp3, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), ErrorCodes::CannotIndexParallelArrays);
}

TEST_F(IdempotencyTest, IndexWithDifferentOptions) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    ASSERT_OK(runOpInitialSync(insert(fromjson("{_id: 1, x: 'hi'}"))));

    auto indexOp1 =
        buildIndex(fromjson("{x: 'text'}"), fromjson("{default_language: 'spanish'}"), kUuid);
    auto dropIndexOp = dropIndex("x_index", kUuid);
    auto indexOp2 =
        buildIndex(fromjson("{x: 'text'}"), fromjson("{default_language: 'english'}"), kUuid);

    auto ops = {indexOp1, dropIndexOp, indexOp2};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), ErrorCodes::IndexOptionsConflict);
}

TEST_F(IdempotencyTest, TextIndexDocumentHasNonStringLanguageField) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto insertOp = insert(fromjson("{_id: 1, x: 'words to index', language: 1}"));
    auto updateOp = update(1, fromjson("{$unset: {language: 1}}"));
    auto indexOp = buildIndex(fromjson("{x: 'text'}"), BSONObj(), kUuid);

    auto ops = {insertOp, updateOp, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 17261);
}

TEST_F(IdempotencyTest, InsertDocumentWithNonStringLanguageFieldWhenTextIndexExists) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto indexOp = buildIndex(fromjson("{x: 'text'}"), BSONObj(), kUuid);
    auto dropIndexOp = dropIndex("x_index", kUuid);
    auto insertOp = insert(fromjson("{_id: 1, x: 'words to index', language: 1}"));

    auto ops = {indexOp, dropIndexOp, insertOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 17261);
}

TEST_F(IdempotencyTest, TextIndexDocumentHasNonStringLanguageOverrideField) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto insertOp = insert(fromjson("{_id: 1, x: 'words to index', y: 1}"));
    auto updateOp = update(1, fromjson("{$unset: {y: 1}}"));
    auto indexOp = buildIndex(fromjson("{x: 'text'}"), fromjson("{language_override: 'y'}"), kUuid);

    auto ops = {insertOp, updateOp, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 17261);
}

TEST_F(IdempotencyTest, InsertDocumentWithNonStringLanguageOverrideFieldWhenTextIndexExists) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto indexOp = buildIndex(fromjson("{x: 'text'}"), fromjson("{language_override: 'y'}"), kUuid);
    auto dropIndexOp = dropIndex("x_index", kUuid);
    auto insertOp = insert(fromjson("{_id: 1, x: 'words to index', y: 1}"));

    auto ops = {indexOp, dropIndexOp, insertOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 17261);
}

TEST_F(IdempotencyTest, TextIndexDocumentHasUnknownLanguage) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto insertOp = insert(fromjson("{_id: 1, x: 'words to index', language: 'bad'}"));
    auto updateOp = update(1, fromjson("{$unset: {language: 1}}"));
    auto indexOp = buildIndex(fromjson("{x: 'text'}"), BSONObj(), kUuid);

    auto ops = {insertOp, updateOp, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 17262);
}

TEST_F(IdempotencyTest, CreateCollectionWithValidation) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));
    const BSONObj uuidObj = kUuid.toBSON();

    auto runOpsAndValidate = [this, uuidObj]() {
        auto options1 = fromjson("{'validator' : {'phone' : {'$type' : 'string' } } }");
        options1 = options1.addField(uuidObj.firstElement());
        auto createColl1 = makeCreateCollectionOplogEntry(nextOpTime(), nss, options1);
        auto dropColl = makeCommandOplogEntry(nextOpTime(), nss, BSON("drop" << nss.coll()));

        auto options2 = fromjson("{'validator' : {'phone' : {'$type' : 'number' } } }");
        options2 = options2.addField(uuidObj.firstElement());
        auto createColl2 = makeCreateCollectionOplogEntry(nextOpTime(), nss, options2);

        auto ops = {createColl1, dropColl, createColl2};
        ASSERT_OK(runOpsInitialSync(ops));
        auto state = validate();

        return state;
    };

    auto state1 = runOpsAndValidate();
    auto state2 = runOpsAndValidate();
    ASSERT_EQUALS(state1, state2);
}

TEST_F(IdempotencyTest, CreateCollectionWithCollation) {
    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));
    CollectionUUID uuid = UUID::gen();

    auto runOpsAndValidate = [this, uuid]() {
        auto insertOp1 = insert(fromjson("{ _id: 'foo' }"));
        auto insertOp2 = insert(fromjson("{ _id: 'Foo', x: 1 }"));
        auto updateOp = update("foo", BSON("$set" << BSON("x" << 2)));
        auto dropColl = makeCommandOplogEntry(nextOpTime(), nss, BSON("drop" << nss.coll()));
        auto options = BSON("collation"
                            << BSON("locale"
                                    << "en"
                                    << "caseLevel" << false << "caseFirst"
                                    << "off"
                                    << "strength" << 1 << "numericOrdering" << false << "alternate"
                                    << "non-ignorable"
                                    << "maxVariable"
                                    << "punct"
                                    << "normalization" << false << "backwards" << false << "version"
                                    << "57.1")
                            << "uuid" << uuid);
        auto createColl = makeCreateCollectionOplogEntry(nextOpTime(), nss, options);

        // We don't drop and re-create the collection since we don't have ways
        // to wait until second-phase drop to completely finish.
        auto ops = {createColl, insertOp1, insertOp2, updateOp};
        ASSERT_OK(runOpsInitialSync(ops));
        auto state = validate();

        return state;
    };

    auto state1 = runOpsAndValidate();
    auto state2 = runOpsAndValidate();
    ASSERT_EQUALS(state1, state2);
}

TEST_F(IdempotencyTest, CreateCollectionWithIdIndex) {
    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));
    CollectionUUID uuid = kUuid;

    auto options1 = BSON("idIndex" << BSON("key" << fromjson("{_id: 1}") << "name"
                                                 << "_id_"
                                                 << "v" << 2)
                                   << "uuid" << uuid);
    auto createColl1 = makeCreateCollectionOplogEntry(nextOpTime(), nss, options1);
    ASSERT_OK(runOpInitialSync(createColl1));

    auto runOpsAndValidate = [this, uuid]() {
        auto insertOp = insert(BSON("_id" << Decimal128(1)));
        auto dropColl = makeCommandOplogEntry(nextOpTime(), nss, BSON("drop" << nss.coll()));
        auto createColl2 = createCollection(uuid);

        auto ops = {insertOp, dropColl, createColl2};
        ASSERT_OK(runOpsInitialSync(ops));
        auto state = validate();

        return state;
    };

    auto state1 = runOpsAndValidate();
    auto state2 = runOpsAndValidate();
    ASSERT_EQUALS(state1, state2);
}

TEST_F(IdempotencyTest, CreateCollectionWithView) {
    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));
    CollectionOptions options;
    options.uuid = kUuid;

    // Create data collection
    ASSERT_OK(runOpInitialSync(createCollection()));
    // Create "system.views" collection
    auto viewNss = NamespaceString(nss.db(), "system.views");
    ASSERT_OK(
        runOpInitialSync(makeCreateCollectionOplogEntry(nextOpTime(), viewNss, options.toBSON())));

    auto viewDoc = BSON("_id" << NamespaceString(nss.db(), "view").ns() << "viewOn" << nss.coll()
                              << "pipeline" << fromjson("[ { '$project' : { 'x' : 1 } } ]"));
    auto insertViewOp = makeInsertDocumentOplogEntry(nextOpTime(), viewNss, viewDoc);
    auto dropColl = makeCommandOplogEntry(nextOpTime(), nss, BSON("drop" << nss.coll()));

    auto ops = {insertViewOp, dropColl};
    testOpsAreIdempotent(ops);
}

TEST_F(IdempotencyTest, CollModNamespaceNotFound) {
    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    ASSERT_OK(runOpInitialSync(
        buildIndex(BSON("createdAt" << 1), BSON("expireAfterSeconds" << 3600), kUuid)));

    auto indexChange = fromjson("{keyPattern: {createdAt:1}, expireAfterSeconds:4000}}");
    auto collModCmd = BSON("collMod" << nss.coll() << "index" << indexChange);
    auto collModOp = makeCommandOplogEntry(nextOpTime(), nss, collModCmd, kUuid);
    auto dropCollOp = makeCommandOplogEntry(nextOpTime(), nss, BSON("drop" << nss.coll()), kUuid);

    auto ops = {collModOp, dropCollOp};
    testOpsAreIdempotent(ops);
}

TEST_F(IdempotencyTest, CollModIndexNotFound) {
    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    ASSERT_OK(runOpInitialSync(
        buildIndex(BSON("createdAt" << 1), BSON("expireAfterSeconds" << 3600), kUuid)));

    auto indexChange = fromjson("{keyPattern: {createdAt:1}, expireAfterSeconds:4000}}");
    auto collModCmd = BSON("collMod" << nss.coll() << "index" << indexChange);
    auto collModOp = makeCommandOplogEntry(nextOpTime(), nss, collModCmd, kUuid);
    auto dropIndexOp = dropIndex("createdAt_index", kUuid);

    auto ops = {collModOp, dropIndexOp};
    testOpsAreIdempotent(ops);
}

TEST_F(SyncTailTest, FailOnDropFCVCollection) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto fcvNS(NamespaceString::kServerConfigurationNamespace);
    auto cmd = BSON("drop" << fcvNS.coll());
    auto op = makeCommandOplogEntry(nextOpTime(), fcvNS, cmd);
    ASSERT_EQUALS(runOpInitialSync(op), ErrorCodes::OplogOperationUnsupported);
}

TEST_F(SyncTailTest, FailOnInsertFCVDocument) {
    auto fcvNS(NamespaceString::kServerConfigurationNamespace);
    ::mongo::repl::createCollection(_opCtx.get(), fcvNS, CollectionOptions());
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto op = makeInsertDocumentOplogEntry(
        nextOpTime(), fcvNS, BSON("_id" << FeatureCompatibilityVersionParser::kParameterName));
    ASSERT_EQUALS(runOpInitialSync(op), ErrorCodes::OplogOperationUnsupported);
}

TEST_F(IdempotencyTest, InsertToFCVCollectionBesidesFCVDocumentSucceeds) {
    auto fcvNS(NamespaceString::kServerConfigurationNamespace);
    ::mongo::repl::createCollection(_opCtx.get(), fcvNS, CollectionOptions());
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto op = makeInsertDocumentOplogEntry(nextOpTime(),
                                           fcvNS,
                                           BSON("_id"
                                                << "other"));
    ASSERT_OK(runOpInitialSync(op));
}

TEST_F(IdempotencyTest, DropDatabaseSucceeds) {
    // Choose `system.profile` so the storage engine doesn't expect the drop to be timestamped.
    auto ns = NamespaceString("foo.system.profile");
    ::mongo::repl::createCollection(_opCtx.get(), ns, CollectionOptions());
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto op = makeCommandOplogEntry(nextOpTime(), ns, BSON("dropDatabase" << 1));
    ASSERT_OK(runOpInitialSync(op));
}

TEST_F(SyncTailTest, DropDatabaseSucceedsInRecovering) {
    // Choose `system.profile` so the storage engine doesn't expect the drop to be timestamped.
    auto ns = NamespaceString("foo.system.profile");
    ::mongo::repl::createCollection(_opCtx.get(), ns, CollectionOptions());
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto op = makeCommandOplogEntry(nextOpTime(), ns, BSON("dropDatabase" << 1));
    ASSERT_OK(runOpSteadyState(op));
}

TEST_F(SyncTailTest, LogSlowOpApplicationWhenSuccessful) {
    // This duration is greater than "slowMS", so the op would be considered slow.
    auto applyDuration = serverGlobalParams.slowMS * 10;
    getServiceContext()->setFastClockSource(
        std::make_unique<AutoAdvancingClockSourceMock>(Milliseconds(applyDuration)));

    // We are inserting into an existing collection.
    const NamespaceString nss("test.t");
    createCollection(_opCtx.get(), nss, {});
    auto entry = makeOplogEntry(OpTypeEnum::kInsert, nss, {});

    startCapturingLogMessages();
    ASSERT_OK(_syncApplyWrapper(_opCtx.get(), &entry, OplogApplication::Mode::kSecondary));

    // Use a builder for easier escaping. We expect the operation to be logged.
    StringBuilder expected;
    expected << "applied op: CRUD { ts: Timestamp(1, 1), t: 1, v: 2, op: \"i\", ns: \"test.t\", "
                "wall: new Date(0), o: "
                "{ _id: 0 } }, took "
             << applyDuration << "ms";
    ASSERT_EQUALS(1, countLogLinesContaining(expected.str()));
}

TEST_F(SyncTailTest, DoNotLogSlowOpApplicationWhenFailed) {
    // This duration is greater than "slowMS", so the op would be considered slow.
    auto applyDuration = serverGlobalParams.slowMS * 10;
    getServiceContext()->setFastClockSource(
        std::make_unique<AutoAdvancingClockSourceMock>(Milliseconds(applyDuration)));

    // We are trying to insert into a non-existing database.
    NamespaceString nss("test.t");
    auto entry = makeOplogEntry(OpTypeEnum::kInsert, nss, {});

    startCapturingLogMessages();
    ASSERT_THROWS(_syncApplyWrapper(_opCtx.get(), &entry, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);

    // Use a builder for easier escaping. We expect the operation to *not* be logged
    // even thought it was slow, since we couldn't apply it successfully.
    StringBuilder expected;
    expected << "applied op: CRUD { op: \"i\", ns: \"test.t\", o: { _id: 0 }, ts: Timestamp(1, 1), "
                "t: 1, h: 1, v: 2 }, took "
             << applyDuration << "ms";
    ASSERT_EQUALS(0, countLogLinesContaining(expected.str()));
}

TEST_F(SyncTailTest, DoNotLogNonSlowOpApplicationWhenSuccessful) {
    // This duration is below "slowMS", so the op would *not* be considered slow.
    auto applyDuration = serverGlobalParams.slowMS / 10;
    getServiceContext()->setFastClockSource(
        std::make_unique<AutoAdvancingClockSourceMock>(Milliseconds(applyDuration)));

    // We are inserting into an existing collection.
    const NamespaceString nss("test.t");
    createCollection(_opCtx.get(), nss, {});
    auto entry = makeOplogEntry(OpTypeEnum::kInsert, nss, {});

    startCapturingLogMessages();
    ASSERT_OK(_syncApplyWrapper(_opCtx.get(), &entry, OplogApplication::Mode::kSecondary));

    // Use a builder for easier escaping. We expect the operation to *not* be logged,
    // since it wasn't slow to apply.
    StringBuilder expected;
    expected << "applied op: CRUD { op: \"i\", ns: \"test.t\", o: { _id: 0 }, ts: Timestamp(1, 1), "
                "t: 1, h: 1, v: 2 }, took "
             << applyDuration << "ms";
    ASSERT_EQUALS(0, countLogLinesContaining(expected.str()));
}

TEST_F(IdempotencyTest, EmptyCappedNamespaceNotFound) {
    // Create a BSON "emptycapped" command.
    auto emptyCappedCmd = BSON("emptycapped" << nss.coll());

    // Create an "emptycapped" oplog entry.
    auto emptyCappedOp = makeCommandOplogEntry(nextOpTime(), nss, emptyCappedCmd);

    // Ensure that NamespaceNotFound is acceptable.
    ASSERT_OK(runOpInitialSync(emptyCappedOp));

    AutoGetCollectionForReadCommand autoColl(_opCtx.get(), nss);

    // Ensure that autoColl.getCollection() and autoColl.getDb() are both null.
    ASSERT_FALSE(autoColl.getCollection());
    ASSERT_FALSE(autoColl.getDb());
}

TEST_F(IdempotencyTest, ConvertToCappedNamespaceNotFound) {
    // Create a BSON "convertToCapped" command.
    auto convertToCappedCmd = BSON("convertToCapped" << nss.coll());

    // Create a "convertToCapped" oplog entry.
    auto convertToCappedOp = makeCommandOplogEntry(nextOpTime(), nss, convertToCappedCmd);

    // Ensure that NamespaceNotFound is acceptable.
    ASSERT_OK(runOpInitialSync(convertToCappedOp));

    AutoGetCollectionForReadCommand autoColl(_opCtx.get(), nss);

    // Ensure that autoColl.getCollection() and autoColl.getDb() are both null.
    ASSERT_FALSE(autoColl.getCollection());
    ASSERT_FALSE(autoColl.getDb());
}

class IdempotencyTestTxns : public IdempotencyTest {};

// Document used by transaction idempotency tests.
const BSONObj doc = fromjson("{_id: 1}");
const BSONObj doc2 = fromjson("{_id: 2}");

TEST_F(IdempotencyTestTxns, CommitUnpreparedTransaction) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto commitOp = commitUnprepared(
        lsid, txnNum, StmtId(0), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({commitOp});
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        commitOp.getOpTime(),
                        commitOp.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kCommitted);
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc));
}

TEST_F(IdempotencyTestTxns, CommitUnpreparedTransactionDataPartiallyApplied) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    NamespaceString nss2("test.coll2");
    auto uuid2 = createCollectionWithUuid(_opCtx.get(), nss2);

    auto commitOp = commitUnprepared(lsid,
                                     txnNum,
                                     StmtId(0),
                                     BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)
                                                << makeInsertApplyOpsEntry(nss2, uuid2, doc)));

    // Manually insert one of the documents so that the data will partially reflect the transaction
    // when the commitTransaction oplog entry is applied during initial sync.
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(),
                                                    nss,
                                                    {doc, commitOp.getOpTime().getTimestamp()},
                                                    commitOp.getOpTime().getTerm()));
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc));
    ASSERT_FALSE(docExists(_opCtx.get(), nss2, doc));

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({commitOp});
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        commitOp.getOpTime(),
                        commitOp.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kCommitted);
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), nss2, doc));
}

TEST_F(IdempotencyTestTxns, CommitPreparedTransaction) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto prepareOp =
        prepare(lsid, txnNum, StmtId(0), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));

    auto commitOp = commitPrepared(lsid, txnNum, StmtId(1), prepareOp.getOpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({prepareOp, commitOp});
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        commitOp.getOpTime(),
                        commitOp.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kCommitted);
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc));
}

TEST_F(IdempotencyTestTxns, CommitPreparedTransactionDataPartiallyApplied) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    NamespaceString nss2("test.coll2");
    auto uuid2 = createCollectionWithUuid(_opCtx.get(), nss2);

    auto prepareOp = prepare(lsid,
                             txnNum,
                             StmtId(0),
                             BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)
                                        << makeInsertApplyOpsEntry(nss2, uuid2, doc)));

    auto commitOp = commitPrepared(lsid, txnNum, StmtId(1), prepareOp.getOpTime());

    // Manually insert one of the documents so that the data will partially reflect the transaction
    // when the commitTransaction oplog entry is applied during initial sync.
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(),
                                                    nss,
                                                    {doc, commitOp.getOpTime().getTimestamp()},
                                                    commitOp.getOpTime().getTerm()));
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc));
    ASSERT_FALSE(docExists(_opCtx.get(), nss2, doc));

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({prepareOp, commitOp});
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        commitOp.getOpTime(),
                        commitOp.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kCommitted);
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), nss2, doc));
}

TEST_F(IdempotencyTestTxns, AbortPreparedTransaction) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto prepareOp =
        prepare(lsid, txnNum, StmtId(0), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));
    auto abortOp = abortPrepared(lsid, txnNum, StmtId(1), prepareOp.getOpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({prepareOp, abortOp});
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        abortOp.getOpTime(),
                        abortOp.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kAborted);
    ASSERT_FALSE(docExists(_opCtx.get(), nss, doc));
}

TEST_F(IdempotencyTestTxns, SinglePartialTxnOp) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({partialOp});
    auto expectedStartOpTime = partialOp.getOpTime();
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        partialOp.getOpTime(),
                        partialOp.getWallClockTime(),
                        expectedStartOpTime,
                        DurableTxnStateEnum::kInProgress);

    // Document should not be visible yet.
    ASSERT_FALSE(docExists(_opCtx.get(), nss, doc));
}

TEST_F(IdempotencyTestTxns, MultiplePartialTxnOps) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp1 = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));
    auto partialOp2 = partialTxn(lsid,
                                 txnNum,
                                 StmtId(1),
                                 partialOp1.getOpTime(),
                                 BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc2)));
    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({partialOp1, partialOp2});
    auto expectedStartOpTime = partialOp1.getOpTime();
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        partialOp1.getOpTime(),
                        partialOp1.getWallClockTime(),
                        expectedStartOpTime,
                        DurableTxnStateEnum::kInProgress);
    // Document should not be visible yet.
    ASSERT_FALSE(docExists(_opCtx.get(), nss, doc));
    ASSERT_FALSE(docExists(_opCtx.get(), nss, doc2));
}

TEST_F(IdempotencyTestTxns, CommitUnpreparedTransactionWithPartialTxnOps) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));

    auto commitOp = commitUnprepared(lsid,
                                     txnNum,
                                     StmtId(1),
                                     BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc2)),
                                     partialOp.getOpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({partialOp, commitOp});
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        commitOp.getOpTime(),
                        commitOp.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kCommitted);
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc2));
}

TEST_F(IdempotencyTestTxns, CommitTwoUnpreparedTransactionsWithPartialTxnOpsAtOnce) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum1(1);
    TxnNumber txnNum2(2);

    auto partialOp1 = partialTxn(
        lsid, txnNum1, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));
    auto commitOp1 =
        commitUnprepared(lsid, txnNum1, StmtId(1), BSONArray(), partialOp1.getOpTime());

    // The second transaction (with a different transaction number) in the same session.
    auto partialOp2 = partialTxn(
        lsid, txnNum2, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc2)));
    auto commitOp2 =
        commitUnprepared(lsid, txnNum2, StmtId(1), BSONArray(), partialOp2.getOpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    // This also tests that we clear the partialTxnList for the session after applying the commit of
    // the first transaction. Otherwise, saving operations from the second transaction to the same
    // partialTxnList as the first transaction will trigger an invariant because of the mismatching
    // transaction numbers.
    testOpsAreIdempotent({partialOp1, commitOp1, partialOp2, commitOp2});

    // The transaction table should only contain the second transaction of the session.
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum2,
                        commitOp2.getOpTime(),
                        commitOp2.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kCommitted);
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc2));
}

TEST_F(IdempotencyTestTxns, CommitAndAbortTwoTransactionsWithPartialTxnOpsAtOnce) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum1(1);
    TxnNumber txnNum2(2);

    auto partialOp1 = partialTxn(
        lsid, txnNum1, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));
    auto abortOp1 = abortPrepared(lsid, txnNum1, StmtId(1), partialOp1.getOpTime());

    // The second transaction (with a different transaction number) in the same session.
    auto partialOp2 = partialTxn(
        lsid, txnNum2, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc2)));
    auto commitOp2 =
        commitUnprepared(lsid, txnNum2, StmtId(1), BSONArray(), partialOp2.getOpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    // This also tests that we clear the partialTxnList for the session after applying the abort of
    // the first transaction. Otherwise, saving operations from the second transaction to the same
    // partialTxnList as the first transaction will trigger an invariant because of the mismatching
    // transaction numbers.
    testOpsAreIdempotent({partialOp1, abortOp1, partialOp2, commitOp2});

    // The transaction table should only contain the second transaction of the session.
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum2,
                        commitOp2.getOpTime(),
                        commitOp2.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kCommitted);
    ASSERT_FALSE(docExists(_opCtx.get(), nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc2));
}

TEST_F(IdempotencyTestTxns, CommitUnpreparedTransactionWithPartialTxnOpsAndDataPartiallyApplied) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));

    auto commitOp = commitUnprepared(lsid,
                                     txnNum,
                                     StmtId(1),
                                     BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc2)),
                                     partialOp.getOpTime());

    // Manually insert the first document so that the data will partially reflect the transaction
    // when the commitTransaction oplog entry is applied during initial sync. This simulates the
    // case where the transaction committed on the sync source at a point during the initial sync,
    // such that we cloned 'doc' but missed 'doc2'.
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(),
                                                    nss,
                                                    {doc, commitOp.getOpTime().getTimestamp()},
                                                    commitOp.getOpTime().getTerm()));

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({partialOp, commitOp});
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        commitOp.getOpTime(),
                        commitOp.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kCommitted);
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc2));
}

TEST_F(IdempotencyTestTxns, PrepareTransactionWithPartialTxnOps) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));
    auto prepareOp = prepare(lsid,
                             txnNum,
                             StmtId(1),
                             BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc2)),
                             partialOp.getOpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({partialOp, prepareOp});
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        prepareOp.getOpTime(),
                        prepareOp.getWallClockTime(),
                        partialOp.getOpTime(),
                        DurableTxnStateEnum::kPrepared);
    // Document should not be visible yet.
    ASSERT_FALSE(docExists(_opCtx.get(), nss, doc));
}

TEST_F(IdempotencyTestTxns, EmptyPrepareTransaction) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    // It is possible to have an empty prepare oplog entry.
    auto prepareOp = prepare(lsid, txnNum, StmtId(1), BSONArray(), OpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({prepareOp});
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        prepareOp.getOpTime(),
                        prepareOp.getWallClockTime(),
                        prepareOp.getOpTime(),
                        DurableTxnStateEnum::kPrepared);
}

TEST_F(IdempotencyTestTxns, CommitPreparedTransactionWithPartialTxnOps) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));
    auto prepareOp = prepare(lsid,
                             txnNum,
                             StmtId(1),
                             BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc2)),
                             partialOp.getOpTime());
    auto commitOp = commitPrepared(lsid, txnNum, StmtId(2), prepareOp.getOpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({partialOp, prepareOp, commitOp});
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        commitOp.getOpTime(),
                        commitOp.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kCommitted);
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc2));
}

TEST_F(IdempotencyTestTxns, CommitTwoPreparedTransactionsWithPartialTxnOpsAtOnce) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum1(1);
    TxnNumber txnNum2(2);

    auto partialOp1 = partialTxn(
        lsid, txnNum1, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));
    auto prepareOp1 = prepare(lsid, txnNum1, StmtId(1), BSONArray(), partialOp1.getOpTime());
    auto commitOp1 = commitPrepared(lsid, txnNum1, StmtId(2), prepareOp1.getOpTime());

    // The second transaction (with a different transaction number) in the same session.
    auto partialOp2 = partialTxn(
        lsid, txnNum2, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc2)));
    auto prepareOp2 = prepare(lsid, txnNum2, StmtId(1), BSONArray(), partialOp2.getOpTime());
    auto commitOp2 = commitPrepared(lsid, txnNum2, StmtId(2), prepareOp2.getOpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    // This also tests that we clear the partialTxnList for the session after applying the commit of
    // the first prepared transaction. Otherwise, saving operations from the second transaction to
    // the same partialTxnList as the first transaction will trigger an invariant because of the
    // mismatching transaction numbers.
    testOpsAreIdempotent({partialOp1, prepareOp1, commitOp1, partialOp2, prepareOp2, commitOp2});

    // The transaction table should only contain the second transaction of the session.
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum2,
                        commitOp2.getOpTime(),
                        commitOp2.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kCommitted);
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc2));
}

TEST_F(IdempotencyTestTxns, CommitPreparedTransactionWithPartialTxnOpsAndDataPartiallyApplied) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));
    auto prepareOp = prepare(lsid,
                             txnNum,
                             StmtId(1),
                             BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc2)),
                             partialOp.getOpTime());
    auto commitOp = commitPrepared(lsid, txnNum, StmtId(2), prepareOp.getOpTime());

    // Manually insert the first document so that the data will partially reflect the transaction
    // when the commitTransaction oplog entry is applied during initial sync. This simulates the
    // case where the transaction committed on the sync source at a point during the initial sync,
    // such that we cloned 'doc' but missed 'doc2'.
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(),
                                                    nss,
                                                    {doc, commitOp.getOpTime().getTimestamp()},
                                                    commitOp.getOpTime().getTerm()));

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({partialOp, prepareOp, commitOp});
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        commitOp.getOpTime(),
                        commitOp.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kCommitted);
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), nss, doc2));
}

TEST_F(IdempotencyTestTxns, AbortPreparedTransactionWithPartialTxnOps) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));
    auto prepareOp = prepare(lsid,
                             txnNum,
                             StmtId(1),
                             BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc2)),
                             partialOp.getOpTime());
    auto abortOp = abortPrepared(lsid, txnNum, StmtId(2), prepareOp.getOpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({partialOp, prepareOp, abortOp});
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        abortOp.getOpTime(),
                        abortOp.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kAborted);
    ASSERT_FALSE(docExists(_opCtx.get(), nss, doc));
    ASSERT_FALSE(docExists(_opCtx.get(), nss, doc2));
}

TEST_F(IdempotencyTestTxns, AbortInProgressTransaction) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)));
    auto abortOp = abortPrepared(lsid, txnNum, StmtId(1), partialOp.getOpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({partialOp, abortOp});
    repl::checkTxnTable(_opCtx.get(),
                        lsid,
                        txnNum,
                        abortOp.getOpTime(),
                        abortOp.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kAborted);
    ASSERT_FALSE(docExists(_opCtx.get(), nss, doc));
}

TEST_F(IdempotencyTestTxns, CommitUnpreparedTransactionIgnoresNamespaceNotFoundErrors) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);

    // Instead of creating a collection, we generate an arbitrary UUID to use for the operations
    // below. This simulates the case where, during initial sync, a document D was inserted into a
    // collection C on the sync source and then collection C was dropped, after we started fetching
    // oplog entries but before we started collection cloning. In this case, we would not clone
    // collection C, but when we try to apply the insertion of document D after collection cloning
    // has finished, the collection would not exist since we never created it. It is acceptable to
    // ignore the NamespaceNotFound error in this case since we know the collection will be dropped
    // later on.
    auto uuid = UUID::gen();
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto commitOp = commitUnprepared(
        lsid, txnNum, StmtId(1), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)), OpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({commitOp});

    // The op should have thrown a NamespaceNotFound error, which should have been ignored, so the
    // operation has no effect.
    ASSERT_FALSE(docExists(_opCtx.get(), nss, doc));
}

TEST_F(IdempotencyTestTxns, CommitPreparedTransactionIgnoresNamespaceNotFoundErrors) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);

    // Instead of creating a collection, we generate an arbitrary UUID to use for the operations
    // below. This simulates the case where, during initial sync, a document D was inserted into a
    // collection C on the sync source and then collection C was dropped, after we started fetching
    // oplog entries but before we started collection cloning. In this case, we would not clone
    // collection C, but when we try to apply the insertion of document D after collection cloning
    // has finished, the collection would not exist since we never created it. It is acceptable to
    // ignore the NamespaceNotFound error in this case since we know the collection will be dropped
    // later on.
    auto uuid = UUID::gen();
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto prepareOp = prepare(
        lsid, txnNum, StmtId(0), BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc)), OpTime());
    auto commitOp = commitPrepared(lsid, txnNum, StmtId(1), prepareOp.getOpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({prepareOp, commitOp});

    // The op should have thrown a NamespaceNotFound error, which should have been ignored, so the
    // operation has no effect.
    ASSERT_FALSE(docExists(_opCtx.get(), nss, doc));
}

}  // namespace
}  // namespace repl
}  // namespace mongo
