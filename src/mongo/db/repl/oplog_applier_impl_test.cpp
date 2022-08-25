/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/idempotency_test_fixture.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/transaction/transaction_participant_gen.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/idl/server_parameter_test_util.h"
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

template <typename T, bool enable>
class SetSteadyStateConstraints : public T {
protected:
    void setUp() override {
        T::setUp();
        _constraintsEnabled = oplogApplicationEnforcesSteadyStateConstraints;
        oplogApplicationEnforcesSteadyStateConstraints = enable;
    }

    void tearDown() override {
        oplogApplicationEnforcesSteadyStateConstraints = _constraintsEnabled;
        T::tearDown();
    }

private:
    bool _constraintsEnabled;
};

typedef SetSteadyStateConstraints<OplogApplierImplTest, false>
    OplogApplierImplTestDisableSteadyStateConstraints;
typedef SetSteadyStateConstraints<OplogApplierImplTest, true>
    OplogApplierImplTestEnableSteadyStateConstraints;

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsInsertDocumentDatabaseMissing) {
    NamespaceString nss("test.t");
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, {});
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), &op, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
}

TEST_F(OplogApplierImplTestDisableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteDocumentDatabaseMissing) {
    NamespaceString otherNss("test.othername");
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, {});
    int prevDeleteFromMissing = replOpCounters.getDeleteFromMissingNamespace()->load();
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, false);
    auto postDeleteFromMissing = replOpCounters.getDeleteFromMissingNamespace()->load();
    ASSERT_EQ(1, postDeleteFromMissing - prevDeleteFromMissing);

    ASSERT_EQ(postDeleteFromMissing,
              replOpCounters.getObj()
                  .getObjectField("constraintsRelaxed")
                  .getField("deleteFromMissingNamespace")
                  .Long());
}

TEST_F(OplogApplierImplTestEnableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteDocumentDatabaseMissing) {
    NamespaceString otherNss("test.othername");
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, {});
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), &op, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
}

TEST_F(OplogApplierImplTest,
       applyOplogEntryOrGroupedInsertsInsertDocumentCollectionLookupByUUIDFails) {
    const NamespaceString nss("test.t");
    createDatabase(_opCtx.get(), nss.db());
    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kInsert, otherNss, kUuid);
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), &op, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
}

TEST_F(OplogApplierImplTestDisableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteDocumentCollectionLookupByUUIDFails) {
    const NamespaceString nss("test.t");
    createDatabase(_opCtx.get(), nss.db());
    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, kUuid);
    int prevDeleteFromMissing = replOpCounters.getDeleteFromMissingNamespace()->load();
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, false);
    auto postDeleteFromMissing = replOpCounters.getDeleteFromMissingNamespace()->load();
    ASSERT_EQ(1, postDeleteFromMissing - prevDeleteFromMissing);

    ASSERT_EQ(postDeleteFromMissing,
              replOpCounters.getObj()
                  .getObjectField("constraintsRelaxed")
                  .getField("deleteFromMissingNamespace")
                  .Long());
}

TEST_F(OplogApplierImplTestEnableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteDocumentCollectionLookupByUUIDFails) {
    const NamespaceString nss("test.t");
    createDatabase(_opCtx.get(), nss.db());
    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, kUuid);
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), &op, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsInsertDocumentCollectionMissing) {
    const NamespaceString nss("test.t");
    createDatabase(_opCtx.get(), nss.db());
    // Even though the collection doesn't exist, this is handled in the actual application function,
    // which in the case of this test just ignores such errors. This tests mostly that we don't
    // implicitly create the collection.
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, {});
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), &op, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
    ASSERT_FALSE(collectionExists(_opCtx.get(), nss));
}

TEST_F(OplogApplierImplTestDisableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteDocumentCollectionMissing) {
    const NamespaceString nss("test.t");
    createDatabase(_opCtx.get(), nss.db());
    // Even though the collection doesn't exist, this is handled in the actual application function,
    // which in the case of this test just ignores such errors. This tests mostly that we don't
    // implicitly create the collection.
    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, {});
    int prevDeleteFromMissing = replOpCounters.getDeleteFromMissingNamespace()->load();
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, false);
    ASSERT_FALSE(collectionExists(_opCtx.get(), nss));
    auto postDeleteFromMissing = replOpCounters.getDeleteFromMissingNamespace()->load();
    ASSERT_EQ(1, postDeleteFromMissing - prevDeleteFromMissing);

    ASSERT_EQ(postDeleteFromMissing,
              replOpCounters.getObj()
                  .getObjectField("constraintsRelaxed")
                  .getField("deleteFromMissingNamespace")
                  .Long());
}

TEST_F(OplogApplierImplTestEnableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteDocumentCollectionMissing) {
    const NamespaceString nss("test.t");
    createDatabase(_opCtx.get(), nss.db());
    // With steady state constraints enabled, attempting to delete from a missing collection is an
    // error.
    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, {});
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), &op, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsInsertDocumentCollectionExists) {
    const NamespaceString nss("test.t");
    repl::createCollection(_opCtx.get(), nss, {});
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, {});
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, true);
}

TEST_F(OplogApplierImplTestDisableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteDocumentDocMissing) {
    const NamespaceString nss("test.t");
    repl::createCollection(_opCtx.get(), nss, {});
    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, {});
    int prevDeleteWasEmpty = replOpCounters.getDeleteWasEmpty()->load();
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, false);
    auto postDeleteWasEmpty = replOpCounters.getDeleteWasEmpty()->load();
    ASSERT_EQ(1, postDeleteWasEmpty - prevDeleteWasEmpty);

    ASSERT_EQ(postDeleteWasEmpty,
              replOpCounters.getObj()
                  .getObjectField("constraintsRelaxed")
                  .getField("deleteWasEmpty")
                  .Long());
}

TEST_F(OplogApplierImplTestEnableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteDocumentDocMissing) {
    const NamespaceString nss("test.t");
    repl::createCollection(_opCtx.get(), nss, {});
    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, {});
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), &op, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NoSuchKey>);
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsDeleteDocumentCollectionAndDocExist) {
    const NamespaceString nss("test.t");
    createCollection(_opCtx.get(), nss, createRecordPreImageCollectionOptions());
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {BSON("_id" << 0)}, 0));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, {});
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, true);
}

TEST_F(OplogApplierImplTestDisableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsInsertExistingDocument) {
    const NamespaceString nss("test.t");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {BSON("_id" << 0)}, 0));
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, uuid);
    int prevInsertOnExistingDoc = replOpCounters.getInsertOnExistingDoc()->load();
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, false);
    auto postInsertOnExistingDoc = replOpCounters.getInsertOnExistingDoc()->load();
    ASSERT_EQ(1, postInsertOnExistingDoc - prevInsertOnExistingDoc);

    ASSERT_EQ(postInsertOnExistingDoc,
              replOpCounters.getObj()
                  .getObjectField("constraintsRelaxed")
                  .getField("insertOnExistingDoc")
                  .Long());
}

TEST_F(OplogApplierImplTestEnableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsInsertExistingDocument) {
    const NamespaceString nss("test.t");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {BSON("_id" << 0)}, 0));
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, uuid);
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::DuplicateKey, op, false);
}

TEST_F(OplogApplierImplTestDisableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsUpdateMissingDocument) {
    const NamespaceString nss("test.t");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto op = makeOplogEntry(repl::OpTypeEnum::kUpdate,
                             nss,
                             uuid,
                             update_oplog_entry::makeDeltaOplogEntry(
                                 BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                             BSON("_id" << 0));
    int prevUpdateOnMissingDoc = replOpCounters.getUpdateOnMissingDoc()->load();
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, true);
    auto postUpdateOnMissingDoc = replOpCounters.getUpdateOnMissingDoc()->load();
    ASSERT_EQ(1, postUpdateOnMissingDoc - prevUpdateOnMissingDoc);

    ASSERT_EQ(postUpdateOnMissingDoc,
              replOpCounters.getObj()
                  .getObjectField("constraintsRelaxed")
                  .getField("updateOnMissingDoc")
                  .Long());
}

TEST_F(OplogApplierImplTestEnableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsUpdateMissingDocument) {
    const NamespaceString nss("test.t");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto op = makeOplogEntry(repl::OpTypeEnum::kUpdate,
                             nss,
                             uuid,
                             update_oplog_entry::makeDeltaOplogEntry(
                                 BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                             BSON("_id" << 0));
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::UpdateOperationFailed, op, false);
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsInsertDocumentCollectionLockedByUUID) {
    const NamespaceString nss("test.t");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    // Test that the collection to lock is determined by the UUID and not the 'ns' field.
    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kInsert, otherNss, uuid);
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, true);
}

TEST_F(OplogApplierImplTestDisableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteMissingDocCollectionLockedByUUID) {
    const NamespaceString nss("test.t");
    CollectionOptions options;
    options.uuid = kUuid;
    createCollection(_opCtx.get(), nss, options);

    // Test that the collection to lock is determined by the UUID and not the 'ns' field.
    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, options.uuid);
    int prevDeleteWasEmpty = replOpCounters.getDeleteWasEmpty()->load();
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, false);
    auto postDeleteWasEmpty = replOpCounters.getDeleteWasEmpty()->load();
    ASSERT_EQ(1, postDeleteWasEmpty - prevDeleteWasEmpty);

    ASSERT_EQ(postDeleteWasEmpty,
              replOpCounters.getObj()
                  .getObjectField("constraintsRelaxed")
                  .getField("deleteWasEmpty")
                  .Long());
}

TEST_F(OplogApplierImplTestEnableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteMissingDocCollectionLockedByUUID) {
    const NamespaceString nss("test.t");
    CollectionOptions options;
    options.uuid = kUuid;
    createCollection(_opCtx.get(), nss, options);

    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, options.uuid);
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), &op, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NoSuchKey>);
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsDeleteDocumentCollectionLockedByUUID) {
    const NamespaceString nss("test.t");
    CollectionOptions options = createRecordPreImageCollectionOptions();
    options.uuid = kUuid;
    createCollection(_opCtx.get(), nss, options);

    // Make sure the document to be deleted exists.
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {BSON("_id" << 0)}, 0));

    // Test that the collection to lock is determined by the UUID and not the 'ns' field.
    NamespaceString otherNss(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, options.uuid);
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, true);
}

TEST_F(OplogApplierImplTest, applyOplogEntryToRecordChangeStreamPreImages) {
    // Setup the pre-images collection.
    ChangeStreamPreImagesCollectionManager::createPreImagesCollection(_opCtx.get(),
                                                                      boost::none /* tenantId */);

    // Create the collection.
    const NamespaceString nss("test.t");
    CollectionOptions options;
    options.uuid = kUuid;
    options.changeStreamPreAndPostImagesOptions.setEnabled(true);
    createCollection(_opCtx.get(), nss, options);

    struct TestCase {
        repl::OpTypeEnum opType;
        OplogApplication::Mode applicationMode;
        boost::optional<bool> fromMigrate;
        bool shouldRecordPreImage;
    };

    // Generate test cases.
    std::vector<TestCase> testCases;
    auto generateTestCasesForOperations = [&](OplogApplication::Mode applicationMode,
                                              boost::optional<bool> fromMigrate,
                                              bool shouldRecordPreImage) {
        for (auto&& opType : {repl::OpTypeEnum::kUpdate, repl::OpTypeEnum::kDelete}) {
            testCases.push_back({opType, applicationMode, fromMigrate, shouldRecordPreImage});
        }
    };
    generateTestCasesForOperations(OplogApplication::Mode::kSecondary, {}, true);
    generateTestCasesForOperations(OplogApplication::Mode::kRecovering, {}, true);
    generateTestCasesForOperations(OplogApplication::Mode::kInitialSync, {}, false);
    const auto kFromMigrate{true};
    generateTestCasesForOperations(OplogApplication::Mode::kSecondary, kFromMigrate, false);
    generateTestCasesForOperations(OplogApplication::Mode::kRecovering, kFromMigrate, false);
    generateTestCasesForOperations(OplogApplication::Mode::kInitialSync, kFromMigrate, false);

    int docId{0};
    for (auto&& testCase : testCases) {
        auto document = BSON("_id" << docId);
        auto&& documentId = document;

        // Make sure the document to be modified exists.
        ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {document}, 0));

        // Make an oplog entry.
        auto op = makeOplogEntry(
            [opCtx = _opCtx.get()] {
                WriteUnitOfWork wuow{opCtx};
                ScopeGuard guard{[&wuow] { wuow.commit(); }};
                return repl::getNextOpTime(opCtx);
            }(),
            testCase.opType,
            nss,
            options.uuid,
            testCase.opType == repl::OpTypeEnum::kUpdate
                ? update_oplog_entry::makeDeltaOplogEntry(
                      BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}")))
                : documentId,
            {documentId},
            testCase.fromMigrate);

        // Apply the oplog entry.
        ASSERT_OK(
            _applyOplogEntryOrGroupedInsertsWrapper(_opCtx.get(), &op, testCase.applicationMode));

        // Load pre-image and cleanup the state.
        WriteUnitOfWork wuow{_opCtx.get()};
        ChangeStreamPreImageId preImageId{*(options.uuid), op.getOpTime().getTimestamp(), 0};
        BSONObj preImageDocumentKey = BSON("_id" << preImageId.toBSON());
        auto preImageLoadResult =
            getStorageInterface()->deleteById(_opCtx.get(),
                                              NamespaceString::kChangeStreamPreImagesNamespace,
                                              preImageDocumentKey.firstElement());
        repl::getNextOpTime(_opCtx.get());
        wuow.commit();

        std::string testDesc{
            str::stream() << "TestCase: opType: " << OpType_serializer(testCase.opType)
                          << " mode: " << OplogApplication::modeToString(testCase.applicationMode)
                          << " fromMigrate: " << (testCase.fromMigrate.get_value_or(false))
                          << " shouldRecordPreImage: " << testCase.shouldRecordPreImage};

        // Check if pre-image was recorded.
        if (testCase.shouldRecordPreImage) {
            ASSERT_OK(preImageLoadResult) << testDesc;

            // Verify that the pre-image document is correct.
            const auto preImageDocument = ChangeStreamPreImage::parse(
                IDLParserContext{"test"}, preImageLoadResult.getValue());
            ASSERT_BSONOBJ_EQ(preImageDocument.getPreImage(), document);
            ASSERT_EQUALS(preImageDocument.getOperationTime(), op.getWallClockTime()) << testDesc;

        } else {
            ASSERT_FALSE(preImageLoadResult.isOK()) << testDesc;
        }
        ++docId;
    }
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsCommand) {
    NamespaceString nss("test.t");
    auto op =
        BSON("op"
             << "c"
             << "ns" << nss.getCommandNS().ns() << "wall" << Date_t() << "o"
             << BSON("create" << nss.coll()) << "ts" << Timestamp(1, 1) << "ui" << UUID::gen());
    bool applyCmdCalled = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            const CollectionPtr&,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&) {
        applyCmdCalled = true;
        ASSERT_TRUE(opCtx);
        ASSERT_TRUE(opCtx->lockState()->isDbLockedForMode(nss.dbName(), MODE_IX));
        ASSERT_EQUALS(nss, collNss);
        return Status::OK();
    };
    auto entry = OplogEntry(op);
    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), &entry, OplogApplication::Mode::kInitialSync));
    ASSERT_TRUE(applyCmdCalled);
}

/**
 * Test only subclass of OplogApplierImpl that does not apply oplog entries, but tracks ops.
 */
class TrackOpsAppliedApplier : public OplogApplierImpl {
public:
    using OplogApplierImpl::OplogApplierImpl;

    Status applyOplogBatchPerWorker(OperationContext* opCtx,
                                    std::vector<const OplogEntry*>* ops,
                                    WorkerMultikeyPathInfo* workerMultikeyPathInfo,
                                    bool isDataConsistent) override;

    std::vector<OplogEntry> getOperationsApplied() {
        stdx::lock_guard lk(_mutex);
        return _operationsApplied;
    }

private:
    std::vector<OplogEntry> _operationsApplied;
    // Synchronize reads and writes to 'operationsApplied'.
    Mutex _mutex = MONGO_MAKE_LATCH("TrackOpsAppliedApplier::_mutex");
};

Status TrackOpsAppliedApplier::applyOplogBatchPerWorker(
    OperationContext* opCtx,
    std::vector<const OplogEntry*>* ops,
    WorkerMultikeyPathInfo* workerMultikeyPathInfo,
    const bool isDataConsistent) {
    stdx::lock_guard lk(_mutex);
    for (auto&& opPtr : *ops) {
        _operationsApplied.push_back(*opPtr);
    }
    return Status::OK();
}

DEATH_TEST_F(OplogApplierImplTest, MultiApplyAbortsWhenNoOperationsAreGiven, "!ops.empty()") {
    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    TrackOpsAppliedApplier oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());
    oplogApplier.applyOplogBatch(_opCtx.get(), {}).getStatus().ignore();
}

bool _testOplogEntryIsForCappedCollection(OperationContext* opCtx,
                                          ReplicationCoordinator* const replCoord,
                                          ReplicationConsistencyMarkers* const consistencyMarkers,
                                          StorageInterface* const storageInterface,
                                          const NamespaceString& nss,
                                          const CollectionOptions& options) {
    auto writerPool = makeReplWriterPool();
    createCollection(opCtx, nss, options);

    auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, BSON("a" << 1));
    ASSERT_FALSE(op.isForCappedCollection());

    NoopOplogApplierObserver observer;
    TrackOpsAppliedApplier oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        replCoord,
        consistencyMarkers,
        storageInterface,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());
    auto lastOpTime = unittest::assertGet(oplogApplier.applyOplogBatch(opCtx, {op}));
    ASSERT_EQUALS(op.getOpTime(), lastOpTime);

    const auto opsApplied = oplogApplier.getOperationsApplied();
    ASSERT_EQUALS(1U, opsApplied.size());
    const auto& opApplied = opsApplied.front();
    ASSERT_EQUALS(op.getEntry(), opApplied.getEntry());
    // "isForCappedCollection" is not parsed from raw oplog entry document.
    return opApplied.isForCappedCollection();
}

TEST_F(
    OplogApplierImplTest,
    MultiApplyDoesNotSetOplogEntryIsForCappedCollectionWhenProcessingNonCappedCollectionInsertOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    ASSERT_FALSE(_testOplogEntryIsForCappedCollection(_opCtx.get(),
                                                      ReplicationCoordinator::get(_opCtx.get()),
                                                      getConsistencyMarkers(),
                                                      getStorageInterface(),
                                                      nss,
                                                      CollectionOptions()));
}

TEST_F(OplogApplierImplTest,
       MultiApplySetsOplogEntryIsForCappedCollectionWhenProcessingCappedCollectionInsertOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    ASSERT_TRUE(_testOplogEntryIsForCappedCollection(_opCtx.get(),
                                                     ReplicationCoordinator::get(_opCtx.get()),
                                                     getConsistencyMarkers(),
                                                     getStorageInterface(),
                                                     nss,
                                                     createOplogCollectionOptions()));
}

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncUsesApplyOplogEntryOrGroupedInsertsToApplyOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss);

    std::vector<const OplogEntry*> ops = {&op};
    WorkerMultikeyPathInfo pathInfo;

    TestApplyOplogGroupApplier oplogApplier(
        nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kSecondary));
    const bool dataIsConsistent = true;
    ASSERT_OK(
        oplogApplier.applyOplogBatchPerWorker(_opCtx.get(), &ops, &pathInfo, dataIsConsistent));
    // Collection should be created after applyOplogEntryOrGroupedInserts() processes operation.
    ASSERT_TRUE(AutoGetCollectionForReadCommand(_opCtx.get(), nss).getCollection());
}

TEST_F(OplogApplierImplTest,
       TxnTableUpdatesDoNotGetCoalescedForRetryableWritesAcrossDifferentTxnNumbers) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    const NamespaceString& nss{"test", "foo"};
    repl::OpTime firstInsertOpTime(Timestamp(1, 0), 1);
    auto firstRetryableOp = makeInsertDocumentOplogEntryWithSessionInfo(
        firstInsertOpTime, nss, BSON("_id" << 1), sessionInfo);

    repl::OpTime secondInsertOpTime(Timestamp(2, 0), 1);
    sessionInfo.setTxnNumber(4);
    auto secondRetryableOp = makeInsertDocumentOplogEntryWithSessionInfo(
        secondInsertOpTime, nss, BSON("_id" << 2), sessionInfo);

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    std::vector<std::vector<const OplogEntry*>> writerVectors(
        writerPool->getStats().options.maxThreads);
    std::vector<std::vector<OplogEntry>> derivedOps;
    std::vector<OplogEntry> ops{firstRetryableOp, secondRetryableOp};
    oplogApplier.fillWriterVectors_forTest(_opCtx.get(), &ops, &writerVectors, &derivedOps);
    // We expect a total of two derived ops - one for each distinct 'txnNumber'.
    ASSERT_EQUALS(2, derivedOps.size());
    ASSERT_EQUALS(1, derivedOps[0].size());
    ASSERT_EQUALS(1, derivedOps[1].size());
    const auto firstDerivedOp = derivedOps[0][0];
    ASSERT_EQUALS(firstInsertOpTime.getTimestamp(),
                  firstDerivedOp.getObject()["lastWriteOpTime"]["ts"].timestamp());
    ASSERT_EQUALS(NamespaceString::kSessionTransactionsTableNamespace, firstDerivedOp.getNss());
    ASSERT_EQUALS(*firstRetryableOp.getTxnNumber(),
                  firstDerivedOp.getObject()["txnNum"].numberInt());
    const auto secondDerivedOp = derivedOps[1][0];
    ASSERT_EQUALS(*secondRetryableOp.getTxnNumber(),
                  secondDerivedOp.getObject()["txnNum"].numberInt());
    ASSERT_EQUALS(NamespaceString::kSessionTransactionsTableNamespace, secondDerivedOp.getNss());
    ASSERT_EQUALS(secondInsertOpTime.getTimestamp(),
                  secondDerivedOp.getObject()["lastWriteOpTime"]["ts"].timestamp());
}

class MultiOplogEntryOplogApplierImplTest : public OplogApplierImplTest {
public:
    MultiOplogEntryOplogApplierImplTest()
        : _nss1("test.preptxn1"), _nss2("test.preptxn2"), _txnNum(1) {}

protected:
    void setUp() override {
        OplogApplierImplTest::setUp();
        const NamespaceString cmdNss{"admin", "$cmd"};

        _uuid1 = createCollectionWithUuid(_opCtx.get(), _nss1);
        _uuid2 = createCollectionWithUuid(_opCtx.get(), _nss2);
        createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);

        _lsid = makeLogicalSessionId(_opCtx.get());

        _insertOp1 = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(1), 1), 1LL},
            cmdNss,
            BSON("applyOps" << BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << _nss1.ns() << "ui" << *_uuid1 << "o"
                                               << BSON("_id" << 1)))
                            << "partialTxn" << true),
            _lsid,
            _txnNum,
            {StmtId(0)},
            OpTime());
        _insertOp2 = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(1), 2), 1LL},
            cmdNss,
            BSON("applyOps" << BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << _nss2.ns() << "ui" << *_uuid2 << "o"
                                               << BSON("_id" << 2)))
                            << "partialTxn" << true),
            _lsid,
            _txnNum,
            {StmtId(1)},
            _insertOp1->getOpTime());
        _commitOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(1), 3), 1LL},
            cmdNss,
            BSON("applyOps" << BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << _nss2.ns() << "ui" << *_uuid2 << "o"
                                               << BSON("_id" << 3)))),
            _lsid,
            _txnNum,
            {StmtId(2)},
            _insertOp2->getOpTime());
        _opObserver->onInsertsFn =
            [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
                stdx::lock_guard<Latch> lock(_insertMutex);
                if (nss.isOplog()) {
                    _insertedOplogDocs.insert(_insertedOplogDocs.end(), docs.begin(), docs.end());
                } else if (nss == _nss1 || nss == _nss2 ||
                           nss == NamespaceString::kSessionTransactionsTableNamespace) {
                    // Storing the inserted documents in a sorted data structure to make checking
                    // for valid results easier. The inserts will be performed by different threads
                    // and there's no guarantee of the order.
                    _insertedDocs[nss].insert(docs.begin(), docs.end());
                } else
                    FAIL("Unexpected insert") << " into " << nss << " first doc: " << docs.front();
            };

        _writerPool = makeReplWriterPool();
    }

    void tearDown() override {
        OplogApplierImplTest::tearDown();
    }

    void checkTxnTable(const LogicalSessionId& lsid,
                       const TxnNumber& txnNum,
                       const repl::OpTime& expectedOpTime,
                       Date_t expectedWallClock,
                       boost::optional<repl::OpTime> expectedStartOpTime,
                       DurableTxnStateEnum expectedState) {
        repl::checkTxnTable(_opCtx.get(),
                            lsid,
                            txnNum,
                            expectedOpTime,
                            expectedWallClock,
                            expectedStartOpTime,
                            expectedState);
    }

    std::vector<BSONObj>& oplogDocs() {
        return _insertedOplogDocs;
    }

protected:
    NamespaceString _nss1;
    NamespaceString _nss2;
    boost::optional<UUID> _uuid1;
    boost::optional<UUID> _uuid2;
    LogicalSessionId _lsid;
    TxnNumber _txnNum;
    boost::optional<OplogEntry> _insertOp1, _insertOp2;
    boost::optional<OplogEntry> _commitOp;
    std::map<NamespaceString, SimpleBSONObjSet> _insertedDocs;
    std::vector<BSONObj> _insertedOplogDocs;
    std::unique_ptr<ThreadPool> _writerPool;

private:
    Mutex _insertMutex = MONGO_MAKE_LATCH("MultiOplogEntryOplogApplierImplTest::_insertMutex");
};

TEST_F(MultiOplogEntryOplogApplierImplTest, MultiApplyUnpreparedTransactionSeparate) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());

    // Apply a batch with only the first operation.  This should result in the first oplog entry
    // being put in the oplog and updating the transaction table, but not actually being applied
    // because they are part of a pending transaction.
    const auto expectedStartOpTime = _insertOp1->getOpTime();
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_insertOp1}));
    ASSERT_EQ(1U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(oplogDocs().back(), _insertOp1->getEntry().toBSON());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _insertOp1->getOpTime(),
                  _insertOp1->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kInProgress);

    // Apply a batch with only the second operation.  This should result in the second oplog entry
    // being put in the oplog, but with no effect because the operation is part of a pending
    // transaction.
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_insertOp2}));
    ASSERT_EQ(2U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(oplogDocs().back(), _insertOp2->getEntry().toBSON());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    // The transaction table should not have been updated for partialTxn operations that are not the
    // first in a transaction.
    checkTxnTable(_lsid,
                  _txnNum,
                  _insertOp1->getOpTime(),
                  _insertOp1->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kInProgress);

    // Apply a batch with only the commit.  This should result in the commit being put in the
    // oplog, and the two previous entries being applied.
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_commitOp}));
    ASSERT_EQ(3U, oplogDocs().size());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    ASSERT_BSONOBJ_EQ(oplogDocs().back(), _commitOp->getEntry().toBSON());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitOp->getOpTime(),
                  _commitOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryOplogApplierImplTest, MultiApplyUnpreparedTransactionAllAtOnce) {
    // Skipping writes to oplog proves we're testing the code path which does not rely on reading
    // the oplog.
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kRecovering),
        _writerPool.get());

    // Apply both inserts and the commit in a single batch.  We expect no oplog entries to
    // be inserted (because we've set skipWritesToOplog), and both entries to be committed.
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_insertOp1, *_insertOp2, *_commitOp}));
    ASSERT_EQ(0U, oplogDocs().size());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitOp->getOpTime(),
                  _commitOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryOplogApplierImplTest, MultiApplyUnpreparedTransactionTwoBatches) {
    // Tests an unprepared transaction with ops both in the batch with the commit and prior
    // batches.
    // Populate transaction with 4 linked inserts, one in nss2 and the others in nss1.
    std::vector<OplogEntry> insertOps;
    std::vector<BSONObj> insertDocs;

    const NamespaceString cmdNss{"admin", "$cmd"};
    for (int i = 0; i < 4; i++) {
        insertDocs.push_back(BSON("_id" << i));
        insertOps.push_back(makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(1), i + 1), 1LL},
            cmdNss,
            BSON("applyOps" << BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << (i == 1 ? _nss2.ns() : _nss1.ns()) << "ui"
                                               << (i == 1 ? *_uuid2 : *_uuid1) << "o"
                                               << insertDocs.back()))
                            << "partialTxn" << true),
            _lsid,
            _txnNum,
            {StmtId(i)},
            i == 0 ? OpTime() : insertOps.back().getOpTime()));
    }
    auto commitOp = makeCommandOplogEntryWithSessionInfoAndStmtIds({Timestamp(Seconds(1), 5), 1LL},
                                                                   cmdNss,
                                                                   BSON("applyOps" << BSONArray()),
                                                                   _lsid,
                                                                   _txnNum,
                                                                   {StmtId(4)},
                                                                   insertOps.back().getOpTime());

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());

    // Insert the first entry in its own batch.  This should result in the oplog entry being written
    // but the entry should not be applied as it is part of a pending transaction.
    const auto expectedStartOpTime = insertOps[0].getOpTime();
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {insertOps[0]}));
    ASSERT_EQ(1U, oplogDocs().size());
    ASSERT_EQ(0U, _insertedDocs[_nss1].size());
    ASSERT_EQ(0U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  insertOps[0].getOpTime(),
                  insertOps[0].getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kInProgress);

    // Insert the rest of the entries, including the commit.  These entries should be added to the
    // oplog, and all the entries including the first should be applied.
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(),
                                           {insertOps[1], insertOps[2], insertOps[3], commitOp}));
    ASSERT_EQ(5U, oplogDocs().size());
    ASSERT_EQ(3U, _insertedDocs[_nss1].size());
    ASSERT_EQ(1U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  commitOp.getOpTime(),
                  commitOp.getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);

    // Check that we inserted the expected documents
    auto nss1It = _insertedDocs[_nss1].begin();
    ASSERT_BSONOBJ_EQ(insertDocs[0], *(nss1It++));
    ASSERT_BSONOBJ_EQ(insertDocs[1], *_insertedDocs[_nss2].begin());
    ASSERT_BSONOBJ_EQ(insertDocs[2], *(nss1It++));
    ASSERT_BSONOBJ_EQ(insertDocs[3], *(nss1It++));
}

TEST_F(MultiOplogEntryOplogApplierImplTest, MultiApplyTwoTransactionsOneBatch) {
    // Tests that two transactions on the same session ID in the same batch both
    // apply correctly.
    TxnNumber txnNum1(1);
    TxnNumber txnNum2(2);


    std::vector<OplogEntry> insertOps1, insertOps2;
    const NamespaceString cmdNss{"admin", "$cmd"};
    insertOps1.push_back(makeCommandOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(1), 1), 1LL},
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << _nss1.ns() << "ui" << *_uuid1 << "o"
                                           << BSON("_id" << 1)))
                        << "partialTxn" << true),
        _lsid,
        txnNum1,
        {StmtId(0)},
        OpTime()));
    insertOps1.push_back(makeCommandOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(1), 2), 1LL},
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << _nss1.ns() << "ui" << *_uuid1 << "o"
                                           << BSON("_id" << 2)))
                        << "partialTxn" << true),

        _lsid,
        txnNum1,
        {StmtId(1)},
        insertOps1.back().getOpTime()));
    insertOps2.push_back(makeCommandOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(2), 1), 1LL},
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << _nss1.ns() << "ui" << *_uuid1 << "o"
                                           << BSON("_id" << 3)))
                        << "partialTxn" << true),
        _lsid,
        txnNum2,
        {StmtId(0)},
        OpTime()));
    insertOps2.push_back(makeCommandOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(2), 2), 1LL},
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << _nss1.ns() << "ui" << *_uuid1 << "o"
                                           << BSON("_id" << 4)))
                        << "partialTxn" << true),
        _lsid,
        txnNum2,
        {StmtId(1)},
        insertOps2.back().getOpTime()));
    auto commitOp1 = makeCommandOplogEntryWithSessionInfoAndStmtIds({Timestamp(Seconds(1), 3), 1LL},
                                                                    _nss1,
                                                                    BSON("applyOps" << BSONArray()),
                                                                    _lsid,
                                                                    txnNum1,
                                                                    {StmtId(2)},
                                                                    insertOps1.back().getOpTime());
    auto commitOp2 = makeCommandOplogEntryWithSessionInfoAndStmtIds({Timestamp(Seconds(2), 3), 1LL},
                                                                    _nss1,
                                                                    BSON("applyOps" << BSONArray()),
                                                                    _lsid,
                                                                    txnNum2,
                                                                    {StmtId(2)},
                                                                    insertOps2.back().getOpTime());

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());

    // Note the insert counter so we can check it later.  It is necessary to use opCounters as
    // inserts are idempotent so we will not detect duplicate inserts just by checking inserts in
    // the opObserver.
    int insertsBefore = replOpCounters.getInsert()->load();
    // Insert all the oplog entries in one batch.  All inserts should be executed, in order, exactly
    // once.
    ASSERT_OK(oplogApplier.applyOplogBatch(
        _opCtx.get(),
        {insertOps1[0], insertOps1[1], commitOp1, insertOps2[0], insertOps2[1], commitOp2}));
    ASSERT_EQ(6U, oplogDocs().size());
    ASSERT_EQ(4, replOpCounters.getInsert()->load() - insertsBefore);
    ASSERT_EQ(4U, _insertedDocs[_nss1].size());
    checkTxnTable(_lsid,
                  txnNum2,
                  commitOp2.getOpTime(),
                  commitOp2.getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);

    // Check docs in nss1.
    auto nss1It = _insertedDocs[_nss1].begin();
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), *(nss1It++));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), *(nss1It++));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 3), *(nss1It++));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 4), *(nss1It++));
}


class MultiOplogEntryPreparedTransactionTest : public MultiOplogEntryOplogApplierImplTest {
protected:
    void setUp() override {
        MultiOplogEntryOplogApplierImplTest::setUp();

        _prepareWithPrevOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(1), 3), 1LL},
            _nss1,
            BSON("applyOps" << BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << _nss2.ns() << "ui" << *_uuid2 << "o"
                                               << BSON("_id" << 3)))
                            << "prepare" << true),
            _lsid,
            _txnNum,
            {StmtId(2)},
            _insertOp2->getOpTime());
        _singlePrepareApplyOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(1), 3), 1LL},
            _nss1,
            BSON("applyOps" << BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << _nss1.ns() << "ui" << *_uuid1 << "o"
                                               << BSON("_id" << 0)))
                            << "prepare" << true),
            _lsid,
            _txnNum,
            {StmtId(0)},
            OpTime());
        _commitPrepareWithPrevOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(1), 4), 1LL},
            _nss1,
            BSON("commitTransaction" << 1 << "commitTimestamp" << Timestamp(Seconds(1), 4)),
            _lsid,
            _txnNum,
            {StmtId(3)},
            _prepareWithPrevOp->getOpTime());
        _commitSinglePrepareApplyOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(1), 4), 1LL},
            _nss1,
            BSON("commitTransaction" << 1 << "commitTimestamp" << Timestamp(Seconds(1), 4)),
            _lsid,
            _txnNum,
            {StmtId(1)},
            _prepareWithPrevOp->getOpTime());
        _abortPrepareWithPrevOp =
            makeCommandOplogEntryWithSessionInfoAndStmtIds({Timestamp(Seconds(1), 4), 1LL},
                                                           _nss1,
                                                           BSON("abortTransaction" << 1),
                                                           _lsid,
                                                           _txnNum,
                                                           {StmtId(3)},
                                                           _prepareWithPrevOp->getOpTime());
        _abortSinglePrepareApplyOp = _abortPrepareWithPrevOp =
            makeCommandOplogEntryWithSessionInfoAndStmtIds({Timestamp(Seconds(1), 4), 1LL},
                                                           _nss1,
                                                           BSON("abortTransaction" << 1),
                                                           _lsid,
                                                           _txnNum,
                                                           {StmtId(1)},
                                                           _singlePrepareApplyOp->getOpTime());
    }

protected:
    boost::optional<OplogEntry> _commitPrepareWithPrevOp, _abortPrepareWithPrevOp,
        _singlePrepareApplyOp, _prepareWithPrevOp, _commitSinglePrepareApplyOp,
        _abortSinglePrepareApplyOp;

private:
    Mutex _insertMutex = MONGO_MAKE_LATCH("MultiOplogEntryPreparedTransactionTest::_insertMutex");
};

TEST_F(MultiOplogEntryPreparedTransactionTest, MultiApplyPreparedTransactionSteadyState) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());

    // Apply a batch with the insert operations.  This should result in the oplog entries
    // being put in the oplog and updating the transaction table, but not actually being applied
    // because they are part of a pending transaction.
    getStorageInterface()->oplogDiskLocRegister(_opCtx.get(), _insertOp2->getTimestamp(), true);
    const auto expectedStartOpTime = _insertOp1->getOpTime();
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_insertOp1, *_insertOp2}));
    ASSERT_EQ(2U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(_insertOp1->getEntry().toBSON(), oplogDocs()[0]);
    ASSERT_BSONOBJ_EQ(_insertOp2->getEntry().toBSON(), oplogDocs()[1]);
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _insertOp1->getOpTime(),
                  _insertOp1->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kInProgress);

    // Apply a batch with only the prepare.  This should result in the prepare being put in the
    // oplog, and the two previous entries being applied (but in a transaction) along with the
    // nested insert in the prepare oplog entry.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _prepareWithPrevOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_prepareWithPrevOp}));
    ASSERT_EQ(3U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(_prepareWithPrevOp->getEntry().toBSON(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  _prepareWithPrevOp->getOpTime(),
                  _prepareWithPrevOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit.  This should result in the commit being put in the
    // oplog, and the three previous entries being committed.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _commitPrepareWithPrevOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_commitPrepareWithPrevOp}));
    ASSERT_BSONOBJ_EQ(_commitPrepareWithPrevOp->getEntry().toBSON(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitPrepareWithPrevOp->getOpTime(),
                  _commitPrepareWithPrevOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest, MultiApplyAbortPreparedTransactionCheckTxnTable) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());

    // Apply a batch with the insert operations.  This should result in the oplog entries
    // being put in the oplog and updating the transaction table, but not actually being applied
    // because they are part of a pending transaction.
    getStorageInterface()->oplogDiskLocRegister(_opCtx.get(), _insertOp1->getTimestamp(), true);
    const auto expectedStartOpTime = _insertOp1->getOpTime();
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_insertOp1, *_insertOp2}));
    checkTxnTable(_lsid,
                  _txnNum,
                  _insertOp1->getOpTime(),
                  _insertOp1->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kInProgress);

    // Apply a batch with only the prepare.  This should result in the prepare being put in the
    // oplog, and the two previous entries being applied (but in a transaction) along with the
    // nested insert in the prepare oplog entry.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _prepareWithPrevOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_prepareWithPrevOp}));
    checkTxnTable(_lsid,
                  _txnNum,
                  _prepareWithPrevOp->getOpTime(),
                  _prepareWithPrevOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the abort.  This should result in the abort being put in the
    // oplog and the transaction table being updated accordingly.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _abortPrepareWithPrevOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_abortPrepareWithPrevOp}));
    ASSERT_BSONOBJ_EQ(_abortPrepareWithPrevOp->getEntry().toBSON(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  _abortPrepareWithPrevOp->getOpTime(),
                  _abortPrepareWithPrevOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kAborted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest, MultiApplyPreparedTransactionInitialSync) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kInitialSync),
        _writerPool.get());
    // Apply a batch with the insert operations.  This should result in the oplog entries
    // being put in the oplog and updating the transaction table, but not actually being applied
    // because they are part of a pending transaction.
    getStorageInterface()->oplogDiskLocRegister(_opCtx.get(), _insertOp1->getTimestamp(), true);
    const auto expectedStartOpTime = _insertOp1->getOpTime();
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_insertOp1, *_insertOp2}));
    ASSERT_EQ(2U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(_insertOp1->getEntry().toBSON(), oplogDocs()[0]);
    ASSERT_BSONOBJ_EQ(_insertOp2->getEntry().toBSON(), oplogDocs()[1]);
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _insertOp1->getOpTime(),
                  _insertOp1->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kInProgress);

    // Apply a batch with only the prepare applyOps. This should result in the prepare being put in
    // the oplog, but, since this is initial sync, nothing else.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _prepareWithPrevOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_prepareWithPrevOp}));
    ASSERT_EQ(3U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(_prepareWithPrevOp->getEntry().toBSON(), oplogDocs().back());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _prepareWithPrevOp->getOpTime(),
                  _prepareWithPrevOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit.  This should result in the commit being put in the
    // oplog, and the three previous entries being applied.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _commitPrepareWithPrevOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_commitPrepareWithPrevOp}));
    ASSERT_BSONOBJ_EQ(_commitPrepareWithPrevOp->getEntry().toBSON(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitPrepareWithPrevOp->getOpTime(),
                  _commitPrepareWithPrevOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest, MultiApplyPreparedTransactionRecovery) {
    // For recovery, the oplog must contain the operations before starting.
    for (auto&& entry :
         {*_insertOp1, *_insertOp2, *_prepareWithPrevOp, *_commitPrepareWithPrevOp}) {
        ASSERT_OK(getStorageInterface()->insertDocument(
            _opCtx.get(),
            NamespaceString::kRsOplogNamespace,
            {entry.getEntry().toBSON(), entry.getOpTime().getTimestamp()},
            entry.getOpTime().getTerm()));
    }
    // Ignore docs inserted into oplog in setup.
    oplogDocs().clear();

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kRecovering),
        _writerPool.get());

    // Apply a batch with the insert operations.  This should have no effect, because this is
    // recovery.
    getStorageInterface()->oplogDiskLocRegister(_opCtx.get(), _insertOp1->getTimestamp(), true);
    const auto expectedStartOpTime = _insertOp1->getOpTime();
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_insertOp1, *_insertOp2}));
    ASSERT_TRUE(oplogDocs().empty());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _insertOp1->getOpTime(),
                  _insertOp1->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kInProgress);

    // Apply a batch with only the prepare applyOps. This should have no effect, since this is
    // recovery.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _prepareWithPrevOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_prepareWithPrevOp}));
    ASSERT_TRUE(oplogDocs().empty());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _prepareWithPrevOp->getOpTime(),
                  _prepareWithPrevOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit.  This should result in the the three previous entries
    // being applied.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _commitPrepareWithPrevOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_commitPrepareWithPrevOp}));
    ASSERT_TRUE(oplogDocs().empty());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitPrepareWithPrevOp->getOpTime(),
                  _commitPrepareWithPrevOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest, MultiApplySingleApplyOpsPreparedTransaction) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());
    const auto expectedStartOpTime = _singlePrepareApplyOp->getOpTime();

    // Apply a batch with only the prepare applyOps. This should result in the prepare being put in
    // the oplog, and the nested insert being applied (but in a transaction).
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _singlePrepareApplyOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_singlePrepareApplyOp}));
    ASSERT_EQ(1U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(_singlePrepareApplyOp->getEntry().toBSON(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  _singlePrepareApplyOp->getOpTime(),
                  _singlePrepareApplyOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit.  This should result in the commit being put in the
    // oplog, and prepared insert being committed.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _commitSinglePrepareApplyOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_commitSinglePrepareApplyOp}));
    ASSERT_BSONOBJ_EQ(_commitSinglePrepareApplyOp->getEntry().toBSON(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitSinglePrepareApplyOp->getOpTime(),
                  _commitSinglePrepareApplyOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest, MultiApplyEmptyApplyOpsPreparedTransaction) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());

    auto emptyPrepareApplyOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(1), 3), 1LL},
        _nss1,
        BSON("applyOps" << BSONArray() << "prepare" << true),
        _lsid,
        _txnNum,
        {StmtId(0)},
        OpTime());
    const auto expectedStartOpTime = emptyPrepareApplyOp.getOpTime();

    // Apply a batch with only the prepare applyOps. This should result in the prepare being put in
    // the oplog, and the nested insert being applied (but in a transaction).
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), emptyPrepareApplyOp.getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {emptyPrepareApplyOp}));
    ASSERT_EQ(1U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(emptyPrepareApplyOp.getEntry().toBSON(), oplogDocs().back());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  emptyPrepareApplyOp.getOpTime(),
                  emptyPrepareApplyOp.getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit.  This should result in the commit being put in the
    // oplog, and prepared insert being committed.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _commitSinglePrepareApplyOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_commitSinglePrepareApplyOp}));
    ASSERT_BSONOBJ_EQ(_commitSinglePrepareApplyOp->getEntry().toBSON(), oplogDocs().back());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitSinglePrepareApplyOp->getOpTime(),
                  _commitSinglePrepareApplyOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest, MultiApplyAbortSingleApplyOpsPreparedTransaction) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        _writerPool.get());

    const auto expectedStartOpTime = _singlePrepareApplyOp->getOpTime();
    // Apply a batch with only the prepare applyOps. This should result in the prepare being put in
    // the oplog, and the nested insert being applied (but in a transaction).
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _singlePrepareApplyOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_singlePrepareApplyOp}));
    checkTxnTable(_lsid,
                  _txnNum,
                  _singlePrepareApplyOp->getOpTime(),
                  _singlePrepareApplyOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the abort.  This should result in the abort being put in the
    // oplog and the transaction table being updated accordingly.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _abortSinglePrepareApplyOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_abortSinglePrepareApplyOp}));
    ASSERT_BSONOBJ_EQ(_abortSinglePrepareApplyOp->getEntry().toBSON(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _abortSinglePrepareApplyOp->getOpTime(),
                  _abortSinglePrepareApplyOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kAborted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest,
       MultiApplySingleApplyOpsPreparedTransactionInitialSync) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kInitialSync),
        _writerPool.get());

    const auto expectedStartOpTime = _singlePrepareApplyOp->getOpTime();

    // Apply a batch with only the prepare applyOps. This should result in the prepare being put in
    // the oplog, but, since this is initial sync, nothing else.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _singlePrepareApplyOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_singlePrepareApplyOp}));
    ASSERT_EQ(1U, oplogDocs().size());
    ASSERT_BSONOBJ_EQ(_singlePrepareApplyOp->getEntry().toBSON(), oplogDocs().back());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _singlePrepareApplyOp->getOpTime(),
                  _singlePrepareApplyOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit.  This should result in the commit being put in the
    // oplog, and the previous entry being applied.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _commitSinglePrepareApplyOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_commitSinglePrepareApplyOp}));
    ASSERT_BSONOBJ_EQ(_commitSinglePrepareApplyOp->getEntry().toBSON(), oplogDocs().back());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitSinglePrepareApplyOp->getOpTime(),
                  _commitSinglePrepareApplyOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

TEST_F(MultiOplogEntryPreparedTransactionTest,
       MultiApplySingleApplyOpsPreparedTransactionRecovery) {
    // For recovery, the oplog must contain the operations before starting.
    for (auto&& entry : {*_singlePrepareApplyOp, *_commitPrepareWithPrevOp}) {
        ASSERT_OK(getStorageInterface()->insertDocument(
            _opCtx.get(),
            NamespaceString::kRsOplogNamespace,
            {entry.getEntry().toBSON(), entry.getOpTime().getTimestamp()},
            entry.getOpTime().getTerm()));
    }
    // Ignore docs inserted into oplog in setup.
    oplogDocs().clear();

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kRecovering),
        _writerPool.get());

    const auto expectedStartOpTime = _singlePrepareApplyOp->getOpTime();

    // Apply a batch with only the prepare applyOps. This should have no effect, since this is
    // recovery.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _singlePrepareApplyOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_singlePrepareApplyOp}));
    ASSERT_TRUE(oplogDocs().empty());
    ASSERT_TRUE(_insertedDocs[_nss1].empty());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _singlePrepareApplyOp->getOpTime(),
                  _singlePrepareApplyOp->getWallClockTime(),
                  expectedStartOpTime,
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit.  This should result in the previous entry being
    // applied.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _commitSinglePrepareApplyOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_commitSinglePrepareApplyOp}));
    ASSERT_TRUE(oplogDocs().empty());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_TRUE(_insertedDocs[_nss2].empty());
    checkTxnTable(_lsid,
                  _txnNum,
                  _commitSinglePrepareApplyOp->getOpTime(),
                  _commitSinglePrepareApplyOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
}

void testWorkerMultikeyPaths(OperationContext* opCtx,
                             const OplogEntry& op,
                             unsigned long numPaths) {
    TestApplyOplogGroupApplier oplogApplier(
        nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kSecondary));
    WorkerMultikeyPathInfo pathInfo;
    std::vector<const OplogEntry*> ops = {&op};
    const bool dataIsConsistent = true;
    ASSERT_OK(oplogApplier.applyOplogBatchPerWorker(opCtx, &ops, &pathInfo, dataIsConsistent));
    ASSERT_EQ(pathInfo.size(), numPaths);
}

TEST_F(OplogApplierImplTest, OplogApplicationThreadFuncAddsWorkerMultikeyPathInfoOnInsert) {
    // Set the state as secondary as we are going to apply createIndexes oplog entry.
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_SECONDARY));

    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName());

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

TEST_F(OplogApplierImplTest, OplogApplicationThreadFuncAddsMultipleWorkerMultikeyPathInfo) {
    // Set the state as secondary as we are going to apply createIndexes oplog entry.
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_SECONDARY));

    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName());

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

        TestApplyOplogGroupApplier oplogApplier(
            nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kSecondary));
        WorkerMultikeyPathInfo pathInfo;
        std::vector<const OplogEntry*> ops = {&opA, &opB};
        const bool dataIsConsistent = true;
        ASSERT_OK(
            oplogApplier.applyOplogBatchPerWorker(_opCtx.get(), &ops, &pathInfo, dataIsConsistent));
        ASSERT_EQ(pathInfo.size(), 2UL);
    }
}

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncDoesNotAddWorkerMultikeyPathInfoOnCreateIndex) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));
    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName());

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

TEST_F(OplogApplierImplTest, OplogApplicationThreadFuncFailsWhenCollectionCreationTriesToMakeUUID) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_SECONDARY));
    NamespaceString nss("foo." + _agent.getSuiteName() + "_" + _agent.getTestName());

    auto op = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss);

    TestApplyOplogGroupApplier oplogApplier(
        nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kSecondary));
    std::vector<const OplogEntry*> ops = {&op};
    const bool dataIsConsistent = true;
    ASSERT_EQUALS(
        ErrorCodes::InvalidOptions,
        oplogApplier.applyOplogBatchPerWorker(_opCtx.get(), &ops, nullptr, dataIsConsistent));
}

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncDisablesDocumentValidationWhileApplyingOperations) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn =
        [&](OperationContext* opCtx, const NamespaceString&, const std::vector<BSONObj>&) {
            onInsertsCalled = true;
            ASSERT_FALSE(opCtx->writesAreReplicated());
            ASSERT_FALSE(opCtx->lockState()->shouldConflictWithSecondaryBatchApplication());
            ASSERT_TRUE(DocumentValidationSettings::get(opCtx).isSchemaValidationDisabled());
            return Status::OK();
        };
    createCollectionWithUuid(_opCtx.get(), nss);
    auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0));
    ASSERT_OK(runOpSteadyState(op));
    ASSERT(onInsertsCalled);
}

TEST_F(
    OplogApplierImplTest,
    OplogApplicationThreadFuncPassesThroughApplyOplogEntryOrGroupedInsertsErrorAfterFailingToApplyOperation) {
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    // Delete operation without _id in 'o' field.
    auto op = makeDeleteDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, {});
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, runOpSteadyState(op));
}

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncPassesThroughApplyOplogEntryOrGroupedInsertsException) {
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

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncSortsOperationsStablyByNamespaceBeforeApplying) {
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

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncGroupsInsertOperationByNamespaceBeforeApplying) {
    int seconds = 1;
    auto makeOp = [&seconds](const NamespaceString& nss) {
        auto t = seconds++;
        return makeInsertDocumentOplogEntry({Timestamp(Seconds(t), 0), 1LL}, nss, BSON("_id" << t));
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

    std::vector<OplogEntry> ops = {
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

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncLimitsBatchCountWhenGroupingInsertOperation) {
    int seconds = 1;
    auto makeOp = [&seconds](const NamespaceString& nss) {
        auto t = seconds++;
        return makeInsertDocumentOplogEntry({Timestamp(Seconds(t), 0), 1LL}, nss, BSON("_id" << t));
    };
    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName() + "_1");
    auto createOp = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss);

    // Generate operations to apply:
    // {create}, {insert_1}, {insert_2}, .. {insert_(limit)}, {insert_(limit+1)}
    std::size_t limit = 64;
    std::vector<OplogEntry> insertOps;
    for (std::size_t i = 0; i < limit + 1; ++i) {
        insertOps.push_back(makeOp(nss));
    }
    std::vector<OplogEntry> operationsToApply;
    operationsToApply.push_back(createOp);
    std::copy(insertOps.begin(), insertOps.end(), std::back_inserter(operationsToApply));

    // Each element in 'docsInserted' is a grouped insert operation.
    std::vector<std::vector<BSONObj>> docsInserted;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            docsInserted.push_back(docs);
        };

    ASSERT_OK(runOpsSteadyState(operationsToApply));

    // applyOplogBatchPerWorker should combine operations as follows:
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

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncLimitsBatchSizeWhenGroupingInsertOperations) {
    int seconds = 1;
    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto createOp = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss);

    // Create a sequence of insert ops that are too large to fit in one group.
    int maxBatchSize = write_ops::insertVectorMaxBytes;
    int opsPerBatch = 3;
    int opSize = maxBatchSize / opsPerBatch - 500;  // Leave some room for other oplog fields.

    // Create the insert ops.
    std::vector<OplogEntry> insertOps;
    int numOps = 4;
    for (int i = 0; i < numOps; i++) {
        insertOps.push_back(makeSizedInsertOp(nss, opSize, seconds++));
    }

    std::vector<OplogEntry> operationsToApply;
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

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncAppliesOpIndividuallyWhenOpIndividuallyExceedsBatchSize) {
    int seconds = 1;
    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName());
    auto createOp = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss);

    int maxBatchSize = write_ops::insertVectorMaxBytes;
    // Create an insert op that exceeds the maximum batch size by itself.
    auto insertOpLarge = makeSizedInsertOp(nss, maxBatchSize, seconds++);
    auto insertOpSmall = makeSizedInsertOp(nss, 100, seconds++);

    std::vector<OplogEntry> operationsToApply = {createOp, insertOpLarge, insertOpSmall};

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

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncAppliesInsertOpsIndividuallyWhenUnableToCreateGroupByNamespace) {
    int seconds = 1;
    auto makeOp = [&seconds](const NamespaceString& nss) {
        auto t = seconds++;
        return makeInsertDocumentOplogEntry({Timestamp(Seconds(t), 0), 1LL}, nss, BSON("_id" << t));
    };

    auto testNs = "test." + _agent.getSuiteName() + "_" + _agent.getTestName();

    // Create a sequence of 3 'insert' ops that can't be grouped because they are from different
    // namespaces.
    std::vector<OplogEntry> operationsToApply = {makeOp(NamespaceString(testNs + "_1")),
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

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncFallsBackOnApplyingInsertsIndividuallyWhenGroupedInsertFails) {
    int seconds = 1;
    auto makeOp = [&seconds](const NamespaceString& nss) {
        auto t = seconds++;
        return makeInsertDocumentOplogEntry({Timestamp(Seconds(t), 0), 1LL}, nss, BSON("_id" << t));
    };
    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName() + "_1");
    auto createOp = makeCreateCollectionOplogEntry({Timestamp(Seconds(seconds++), 0), 1LL}, nss);

    // Generate operations to apply:
    // {create}, {insert_1}, {insert_2}, .. {insert_(limit)}, {insert_(limit+1)}
    std::size_t limit = 64;
    std::vector<OplogEntry> insertOps;
    for (std::size_t i = 0; i < limit + 1; ++i) {
        insertOps.push_back(makeOp(nss));
    }
    std::vector<OplogEntry> operationsToApply;
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

    // On failing to apply the grouped insert operation, applyOplogBatchPerWorker should
    // apply the operations as given in "operationsToApply": {create}, {insert_1}, {insert_2}, ..
    // {insert_(limit)}, {insert_(limit+1)} Ignore {create} since we are only tracking inserts.
    ASSERT_EQUALS(limit + 1, docsInserted.size());

    for (std::size_t i = 0; i < limit + 1; ++i) {
        const auto& insertOp = insertOps[i];
        const auto& group = docsInserted[i];
        ASSERT_EQUALS(1U, group.size()) << i;
        ASSERT_BSONOBJ_EQ(insertOp.getObject(), group[0]);
    }

    // Ensure that applyOplogBatchPerWorker does not attempt to group remaining operations
    // in first failed grouped insert operation.
    ASSERT_EQUALS(1U, numFailedGroupedInserts);
}

TEST_F(OplogApplierImplTest, ApplyGroupIgnoresUpdateOperationIfDocumentIsMissingFromSyncSource) {
    TestApplyOplogGroupApplier oplogApplier(
        nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kInitialSync));
    NamespaceString nss("test.t");
    {
        Lock::GlobalWrite globalLock(_opCtx.get());
        bool justCreated = false;
        auto databaseHolder = DatabaseHolder::get(_opCtx.get());
        auto db = databaseHolder->openDb(_opCtx.get(), nss.dbName(), &justCreated);
        ASSERT_TRUE(db);
        ASSERT_TRUE(justCreated);
    }
    auto op = makeUpdateDocumentOplogEntry(
        {Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0), BSON("_id" << 0 << "x" << 2));
    std::vector<const OplogEntry*> ops = {&op};
    WorkerMultikeyPathInfo pathInfo;
    const bool dataIsConsistent = true;
    ASSERT_OK(
        oplogApplier.applyOplogBatchPerWorker(_opCtx.get(), &ops, &pathInfo, dataIsConsistent));

    // Since the document was missing when we cloned data from the sync source, the collection
    // referenced by the failed operation should not be automatically created.
    ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx.get(), nss).getCollection());
}

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncSkipsDocumentOnNamespaceNotFoundDuringInitialSync) {
    BSONObj emptyDoc;
    TestApplyOplogGroupApplier oplogApplier(
        nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kInitialSync));
    NamespaceString nss("local." + _agent.getSuiteName() + "_" + _agent.getTestName());
    NamespaceString badNss("local." + _agent.getSuiteName() + "_" + _agent.getTestName() + "bad");
    auto doc1 = BSON("_id" << 1);
    auto doc2 = BSON("_id" << 2);
    auto doc3 = BSON("_id" << 3);
    auto op0 = makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss);
    auto op1 = makeInsertDocumentOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss, doc1);
    auto op2 = makeInsertDocumentOplogEntry({Timestamp(Seconds(3), 0), 1LL}, badNss, doc2);
    auto op3 = makeInsertDocumentOplogEntry({Timestamp(Seconds(4), 0), 1LL}, nss, doc3);
    std::vector<const OplogEntry*> ops = {&op0, &op1, &op2, &op3};
    WorkerMultikeyPathInfo pathInfo;
    const bool dataIsConsistent = true;
    ASSERT_OK(
        oplogApplier.applyOplogBatchPerWorker(_opCtx.get(), &ops, &pathInfo, dataIsConsistent));

    CollectionReader collectionReader(_opCtx.get(), nss);
    ASSERT_BSONOBJ_EQ(doc1, unittest::assertGet(collectionReader.next()));
    ASSERT_BSONOBJ_EQ(doc3, unittest::assertGet(collectionReader.next()));
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, collectionReader.next().getStatus());
}

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncSkipsIndexCreationOnNamespaceNotFoundDuringInitialSync) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));
    BSONObj emptyDoc;
    TestApplyOplogGroupApplier oplogApplier(
        nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kInitialSync));
    NamespaceString nss("test." + _agent.getSuiteName() + "_" + _agent.getTestName());
    NamespaceString badNss("test." + _agent.getSuiteName() + "_" + _agent.getTestName() + "bad");
    auto doc1 = BSON("_id" << 1);
    auto keyPattern = BSON("a" << 1);
    auto doc3 = BSON("_id" << 3);
    auto op0 =
        makeCreateCollectionOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, BSON("uuid" << kUuid));
    auto op1 = makeInsertDocumentOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss, doc1);
    auto op2 = makeCreateIndexOplogEntry(
        {Timestamp(Seconds(3), 0), 1LL}, badNss, "a_1", keyPattern, kUuid);
    auto op3 = makeInsertDocumentOplogEntry({Timestamp(Seconds(4), 0), 1LL}, nss, doc3);
    std::vector<const OplogEntry*> ops = {&op0, &op1, &op2, &op3};
    WorkerMultikeyPathInfo pathInfo;
    const bool dataIsConsistent = true;
    ASSERT_OK(
        oplogApplier.applyOplogBatchPerWorker(_opCtx.get(), &ops, &pathInfo, dataIsConsistent));

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
    auto updateOp = update(1,
                           update_oplog_entry::makeDeltaOplogEntry(BSON(
                               doc_diff::kUpdateSectionFieldName << fromjson("{loc: [1, 2]}"))));
    auto indexOp =
        buildIndex(fromjson("{loc: '2dsphere'}"), BSON("2dsphereIndexVersion" << 3), kUuid);

    auto ops = {insertOp, updateOp, indexOp};
    testOpsAreIdempotent(ops);

    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync(ops);
    ASSERT_EQ(status.code(), 16755);
}

TEST_F(IdempotencyTest, Geo2dsphereIndex) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));
    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto indexOp =
        buildIndex(fromjson("{loc: '2dsphere'}"), BSON("2dsphereIndexVersion" << 3), kUuid);
    auto dropIndexOp = dropIndex("loc_index", kUuid);
    auto insertOp = insert(fromjson("{_id: 1, loc: 'hi'}"));

    auto ops = {indexOp, dropIndexOp, insertOp};
    testOpsAreIdempotent(ops);
}

TEST_F(IdempotencyTest, Geo2dIndex) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));
    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto insertOp = insert(fromjson("{_id: 1, loc: [1]}"));
    auto updateOp = update(1,
                           update_oplog_entry::makeDeltaOplogEntry(BSON(
                               doc_diff::kUpdateSectionFieldName << fromjson("{loc: [1, 2]}"))));
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
    auto updateOp = update(1,
                           update_oplog_entry::makeDeltaOplogEntry(
                               BSON(doc_diff::kUpdateSectionFieldName << fromjson("{x: 6}"))));
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

    auto updateOp1 = update(1,
                            update_oplog_entry::makeDeltaOplogEntry(BSON(
                                doc_diff::kUpdateSectionFieldName << fromjson("{x: [1, 2]}"))));
    auto updateOp2 = update(1,
                            update_oplog_entry::makeDeltaOplogEntry(
                                BSON(doc_diff::kUpdateSectionFieldName << fromjson("{x: 1}"))));
    auto updateOp3 = update(1,
                            update_oplog_entry::makeDeltaOplogEntry(BSON(
                                doc_diff::kUpdateSectionFieldName << fromjson("{y: [3, 4]}"))));

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
}

TEST_F(IdempotencyTest, TextIndexDocumentHasNonStringLanguageField) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto insertOp = insert(fromjson("{_id: 1, x: 'words to index', language: 1}"));
    auto updateOp =
        update(1,
               update_oplog_entry::makeDeltaOplogEntry(
                   BSON(doc_diff::kDeleteSectionFieldName << fromjson("{language: false}"))));
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
}

TEST_F(IdempotencyTest, TextIndexDocumentHasNonStringLanguageOverrideField) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto insertOp = insert(fromjson("{_id: 1, x: 'words to index', y: 1}"));
    auto updateOp = update(1,
                           update_oplog_entry::makeDeltaOplogEntry(
                               BSON(doc_diff::kDeleteSectionFieldName << fromjson("{y: false}"))));
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
}

TEST_F(IdempotencyTest, TextIndexDocumentHasUnknownLanguage) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto insertOp = insert(fromjson("{_id: 1, x: 'words to index', language: 'bad'}"));
    auto updateOp =
        update(1,
               update_oplog_entry::makeDeltaOplogEntry(
                   BSON(doc_diff::kDeleteSectionFieldName << fromjson("{language: false}"))));
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
    UUID uuid = UUID::gen();

    auto runOpsAndValidate = [this, uuid]() {
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
        auto insertOp1 = insert(fromjson("{ _id: 'foo' }"));
        auto insertOp2 = insert(fromjson("{ _id: 'Foo', x: 1 }"));
        auto updateOp = update("foo",
                               update_oplog_entry::makeDeltaOplogEntry(
                                   BSON(doc_diff::kUpdateSectionFieldName << fromjson("{x: 2}"))));

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

DEATH_TEST_F(IdempotencyTest, CannotCreateIndexForApplyOpsOnPrimary, "invariant") {
    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    auto indexOp = buildIndex(fromjson("{x: 'text'}"), BSONObj(), kUuid);
    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));
    auto status = runOpsInitialSync({indexOp});
}

TEST_F(OplogApplierImplTest, FailOnDropFCVCollection) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto fcvNS(NamespaceString::kServerConfigurationNamespace);
    auto cmd = BSON("drop" << fcvNS.coll());
    auto op = makeCommandOplogEntry(nextOpTime(), fcvNS, cmd);
    ASSERT_EQUALS(runOpInitialSync(op), ErrorCodes::OplogOperationUnsupported);
}

TEST_F(OplogApplierImplTest, FailOnInsertFCVDocument) {
    auto fcvNS(NamespaceString::kServerConfigurationNamespace);
    ::mongo::repl::createCollection(_opCtx.get(), fcvNS, CollectionOptions());
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto op = makeInsertDocumentOplogEntry(
        nextOpTime(), fcvNS, BSON("_id" << multiversion::kParameterName));
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

TEST_F(OplogApplierImplTest, DropDatabaseSucceedsInRecovering) {
    // Choose `system.profile` so the storage engine doesn't expect the drop to be timestamped.
    auto ns = NamespaceString("foo.system.profile");
    ::mongo::repl::createCollection(_opCtx.get(), ns, CollectionOptions());
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto op = makeCommandOplogEntry(nextOpTime(), ns, BSON("dropDatabase" << 1));
    ASSERT_OK(runOpSteadyState(op));
}

TEST_F(OplogApplierImplWithFastAutoAdvancingClockTest, LogSlowOpApplicationWhenSuccessful) {
    // This duration is greater than "slowMS", so the op would be considered slow.
    auto applyDuration = serverGlobalParams.slowMS * 10;

    // We are inserting into an existing collection.
    const NamespaceString nss("test.t");
    repl::createCollection(_opCtx.get(), nss, {});
    auto entry = makeOplogEntry(OpTypeEnum::kInsert, nss, {});

    startCapturingLogMessages();
    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), &entry, OplogApplication::Mode::kSecondary));

    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON(
            "attr" << BSON("CRUD" << BSON("ts" << Timestamp(1, 1) << "t" << 1LL << "v" << 2 << "op"
                                               << "i"
                                               << "ns"
                                               << "test.t"
                                               << "wall" << Date_t::fromMillisSinceEpoch(0))
                                  << "durationMillis" << applyDuration))));
}

TEST_F(OplogApplierImplWithFastAutoAdvancingClockTest, DoNotLogSlowOpApplicationWhenFailed) {
    // This duration is greater than "slowMS", so the op would be considered slow.
    auto applyDuration = serverGlobalParams.slowMS * 10;

    // We are trying to insert into a non-existing database.
    NamespaceString nss("test.t");
    auto entry = makeOplogEntry(OpTypeEnum::kInsert, nss, {});

    startCapturingLogMessages();
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), &entry, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);

    // Use a builder for easier escaping. We expect the operation to *not* be logged
    // even thought it was slow, since we couldn't apply it successfully.
    StringBuilder expected;
    expected << "applied op: CRUD { op: \"i\", ns: \"test.t\", o: { _id: 0 }, ts: Timestamp(1, 1), "
                "t: 1, h: 1, v: 2 }, took "
             << applyDuration << "ms";
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining(expected.str()));
}

TEST_F(OplogApplierImplWithSlowAutoAdvancingClockTest, DoNotLogNonSlowOpApplicationWhenSuccessful) {
    // This duration is below "slowMS", so the op would *not* be considered slow.
    auto applyDuration = serverGlobalParams.slowMS / 10;

    // We are inserting into an existing collection.
    const NamespaceString nss("test.t");
    repl::createCollection(_opCtx.get(), nss, {});
    auto entry = makeOplogEntry(OpTypeEnum::kInsert, nss, {});

    startCapturingLogMessages();
    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), &entry, OplogApplication::Mode::kSecondary));

    // Use a builder for easier escaping. We expect the operation to *not* be logged,
    // since it wasn't slow to apply.
    StringBuilder expected;
    expected << "applied op: CRUD { op: \"i\", ns: \"test.t\", o: { _id: 0 }, ts: Timestamp(1, 1), "
                "t: 1, h: 1, v: 2 }, took "
             << applyDuration << "ms";
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining(expected.str()));
}

class OplogApplierImplTxnTableTest : public OplogApplierImplTest {
public:
    void setUp() override {
        OplogApplierImplTest::setUp();

        // This fixture sets up some replication, but notably omits installing an
        // OpObserverImpl. This state causes collection creation to timestamp catalog writes, but
        // secondary index creation does not. We use an UnreplicatedWritesBlock to avoid
        // timestamping any of the catalog setup.
        repl::UnreplicatedWritesBlock noRep(_opCtx.get());
        MongoDSessionCatalog::set(
            _opCtx->getServiceContext(),
            std::make_unique<MongoDSessionCatalog>(
                std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(_opCtx.get());
        mongoDSessionCatalog->onStepUp(_opCtx.get());

        DBDirectClient client(_opCtx.get());
        BSONObj result;
        ASSERT(client.runCommand(kNs.db().toString(), BSON("create" << kNs.coll()), result));
    }

    /**
     * Creates an OplogEntry with given parameters and preset defaults for this test suite.
     */
    repl::OplogEntry makeOplogEntry(const NamespaceString& ns,
                                    repl::OpTime opTime,
                                    repl::OpTypeEnum opType,
                                    BSONObj object,
                                    boost::optional<BSONObj> object2,
                                    const OperationSessionInfo& sessionInfo,
                                    Date_t wallClockTime) {
        return {repl::DurableOplogEntry(
            opTime,         // optime
            opType,         // opType
            ns,             // namespace
            boost::none,    // uuid
            boost::none,    // fromMigrate
            0,              // version
            object,         // o
            object2,        // o2
            sessionInfo,    // sessionInfo
            boost::none,    // false
            wallClockTime,  // wall clock time
            {},             // statement ids
            boost::none,    // optime of previous write within same transaction
            boost::none,    // pre-image optime
            boost::none,    // post-image optime
            boost::none,    // ShardId of resharding recipient
            boost::none,    // _id
            boost::none)};  // needsRetryImage
    }

    /**
     * Creates an OplogEntry with given parameters and preset defaults for this test suite.
     */
    repl::OplogEntry makeOplogEntryForMigrate(const NamespaceString& ns,
                                              repl::OpTime opTime,
                                              repl::OpTypeEnum opType,
                                              BSONObj object,
                                              boost::optional<BSONObj> object2,
                                              const OperationSessionInfo& sessionInfo,
                                              Date_t wallClockTime) {
        return {repl::DurableOplogEntry(
            opTime,         // optime
            opType,         // opType
            ns,             // namespace
            boost::none,    // uuid
            true,           // fromMigrate
            0,              // version
            object,         // o
            object2,        // o2
            sessionInfo,    // sessionInfo
            boost::none,    // false
            wallClockTime,  // wall clock time
            {},             // statement ids
            boost::none,    // optime of previous write within same transaction
            boost::none,    // pre-image optime
            boost::none,    // post-image optime
            boost::none,    // ShardId of resharding recipient
            boost::none,    // _id
            boost::none)};  // needsRetryImage
    }

    void checkTxnTable(const OperationSessionInfo& sessionInfo,
                       const repl::OpTime& expectedOpTime,
                       Date_t expectedWallClock) {
        invariant(sessionInfo.getSessionId());
        invariant(sessionInfo.getTxnNumber());

        repl::checkTxnTable(_opCtx.get(),
                            *sessionInfo.getSessionId(),
                            *sessionInfo.getTxnNumber(),
                            expectedOpTime,
                            expectedWallClock,
                            {},
                            {});
    }

    static const NamespaceString& nss() {
        return kNs;
    }

private:
    static const NamespaceString kNs;
};

const NamespaceString OplogApplierImplTxnTableTest::kNs("test.foo");

TEST_F(OplogApplierImplTxnTableTest, SimpleWriteWithTxn) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    const auto date = Date_t::now();

    auto insertOp = makeOplogEntry(nss(),
                                   {Timestamp(1, 0), 1},
                                   repl::OpTypeEnum::kInsert,
                                   BSON("_id" << 1),
                                   boost::none,
                                   sessionInfo,
                                   date);

    auto writerPool = makeReplWriterPool();

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {insertOp}));

    checkTxnTable(sessionInfo, {Timestamp(1, 0), 1}, date);
}

TEST_F(OplogApplierImplTxnTableTest, WriteWithTxnMixedWithDirectWriteToTxnTable) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    const auto date = Date_t::now();

    auto insertOp = makeOplogEntry(nss(),
                                   {Timestamp(1, 0), 1},
                                   repl::OpTypeEnum::kInsert,
                                   BSON("_id" << 1),
                                   boost::none,
                                   sessionInfo,
                                   date);

    auto deleteOp = makeOplogEntry(NamespaceString::kSessionTransactionsTableNamespace,
                                   {Timestamp(2, 0), 1},
                                   repl::OpTypeEnum::kDelete,
                                   BSON("_id" << sessionInfo.getSessionId()->toBSON()),
                                   boost::none,
                                   {},
                                   Date_t::now());

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());


    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {insertOp, deleteOp}));

    ASSERT_FALSE(docExists(
        _opCtx.get(),
        NamespaceString::kSessionTransactionsTableNamespace,
        BSON(SessionTxnRecord::kSessionIdFieldName << sessionInfo.getSessionId()->toBSON())));
}

TEST_F(OplogApplierImplTxnTableTest, InterleavedWriteWithTxnMixedWithDirectDeleteToTxnTable) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    auto date = Date_t::now();

    auto insertOp = makeOplogEntry(nss(),
                                   {Timestamp(1, 0), 1},
                                   repl::OpTypeEnum::kInsert,
                                   BSON("_id" << 1),
                                   boost::none,
                                   sessionInfo,
                                   date);

    auto deleteOp = makeOplogEntry(NamespaceString::kSessionTransactionsTableNamespace,
                                   {Timestamp(2, 0), 1},
                                   repl::OpTypeEnum::kDelete,
                                   BSON("_id" << sessionInfo.getSessionId()->toBSON()),
                                   boost::none,
                                   {},
                                   Date_t::now());

    date = Date_t::now();
    sessionInfo.setTxnNumber(7);
    auto insertOp2 = makeOplogEntry(nss(),
                                    {Timestamp(3, 0), 2},
                                    repl::OpTypeEnum::kInsert,
                                    BSON("_id" << 6),
                                    boost::none,
                                    sessionInfo,
                                    date);

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {insertOp, deleteOp, insertOp2}));

    checkTxnTable(sessionInfo, {Timestamp(3, 0), 2}, date);
}

TEST_F(OplogApplierImplTxnTableTest, InterleavedWriteWithTxnMixedWithDirectUpdateToTxnTable) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    auto date = Date_t::now();

    auto insertOp = makeOplogEntry(nss(),
                                   {Timestamp(1, 0), 1},
                                   repl::OpTypeEnum::kInsert,
                                   BSON("_id" << 1),
                                   boost::none,
                                   sessionInfo,
                                   date);

    repl::OpTime newWriteOpTime(Timestamp(2, 0), 1);
    auto updateOp = makeOplogEntry(
        NamespaceString::kSessionTransactionsTableNamespace,
        {Timestamp(4, 0), 1},
        repl::OpTypeEnum::kUpdate,
        update_oplog_entry::makeDeltaOplogEntry(
            BSON(doc_diff::kUpdateSectionFieldName << BSON("lastWriteOpTime" << newWriteOpTime))),
        BSON("_id" << sessionInfo.getSessionId()->toBSON()),
        {},
        Date_t::now());

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {insertOp, updateOp}));

    checkTxnTable(sessionInfo, newWriteOpTime, date);
}

TEST_F(OplogApplierImplTxnTableTest, RetryableWriteThenMultiStatementTxnWriteOnSameSession) {
    const NamespaceString cmdNss{"admin", "$cmd"};
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    auto date = Date_t::now();
    auto uuid = [&] {
        return AutoGetCollectionForRead(_opCtx.get(), nss()).getCollection()->uuid();
    }();

    repl::OpTime retryableInsertOpTime(Timestamp(1, 0), 1);

    auto retryableInsertOp = makeOplogEntry(nss(),
                                            retryableInsertOpTime,
                                            repl::OpTypeEnum::kInsert,
                                            BSON("_id" << 1),
                                            boost::none,
                                            sessionInfo,
                                            date);

    repl::OpTime txnInsertOpTime(Timestamp(2, 0), 1);
    sessionInfo.setTxnNumber(4);

    auto txnInsertOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
        txnInsertOpTime,
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << nss().ns() << "ui" << uuid << "o"
                                           << BSON("_id" << 2)))
                        << "partialTxn" << true),
        sessionId,
        *sessionInfo.getTxnNumber(),
        {StmtId(0)},
        OpTime());

    repl::OpTime txnCommitOpTime(Timestamp(3, 0), 1);
    auto txnCommitOp =
        makeCommandOplogEntryWithSessionInfoAndStmtIds(txnCommitOpTime,
                                                       cmdNss,
                                                       BSON("applyOps" << BSONArray()),
                                                       sessionId,
                                                       *sessionInfo.getTxnNumber(),
                                                       {StmtId(1)},
                                                       txnInsertOpTime);

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(
        oplogApplier.applyOplogBatch(_opCtx.get(), {retryableInsertOp, txnInsertOp, txnCommitOp}));

    repl::checkTxnTable(_opCtx.get(),
                        *sessionInfo.getSessionId(),
                        *sessionInfo.getTxnNumber(),
                        txnCommitOpTime,
                        txnCommitOp.getWallClockTime(),
                        boost::none,
                        DurableTxnStateEnum::kCommitted);
}

TEST_F(OplogApplierImplTxnTableTest, MultiStatementTxnWriteThenRetryableWriteOnSameSession) {
    const NamespaceString cmdNss{"admin", "$cmd"};
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    auto date = Date_t::now();
    auto uuid = [&] {
        return AutoGetCollectionForRead(_opCtx.get(), nss()).getCollection()->uuid();
    }();

    repl::OpTime txnInsertOpTime(Timestamp(1, 0), 1);
    auto txnInsertOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
        txnInsertOpTime,
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << nss().ns() << "ui" << uuid << "o"
                                           << BSON("_id" << 2)))
                        << "partialTxn" << true),
        sessionId,
        *sessionInfo.getTxnNumber(),
        {StmtId(0)},
        OpTime());

    repl::OpTime txnCommitOpTime(Timestamp(2, 0), 1);
    auto txnCommitOp =
        makeCommandOplogEntryWithSessionInfoAndStmtIds(txnCommitOpTime,
                                                       cmdNss,
                                                       BSON("applyOps" << BSONArray()),
                                                       sessionId,
                                                       *sessionInfo.getTxnNumber(),
                                                       {StmtId(1)},
                                                       txnInsertOpTime);

    repl::OpTime retryableInsertOpTime(Timestamp(3, 0), 1);
    sessionInfo.setTxnNumber(4);

    auto retryableInsertOp = makeOplogEntry(nss(),
                                            retryableInsertOpTime,
                                            repl::OpTypeEnum::kInsert,
                                            BSON("_id" << 1),
                                            boost::none,
                                            sessionInfo,
                                            date);

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(
        oplogApplier.applyOplogBatch(_opCtx.get(), {txnInsertOp, txnCommitOp, retryableInsertOp}));

    repl::checkTxnTable(_opCtx.get(),
                        *sessionInfo.getSessionId(),
                        *sessionInfo.getTxnNumber(),
                        retryableInsertOpTime,
                        retryableInsertOp.getWallClockTime(),
                        boost::none,
                        boost::none);
}


TEST_F(OplogApplierImplTxnTableTest, MultiApplyUpdatesTheTransactionTable) {
    NamespaceString ns0("test.0");
    NamespaceString ns1("test.1");
    NamespaceString ns2("test.2");
    NamespaceString ns3("test.3");

    DBDirectClient client(_opCtx.get());
    BSONObj result;
    ASSERT(client.runCommand(ns0.db().toString(), BSON("create" << ns0.coll()), result));
    ASSERT(client.runCommand(ns1.db().toString(), BSON("create" << ns1.coll()), result));
    ASSERT(client.runCommand(ns2.db().toString(), BSON("create" << ns2.coll()), result));
    ASSERT(client.runCommand(ns3.db().toString(), BSON("create" << ns3.coll()), result));
    auto uuid0 = [&] {
        return AutoGetCollectionForRead(_opCtx.get(), ns0).getCollection()->uuid();
    }();
    auto uuid1 = [&] {
        return AutoGetCollectionForRead(_opCtx.get(), ns1).getCollection()->uuid();
    }();
    auto uuid2 = [&] {
        return AutoGetCollectionForRead(_opCtx.get(), ns2).getCollection()->uuid();
    }();

    // Entries with a session id and a txnNumber update the transaction table.
    auto lsidSingle = makeLogicalSessionIdForTest();
    auto opSingle = makeInsertDocumentOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(1), 0), 1LL}, ns0, uuid0, BSON("_id" << 0), lsidSingle, 5LL, {0});

    // For entries with the same session, the entry with a larger txnNumber is saved.
    auto lsidDiffTxn = makeLogicalSessionIdForTest();
    auto opDiffTxnSmaller = makeInsertDocumentOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(2), 0), 1LL}, ns1, uuid1, BSON("_id" << 0), lsidDiffTxn, 10LL, {1});
    auto opDiffTxnLarger = makeInsertDocumentOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(3), 0), 1LL}, ns1, uuid1, BSON("_id" << 1), lsidDiffTxn, 20LL, {1});

    // For entries with the same session and txnNumber, the later optime is saved.
    auto lsidSameTxn = makeLogicalSessionIdForTest();
    auto opSameTxnLater = makeInsertDocumentOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(6), 0), 1LL}, ns2, uuid2, BSON("_id" << 0), lsidSameTxn, 30LL, {0});
    auto opSameTxnSooner = makeInsertDocumentOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(5), 0), 1LL}, ns2, uuid2, BSON("_id" << 1), lsidSameTxn, 30LL, {1});

    // Entries with a session id but no txnNumber do not lead to updates.
    auto lsidNoTxn = makeLogicalSessionIdForTest();
    OperationSessionInfo info;
    info.setSessionId(lsidNoTxn);
    auto opNoTxn = makeInsertDocumentOplogEntryWithSessionInfo(
        {Timestamp(Seconds(7), 0), 1LL}, ns3, BSON("_id" << 0), info);

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.applyOplogBatch(
        _opCtx.get(),
        {opSingle, opDiffTxnSmaller, opDiffTxnLarger, opSameTxnSooner, opSameTxnLater, opNoTxn}));

    // The txnNum and optime of the only write were saved.
    auto resultSingleDoc =
        client.findOne(NamespaceString::kSessionTransactionsTableNamespace,
                       BSON(SessionTxnRecord::kSessionIdFieldName << lsidSingle.toBSON()));
    ASSERT_TRUE(!resultSingleDoc.isEmpty());

    auto resultSingle =
        SessionTxnRecord::parse(IDLParserContext("resultSingleDoc test"), resultSingleDoc);

    ASSERT_EQ(resultSingle.getTxnNum(), 5LL);
    ASSERT_EQ(resultSingle.getLastWriteOpTime(), repl::OpTime(Timestamp(Seconds(1), 0), 1));

    // The txnNum and optime of the write with the larger txnNum were saved.
    auto resultDiffTxnDoc =
        client.findOne(NamespaceString::kSessionTransactionsTableNamespace,
                       BSON(SessionTxnRecord::kSessionIdFieldName << lsidDiffTxn.toBSON()));
    ASSERT_TRUE(!resultDiffTxnDoc.isEmpty());

    auto resultDiffTxn =
        SessionTxnRecord::parse(IDLParserContext("resultDiffTxnDoc test"), resultDiffTxnDoc);

    ASSERT_EQ(resultDiffTxn.getTxnNum(), 20LL);
    ASSERT_EQ(resultDiffTxn.getLastWriteOpTime(), repl::OpTime(Timestamp(Seconds(3), 0), 1));

    // The txnNum and optime of the write with the later optime were saved.
    auto resultSameTxnDoc =
        client.findOne(NamespaceString::kSessionTransactionsTableNamespace,
                       BSON(SessionTxnRecord::kSessionIdFieldName << lsidSameTxn.toBSON()));
    ASSERT_TRUE(!resultSameTxnDoc.isEmpty());

    auto resultSameTxn =
        SessionTxnRecord::parse(IDLParserContext("resultSameTxnDoc test"), resultSameTxnDoc);

    ASSERT_EQ(resultSameTxn.getTxnNum(), 30LL);
    ASSERT_EQ(resultSameTxn.getLastWriteOpTime(), repl::OpTime(Timestamp(Seconds(6), 0), 1));

    // There is no entry for the write with no txnNumber.
    auto resultNoTxn =
        client.findOne(NamespaceString::kSessionTransactionsTableNamespace,
                       BSON(SessionTxnRecord::kSessionIdFieldName << lsidNoTxn.toBSON()));
    ASSERT_TRUE(resultNoTxn.isEmpty());
}

TEST_F(OplogApplierImplTxnTableTest, SessionMigrationNoOpEntriesShouldUpdateTxnTable) {
    const auto insertLsid = makeLogicalSessionIdForTest();
    OperationSessionInfo insertSessionInfo;
    insertSessionInfo.setSessionId(insertLsid);
    insertSessionInfo.setTxnNumber(3);
    auto date = Date_t::now();

    auto innerOplog = makeOplogEntry(nss(),
                                     {Timestamp(10, 10), 1},
                                     repl::OpTypeEnum::kInsert,
                                     BSON("_id" << 1),
                                     boost::none,
                                     insertSessionInfo,
                                     date);

    auto outerInsertDate = Date_t::now();
    auto insertOplog = makeOplogEntryForMigrate(nss(),
                                                {Timestamp(40, 0), 1},
                                                repl::OpTypeEnum::kNoop,
                                                BSON("$sessionMigrateInfo" << 1),
                                                innerOplog.getEntry().toBSON(),
                                                insertSessionInfo,
                                                outerInsertDate);

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {insertOplog}));

    checkTxnTable(insertSessionInfo, {Timestamp(40, 0), 1}, outerInsertDate);
}

TEST_F(OplogApplierImplTxnTableTest, PreImageNoOpEntriesShouldNotUpdateTxnTable) {
    const auto preImageLsid = makeLogicalSessionIdForTest();
    OperationSessionInfo preImageSessionInfo;
    preImageSessionInfo.setSessionId(preImageLsid);
    preImageSessionInfo.setTxnNumber(3);
    auto preImageDate = Date_t::now();

    auto preImageOplog = makeOplogEntryForMigrate(nss(),
                                                  {Timestamp(30, 0), 1},
                                                  repl::OpTypeEnum::kNoop,
                                                  BSON("_id" << 1),
                                                  boost::none,
                                                  preImageSessionInfo,
                                                  preImageDate);

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {preImageOplog}));

    ASSERT_FALSE(docExists(_opCtx.get(),
                           NamespaceString::kSessionTransactionsTableNamespace,
                           BSON(SessionTxnRecord::kSessionIdFieldName
                                << preImageSessionInfo.getSessionId()->toBSON())));
}

TEST_F(OplogApplierImplTxnTableTest, NonMigrateNoOpEntriesShouldNotUpdateTxnTable) {
    const auto lsid = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(3);

    auto oplog = makeOplogEntry(nss(),
                                {Timestamp(30, 0), 1},
                                repl::OpTypeEnum::kNoop,
                                BSON("_id" << 1),
                                boost::none,
                                sessionInfo,
                                Date_t::now());

    auto writerPool = makeReplWriterPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        writerPool.get());

    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {oplog}));

    ASSERT_FALSE(docExists(
        _opCtx.get(),
        NamespaceString::kSessionTransactionsTableNamespace,
        BSON(SessionTxnRecord::kSessionIdFieldName << sessionInfo.getSessionId()->toBSON())));
}

TEST_F(IdempotencyTest, EmptyCappedNamespaceNotFound) {
    // Create a BSON "emptycapped" command.
    auto emptyCappedCmd = BSON("emptycapped" << nss.coll());

    // Create an "emptycapped" oplog entry.
    auto emptyCappedOp = makeCommandOplogEntry(nextOpTime(), nss, emptyCappedCmd);

    // Ensure that NamespaceNotFound is acceptable.
    ASSERT_OK(runOpInitialSync(emptyCappedOp));

    AutoGetCollectionForReadCommand autoColl(_opCtx.get(), nss);
    ASSERT_FALSE(autoColl);
}

TEST_F(IdempotencyTest, UpdateTwoFields) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    ASSERT_OK(runOpInitialSync(insert(fromjson("{_id: 1, y: [0]}"))));
    auto updateOp1 = update(1,
                            update_oplog_entry::makeDeltaOplogEntry(
                                BSON(doc_diff::kUpdateSectionFieldName << fromjson("{x: 1}"))));
    auto updateOp2 =
        update(1,
               update_oplog_entry::makeDeltaOplogEntry(
                   BSON(doc_diff::kUpdateSectionFieldName << fromjson("{x: 2, 'y.0': 2}"))));
    auto updateOp3 = update(1,
                            update_oplog_entry::makeDeltaOplogEntry(
                                BSON(doc_diff::kUpdateSectionFieldName << fromjson("{y: 3}"))));

    auto ops = {updateOp1, updateOp2, updateOp3};
    testOpsAreIdempotent(ops);
}

typedef SetSteadyStateConstraints<IdempotencyTest, false>
    IdempotencyTestDisableSteadyStateConstraints;
typedef SetSteadyStateConstraints<IdempotencyTest, true>
    IdempotencyTestEnableSteadyStateConstraints;

TEST_F(IdempotencyTestDisableSteadyStateConstraints, AcceptableErrorsRecordedInSteadyStateMode) {
    // Create a BSON "emptycapped" command.
    auto emptyCappedCmd = BSON("emptycapped" << nss.coll());

    // Create a "emptycapped" oplog entry.
    auto emptyCappedOp = makeCommandOplogEntry(nextOpTime(), nss, emptyCappedCmd);

    // Ensure that NamespaceNotFound is "acceptable" but counted.
    int prevAcceptableError = replOpCounters.getAcceptableErrorInCommand()->load();
    ASSERT_OK(runOpSteadyState(emptyCappedOp));

    auto postAcceptableError = replOpCounters.getAcceptableErrorInCommand()->load();
    ASSERT_EQ(1, postAcceptableError - prevAcceptableError);

    ASSERT_EQ(postAcceptableError,
              replOpCounters.getObj()
                  .getObjectField("constraintsRelaxed")
                  .getField("acceptableErrorInCommand")
                  .Long());
}

TEST_F(IdempotencyTestEnableSteadyStateConstraints,
       AcceptableErrorsNotAcceptableInSteadyStateMode) {
    // Create a BSON "emptycapped" command.
    auto emptyCappedCmd = BSON("emptycapped" << nss.coll());

    // Create a "emptyCapped" oplog entry.
    auto emptyCappedOp = makeCommandOplogEntry(nextOpTime(), nss, emptyCappedCmd);

    // Ensure that NamespaceNotFound is returned.
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, runOpSteadyState(emptyCappedOp));
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
