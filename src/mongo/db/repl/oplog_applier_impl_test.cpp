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

#include "mongo/db/repl/oplog_applier_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/document_validation.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_pre_and_post_images_options_gen.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/repl/idempotency_test_fixture.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace repl {
namespace {

CollectionAcquisition acquireCollForRead(OperationContext* opCtx, const NamespaceString& nss) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
}

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
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, {});
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
}

TEST_F(OplogApplierImplTestDisableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteDocumentDatabaseMissing) {

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test.t");
    NamespaceString otherNss = NamespaceString::createNamespaceString_forTest("test.othername");
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, {});
    int prevDeleteFromMissing = replOpCounters.getDeleteFromMissingNamespace()->load();
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nss, false);
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
    NamespaceString otherNss = NamespaceString::createNamespaceString_forTest("test.othername");
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, {});
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
}

TEST_F(OplogApplierImplTest,
       applyOplogEntryOrGroupedInsertsInsertDocumentCollectionLookupByUUIDFails) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    createDatabase(_opCtx.get(), nss.db_forTest());
    NamespaceString otherNss =
        NamespaceString::createNamespaceString_forTest(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kInsert, otherNss, kUuid);
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
}

TEST_F(OplogApplierImplTestDisableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteDocumentCollectionLookupByUUIDFails) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    createDatabase(_opCtx.get(), nss.db_forTest());
    NamespaceString otherNss =
        NamespaceString::createNamespaceString_forTest(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, kUuid);
    int prevDeleteFromMissing = replOpCounters.getDeleteFromMissingNamespace()->load();
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nss, false);
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
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    createDatabase(_opCtx.get(), nss.db_forTest());
    NamespaceString otherNss =
        NamespaceString::createNamespaceString_forTest(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, kUuid);
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsInsertDocumentCollectionMissing) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    createDatabase(_opCtx.get(), nss.db_forTest());
    // Even though the collection doesn't exist, this is handled in the actual application function,
    // which in the case of this test just ignores such errors. This tests mostly that we don't
    // implicitly create the collection.
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, {});
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
    ASSERT_FALSE(collectionExists(_opCtx.get(), nss));
}

TEST_F(OplogApplierImplTestDisableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteDocumentCollectionMissing) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    createDatabase(_opCtx.get(), nss.db_forTest());
    // Even though the collection doesn't exist, this is handled in the actual application function,
    // which in the case of this test just ignores such errors. This tests mostly that we don't
    // implicitly create the collection.
    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, {});
    int prevDeleteFromMissing = replOpCounters.getDeleteFromMissingNamespace()->load();
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nss, false);
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
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    createDatabase(_opCtx.get(), nss.db_forTest());
    // With steady state constraints enabled, attempting to delete from a missing collection is an
    // error.
    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, {});
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsInsertDocumentCollectionExists) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    repl::createCollection(_opCtx.get(), nss, {});
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, {});
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nss, true);
}

TEST_F(OplogApplierImplTestDisableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteDocumentDocMissing) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    repl::createCollection(_opCtx.get(), nss, {});
    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, {});
    int prevDeleteWasEmpty = replOpCounters.getDeleteWasEmpty()->load();
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nss, false);
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
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    repl::createCollection(_opCtx.get(), nss, {});
    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, {});
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NoSuchKey>);
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsDeleteDocumentCollectionAndDocExist) {
    // Setup the pre-images collection.
    ChangeStreamPreImagesCollectionManager::get(_opCtx.get())
        .createPreImagesCollection(_opCtx.get());
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    createCollection(
        _opCtx.get(), nss, createRecordChangeStreamPreAndPostImagesCollectionOptions());
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {BSON("_id" << 0)}, 0));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, {});
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nss, true);
}

TEST_F(OplogApplierImplTestDisableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsInsertExistingDocument) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {BSON("_id" << 0)}, 0));
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, uuid);
    int prevInsertOnExistingDoc = replOpCounters.getInsertOnExistingDoc()->load();
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nss, false);
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
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {BSON("_id" << 0)}, 0));
    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, uuid);
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::DuplicateKey, op, nss, false);
}

TEST_F(OplogApplierImplTestDisableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsUpdateMissingDocument) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto op = makeOplogEntry(repl::OpTypeEnum::kUpdate,
                             nss,
                             uuid,
                             update_oplog_entry::makeDeltaOplogEntry(
                                 BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                             BSON("_id" << 0));
    int prevUpdateOnMissingDoc = replOpCounters.getUpdateOnMissingDoc()->load();
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nss, true);
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
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto op = makeOplogEntry(repl::OpTypeEnum::kUpdate,
                             nss,
                             uuid,
                             update_oplog_entry::makeDeltaOplogEntry(
                                 BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                             BSON("_id" << 0));
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(
        ErrorCodes::UpdateOperationFailed, op, nss, false);
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsInsertDocumentCollectionLockedByUUID) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    // Test that the collection to lock is determined by the UUID and not the 'ns' field.
    NamespaceString otherNss =
        NamespaceString::createNamespaceString_forTest(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kInsert, otherNss, uuid);
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nss, true);
}

TEST_F(OplogApplierImplTestDisableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsDeleteMissingDocCollectionLockedByUUID) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    CollectionOptions options;
    options.uuid = kUuid;
    createCollection(_opCtx.get(), nss, options);

    // Test that the collection to lock is determined by the UUID and not the 'ns' field.
    NamespaceString otherNss =
        NamespaceString::createNamespaceString_forTest(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, options.uuid);
    int prevDeleteWasEmpty = replOpCounters.getDeleteWasEmpty()->load();
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nss, false);
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
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    CollectionOptions options;
    options.uuid = kUuid;
    createCollection(_opCtx.get(), nss, options);

    NamespaceString otherNss =
        NamespaceString::createNamespaceString_forTest(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, options.uuid);
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NoSuchKey>);
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsDeleteDocumentCollectionLockedByUUID) {
    // Setup the pre-images collection.
    ChangeStreamPreImagesCollectionManager::get(_opCtx.get())
        .createPreImagesCollection(_opCtx.get());
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    CollectionOptions options = createRecordChangeStreamPreAndPostImagesCollectionOptions();
    options.uuid = kUuid;
    createCollection(_opCtx.get(), nss, options);

    // Make sure the document to be deleted exists.
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {BSON("_id" << 0)}, 0));

    // Test that the collection to lock is determined by the UUID and not the 'ns' field.
    NamespaceString otherNss =
        NamespaceString::createNamespaceString_forTest(nss.getSisterNS("othername"));
    auto op = makeOplogEntry(OpTypeEnum::kDelete, otherNss, options.uuid);
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nss, true);
}

TEST_F(OplogApplierImplTest, applyOplogEntryToRecordChangeStreamPreImages) {
    // Setup the pre-images collection.
    ChangeStreamPreImagesCollectionManager::get(_opCtx.get())
        .createPreImagesCollection(_opCtx.get());

    // Create the collection.
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
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
    generateTestCasesForOperations(OplogApplication::Mode::kUnstableRecovering, {}, true);
    generateTestCasesForOperations(OplogApplication::Mode::kStableRecovering, {}, true);
    generateTestCasesForOperations(OplogApplication::Mode::kInitialSync, {}, false);
    const auto kFromMigrate{true};
    generateTestCasesForOperations(OplogApplication::Mode::kSecondary, kFromMigrate, false);
    generateTestCasesForOperations(
        OplogApplication::Mode::kUnstableRecovering, kFromMigrate, false);
    generateTestCasesForOperations(OplogApplication::Mode::kStableRecovering, kFromMigrate, false);
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
                ScopeGuard guard{[&wuow] {
                    wuow.commit();
                }};
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
        ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
            _opCtx.get(), ApplierOperation{&op}, testCase.applicationMode));

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
            const auto preImageDocument = ChangeStreamPreImage::parse(preImageLoadResult.getValue(),
                                                                      IDLParserContext{"test"});
            ASSERT_BSONOBJ_EQ(preImageDocument.getPreImage(), document);
            ASSERT_EQUALS(preImageDocument.getOperationTime(), op.getWallClockTime()) << testDesc;

        } else {
            ASSERT_FALSE(preImageLoadResult.isOK()) << testDesc;
        }
        ++docId;
    }
}

TEST_F(OplogApplierImplTest, CreateCollectionCommand) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto op = BSON("op" << "c"
                        << "ns" << nss.getCommandNS().ns_forTest() << "wall" << Date_t() << "o"
                        << BSON("create" << nss.coll()) << "ts" << Timestamp(1, 1) << "ui"
                        << UUID::gen());
    bool applyCmdCalled = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&,
                                            const boost::optional<CreateCollCatalogIdentifier>&) {
        applyCmdCalled = true;
        ASSERT_TRUE(opCtx);
        ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_IX));
        ASSERT_EQUALS(nss, collNss);
    };
    auto entry = OplogEntry(op);
    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&entry}, OplogApplication::Mode::kInitialSync));
    ASSERT_TRUE(applyCmdCalled);
    ASSERT_TRUE(collectionExists(_opCtx.get(), nss));
}

TEST_F(OplogApplierImplTest, CreateCollectionCommandMultitenant) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", true);

    auto tid{TenantId(OID::gen())};
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(tid, "test.foo");

    auto op = BSON("create" << nss.coll());
    bool applyCmdCalled = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&,
                                            const boost::optional<CreateCollCatalogIdentifier>&) {
        applyCmdCalled = true;
        ASSERT_TRUE(opCtx);
        ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_IX));
        ASSERT_TRUE(collNss.tenantId());
        ASSERT_EQ(tid, collNss.tenantId().get());
        ASSERT_EQUALS(nss, collNss);
    };

    const auto entry = makeCreateCollectionOplogEntry(_opCtx.get(), nextOpTime(), nss);
    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&entry}, OplogApplication::Mode::kSecondary));
    ASSERT_TRUE(applyCmdCalled);
}

TEST_F(OplogApplierImplTest, CreateCollectionCommandMultitenantRequireTenantIDFalse) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", false);

    auto tid{TenantId(OID::gen())};
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(tid, "test.foo");

    auto op = BSON("op" << "c"
                        << "ns" << nss.getCommandNS().toStringWithTenantId_forTest() << "wall"
                        << Date_t() << "o" << BSON("create" << nss.coll()) << "ts"
                        << Timestamp(1, 1) << "ui" << UUID::gen());


    bool applyCmdCalled = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&,
                                            const boost::optional<CreateCollCatalogIdentifier>&) {
        applyCmdCalled = true;
        ASSERT_TRUE(opCtx);
        ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_IX));
        ASSERT_TRUE(collNss.tenantId());
        ASSERT_EQ(tid, collNss.tenantId().get());
        ASSERT_EQUALS(nss, collNss);
    };

    auto entry = OplogEntry(op);
    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&entry}, OplogApplication::Mode::kSecondary));
    ASSERT_TRUE(applyCmdCalled);
}

TEST_F(OplogApplierImplTest, CreateCollectionCommandMultitenantAlreadyExists) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", true);

    auto tid1{TenantId(OID::gen())};
    auto tid2{TenantId(OID::gen())};
    std::string commonNamespace("test.foo");
    NamespaceString nssTenant1 =
        NamespaceString::createNamespaceString_forTest(tid1, commonNamespace);
    NamespaceString nssTenant2 =
        NamespaceString::createNamespaceString_forTest(tid2, commonNamespace);
    ASSERT_NE(tid1, tid2);

    CollectionOptions options;
    options.uuid = kUuid;

    repl::createCollection(_opCtx.get(), nssTenant1, options);

    bool applyCmdCalled = false;

    // Target this callback to only work with nssTenant2
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&,
                                            const boost::optional<CreateCollCatalogIdentifier>&) {
        applyCmdCalled = true;
        ASSERT_TRUE(opCtx);
        ASSERT_TRUE(
            shard_role_details::getLocker(opCtx)->isDbLockedForMode(nssTenant2.dbName(), MODE_IX));
        ASSERT_TRUE(collNss.tenantId());
        ASSERT_EQ(tid2, collNss.tenantId().get());
        ASSERT_EQUALS(nssTenant2, collNss);
    };

    ASSERT_TRUE(collectionExists(_opCtx.get(), nssTenant1));
    ASSERT_FALSE(collectionExists(_opCtx.get(), nssTenant2));

    const auto entry1 =
        makeCreateCollectionOplogEntry(_opCtx.get(), nextOpTime(), nssTenant1, options);
    const auto entry2 =
        makeCreateCollectionOplogEntry(_opCtx.get(), nextOpTime(), nssTenant2, UUID::gen());

    // This fails silently so we won't see any indication of a collision, but we can also assert
    // that the opObserver event above won't be called in the event of a collision.
    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&entry1}, OplogApplication::Mode::kSecondary));
    ASSERT_FALSE(applyCmdCalled);

    ASSERT_TRUE(collectionExists(_opCtx.get(), nssTenant1));
    ASSERT_FALSE(collectionExists(_opCtx.get(), nssTenant2));

    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&entry2}, OplogApplication::Mode::kSecondary));
    ASSERT_TRUE(applyCmdCalled);

    ASSERT_TRUE(collectionExists(_opCtx.get(), nssTenant1));
    ASSERT_TRUE(collectionExists(_opCtx.get(), nssTenant2));
}

TEST_F(OplogApplierImplTest, RenameCollectionCommandMultitenant) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", true);

    auto tid{TenantId(OID::gen())};  // rename should not occur across tenants
    const NamespaceString sourceNss =
        NamespaceString::createNamespaceString_forTest(tid, "test.foo");
    const NamespaceString targetNss =
        NamespaceString::createNamespaceString_forTest(tid, "test.bar");

    auto oRename = BSON("renameCollection" << sourceNss.toString_forTest() << "to"
                                           << targetNss.toString_forTest() << "tid" << tid);

    repl::createCollection(_opCtx.get(), sourceNss, {});
    // createCollection uses an actual opTime, so we must generate an actually opTime in the future.
    auto opTime = [opCtx = _opCtx.get()] {
        WriteUnitOfWork wuow{opCtx};
        ScopeGuard guard{[&wuow] {
            wuow.commit();
        }};
        return repl::getNextOpTime(opCtx);
    }();
    auto op = makeCommandOplogEntry(opTime, sourceNss, oRename, {});

    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kSecondary));
    ASSERT_FALSE(collectionExists(_opCtx.get(), sourceNss));
    ASSERT_TRUE(collectionExists(_opCtx.get(), targetNss));
}

TEST_F(OplogApplierImplTest, RenameCollectionCommandMultitenantRequireTenantIDFalse) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", false);

    auto tid{TenantId(OID::gen())};  // rename should not occur across tenants
    const NamespaceString sourceNss =
        NamespaceString::createNamespaceString_forTest(tid, "test.foo");
    const NamespaceString targetNss =
        NamespaceString::createNamespaceString_forTest(tid, "test.bar");

    auto oRename = BSON("renameCollection" << sourceNss.toStringWithTenantId_forTest() << "to"
                                           << targetNss.toStringWithTenantId_forTest());

    repl::createCollection(_opCtx.get(), sourceNss, {});
    // createCollection uses an actual opTime, so we must generate an actually opTime in the future.
    auto opTime = [opCtx = _opCtx.get()] {
        WriteUnitOfWork wuow{opCtx};
        ScopeGuard guard{[&wuow] {
            wuow.commit();
        }};
        return repl::getNextOpTime(opCtx);
    }();
    auto op = makeCommandOplogEntry(opTime, sourceNss, oRename, {});

    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kSecondary));
    ASSERT_FALSE(collectionExists(_opCtx.get(), sourceNss));
    ASSERT_TRUE(collectionExists(_opCtx.get(), targetNss));
}

TEST_F(OplogApplierImplTest, RenameCollectionCommandMultitenantAcrossTenantsRequireTenantIDFalse) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", false);

    auto tid{TenantId(OID::gen())};
    auto wrongTid{TenantId(OID::gen())};  // rename should not occur across tenants
    const NamespaceString sourceNss =
        NamespaceString::createNamespaceString_forTest(tid, "test.foo");
    const NamespaceString targetNss =
        NamespaceString::createNamespaceString_forTest(tid, "test.bar");
    const NamespaceString wrongTargetNss =
        NamespaceString::createNamespaceString_forTest(wrongTid, targetNss.toString_forTest());

    ASSERT_NE(sourceNss, wrongTargetNss);

    auto oRename = BSON("renameCollection" << sourceNss.toStringWithTenantId_forTest() << "to"
                                           << wrongTargetNss.toStringWithTenantId_forTest());

    repl::createCollection(_opCtx.get(), sourceNss, {});
    // createCollection uses an actual opTime, so we must generate an actually opTime in the future.
    auto opTime = [opCtx = _opCtx.get()] {
        WriteUnitOfWork wuow{opCtx};
        ScopeGuard guard{[&wuow] {
            wuow.commit();
        }};
        return repl::getNextOpTime(opCtx);
    }();
    auto op = makeCommandOplogEntry(opTime, sourceNss, oRename, {});

    ASSERT_EQ(ErrorCodes::IllegalOperation,
              _applyOplogEntryOrGroupedInsertsWrapper(
                  _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kSecondary));
    ASSERT_TRUE(collectionExists(_opCtx.get(), sourceNss));
    ASSERT_FALSE(collectionExists(_opCtx.get(), targetNss));
    ASSERT_FALSE(collectionExists(_opCtx.get(), wrongTargetNss));
}

OplogEntry makeInvalidateOp(OpTime opTime,
                            const NamespaceString& nss,
                            BSONObj document,
                            OperationSessionInfo sessionInfo,
                            mongo::UUID uuid) {
    return DurableOplogEntry(opTime,                     // optime
                             OpTypeEnum::kUpdate,        // opType
                             nss,                        // namespace
                             uuid,                       // uuid
                             boost::none,                // fromMigrate
                             boost::none,                // checkExistenceForDiffInsert
                             boost::none,                // versionContext
                             OplogEntry::kOplogVersion,  // version
                             document,                   // o
                             boost::none,                // o2
                             sessionInfo,                // sessionInfo
                             boost::none,                // upsert
                             Date_t(),                   // wall clock time
                             {},                         // statement ids
                             boost::none,  // optime of previous write within same transaction
                             boost::none,  // pre-image optime
                             boost::none,  // post-image optime
                             boost::none,  // ShardId of resharding recipient
                             boost::none,  // _id
                             RetryImageEnum::kPreImage);  // needsRetryImage
}

TEST_F(OplogApplierImplTest, applyOplogEntryToInvalidateChangeStreamPreImages) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    CollectionOptions options;
    createCollection(_opCtx.get(), NamespaceString::kConfigImagesNamespace, options);

    auto document = BSON("_id" << 0);

    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);

    const BSONObj preImage(BSON("_id" << 2 << "post" << 1));
    OpTime opTime = OpTime();

    repl::ImageEntry imageEntry;
    imageEntry.set_id(sessionId);
    imageEntry.setTxnNumber(2);
    imageEntry.setTs(opTime.getTimestamp());
    imageEntry.setImageKind(repl::RetryImageEnum::kPreImage);
    imageEntry.setImage(preImage);
    imageEntry.setInvalidated(false);

    ASSERT_OK(getStorageInterface()->insertDocument(
        _opCtx.get(), NamespaceString::kConfigImagesNamespace, {imageEntry.toBSON()}, 0));

    {
        auto sideCollection = acquireCollection(
            _opCtx.get(),
            CollectionAcquisitionRequest(NamespaceString::kConfigImagesNamespace,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(_opCtx.get()),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        auto imageEntry =
            ImageEntry::parse(Helpers::findOneForTesting(
                                  _opCtx.get(), sideCollection, BSON("_id" << sessionId.toBSON())),
                              IDLParserContext("test"));
        ASSERT_FALSE(imageEntry.getInvalidated());
    }

    // Make an oplog entry to invalidate the pre image.
    OplogEntry invalidateOp = makeInvalidateOp(OpTime(), nss, document, sessionInfo, kUuid);

    // Apply the oplog entry.
    {
        repl::UnreplicatedWritesBlock uwb(_opCtx.get());
        DisableDocumentValidation validationDisabler(_opCtx.get());
        ASSERT_THROWS(applyOplogEntryOrGroupedInserts(_opCtx.get(),
                                                      ApplierOperation{&invalidateOp},
                                                      OplogApplication::Mode::kInitialSync,
                                                      /* isDataConsistent */ false),
                      ExceptionFor<ErrorCodes::NamespaceNotFound>);
    }

    auto sideCollection = acquireCollection(
        _opCtx.get(),
        CollectionAcquisitionRequest(NamespaceString::kConfigImagesNamespace,
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(_opCtx.get()),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
    imageEntry = ImageEntry::parse(
        Helpers::findOneForTesting(_opCtx.get(), sideCollection, BSON("_id" << sessionId.toBSON())),
        IDLParserContext("test"));
    ASSERT(imageEntry.getInvalidated());
}

TEST_F(OplogApplierImplTest, applyOplogEntryToInvalidateNonModPreImages) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    CollectionOptions options;
    createCollection(_opCtx.get(), NamespaceString::kConfigImagesNamespace, options);
    auto collUUID = createCollectionWithUuid(_opCtx.get(), nss);

    auto document = BSON("_id" << 0);

    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);

    const BSONObj preImage(BSON("_id" << 2 << "post" << 1));
    OpTime opTime = OpTime();

    repl::ImageEntry imageEntry;
    imageEntry.set_id(sessionId);
    imageEntry.setTxnNumber(2);
    imageEntry.setTs(opTime.getTimestamp());
    imageEntry.setImageKind(repl::RetryImageEnum::kPreImage);
    imageEntry.setImage(preImage);
    imageEntry.setInvalidated(false);

    ASSERT_OK(getStorageInterface()->insertDocument(
        _opCtx.get(), NamespaceString::kConfigImagesNamespace, {imageEntry.toBSON()}, 0));

    {
        auto sideCollection = acquireCollection(
            _opCtx.get(),
            CollectionAcquisitionRequest(NamespaceString::kConfigImagesNamespace,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(_opCtx.get()),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        auto imageEntry =
            ImageEntry::parse(Helpers::findOneForTesting(
                                  _opCtx.get(), sideCollection, BSON("_id" << sessionId.toBSON())),
                              IDLParserContext("test"));
        ASSERT_FALSE(imageEntry.getInvalidated());
    }
    OpTime updateOpTime{{2, 1}, 1};
    auto updateOp = makeOplogEntry(updateOpTime,
                                   repl::OpTypeEnum::kUpdate,
                                   nss,
                                   collUUID,
                                   update_oplog_entry::makeDeltaOplogEntry(BSON(
                                       doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                                   BSON("_id" << 0),  // o2
                                   boost::none,       // fromMigrate
                                   sessionInfo,
                                   RetryImageEnum::kPreImage);

    // Apply the oplog entry.
    {
        repl::UnreplicatedWritesBlock uwb(_opCtx.get());
        DisableDocumentValidation validationDisabler(_opCtx.get());
        ASSERT_NOT_OK(applyOplogEntryOrGroupedInserts(_opCtx.get(),
                                                      ApplierOperation{&updateOp},
                                                      OplogApplication::Mode::kInitialSync,
                                                      /* isDataConsistent */ false));
    }

    auto sideCollection = acquireCollection(
        _opCtx.get(),
        CollectionAcquisitionRequest(NamespaceString::kConfigImagesNamespace,
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(_opCtx.get()),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
    imageEntry = ImageEntry::parse(
        Helpers::findOneForTesting(_opCtx.get(), sideCollection, BSON("_id" << sessionId.toBSON())),
        IDLParserContext("test"));

    ASSERT(imageEntry.getInvalidated());
}


// Tests we correctly handle the case in SERVER-79033 where we attempt to invalidate the image
// collection entry for a given lsid but we have already written an invalidate entry with a later
// timestamp.
TEST_F(OplogApplierImplTest, ImageCollectionInvalidationInInitialSyncHandlesConflictingUpsert) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    CollectionOptions options;
    createCollection(_opCtx.get(), NamespaceString::kConfigImagesNamespace, options);

    auto document = BSON("_id" << 0);

    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);

    const BSONObj preImage(BSON("_id" << 2));
    OpTime opTime = OpTime();

    repl::ImageEntry imageEntry;
    imageEntry.set_id(sessionId);
    imageEntry.setTxnNumber(2);
    imageEntry.setTs(opTime.getTimestamp());
    imageEntry.setImageKind(repl::RetryImageEnum::kPreImage);
    imageEntry.setImage(preImage);
    imageEntry.setInvalidated(false);

    ASSERT_OK(getStorageInterface()->insertDocument(
        _opCtx.get(), NamespaceString::kConfigImagesNamespace, {imageEntry.toBSON()}, 0));

    {
        auto sideCollection = acquireCollection(
            _opCtx.get(),
            CollectionAcquisitionRequest(NamespaceString::kConfigImagesNamespace,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(_opCtx.get()),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        auto imageEntry =
            ImageEntry::parse(Helpers::findOneForTesting(
                                  _opCtx.get(), sideCollection, BSON("_id" << sessionId.toBSON())),
                              IDLParserContext("test"));
        ASSERT_FALSE(imageEntry.getInvalidated());
    }

    OpTime invalidateOpTime = OpTime(Timestamp(1686186824, 50), 1);
    OpTime earlierInvalidateOpTime = OpTime(Timestamp(1686186824, 47), 1);

    // Make an oplog entry to invalidate the pre image.
    OplogEntry invalidateOp = makeInvalidateOp(invalidateOpTime, nss, document, sessionInfo, kUuid);

    // Apply the first oplog entry which should lead us to write an invalidate entry.
    {
        repl::UnreplicatedWritesBlock uwb(_opCtx.get());
        DisableDocumentValidation validationDisabler(_opCtx.get());
        ASSERT_THROWS(applyOplogEntryOrGroupedInserts(_opCtx.get(),
                                                      ApplierOperation{&invalidateOp},
                                                      OplogApplication::Mode::kInitialSync,
                                                      /* isDataConsistent */ false),
                      ExceptionFor<ErrorCodes::NamespaceNotFound>);

        // Confirm that we wrote an invalidate entry.
        auto sideCollection = acquireCollection(
            _opCtx.get(),
            CollectionAcquisitionRequest(NamespaceString::kConfigImagesNamespace,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(_opCtx.get()),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        imageEntry =
            ImageEntry::parse(Helpers::findOneForTesting(
                                  _opCtx.get(), sideCollection, BSON("_id" << sessionId.toBSON())),
                              IDLParserContext("test"));
        ASSERT(imageEntry.getInvalidated());
        ASSERT_EQ(imageEntry.getTs(), invalidateOpTime.getTimestamp());
    }

    // Create and attempt to apply another entry that will also lead us to try to insert an
    // invalidate document, but with an earlier timestamp than the first one. If we don't switch to
    // upsert:false on retry, then we will hang indefinitely here due to repeated write conflicts,
    // as we will never observe the existing document due to its timestamp being later than ours.
    OplogEntry earlierInvalidateOp =
        makeInvalidateOp(earlierInvalidateOpTime, nss, document, sessionInfo, kUuid);

    {
        repl::UnreplicatedWritesBlock uwb(_opCtx.get());
        DisableDocumentValidation validationDisabler(_opCtx.get());
        ASSERT_THROWS(applyOplogEntryOrGroupedInserts(_opCtx.get(),
                                                      ApplierOperation{&earlierInvalidateOp},
                                                      OplogApplication::Mode::kInitialSync,
                                                      /* isDataConsistent */ false),
                      ExceptionFor<ErrorCodes::NamespaceNotFound>);
    }

    // The image collection entgry should be unchanged.
    auto sideCollection = acquireCollection(
        _opCtx.get(),
        CollectionAcquisitionRequest(NamespaceString::kConfigImagesNamespace,
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(_opCtx.get()),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
    imageEntry = ImageEntry::parse(
        Helpers::findOneForTesting(_opCtx.get(), sideCollection, BSON("_id" << sessionId.toBSON())),
        IDLParserContext("test"));
    ASSERT(imageEntry.getInvalidated());
}

TEST_F(IdempotencyTest, CollModCommandMultitenant) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", true);

    auto tid{TenantId(OID::gen())};
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(tid, "test.foo");

    setNss(nss);  // IdempotencyTest keeps nss state, update with tid

    bool applyCmdCalled = false;
    _opObserver->onCollModFn = [&](OperationContext* opCtx,
                                   const NamespaceString& targetNss,
                                   const UUID& uuid,
                                   const BSONObj& collModCmd,
                                   const CollectionOptions& oldCollOptions,
                                   boost::optional<IndexCollModInfo> indexInfo) {
        applyCmdCalled = true;

        ASSERT_TRUE(targetNss.tenantId());
        ASSERT_EQ(targetNss, nss);
    };

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_OK(
        runOpInitialSync(makeCreateCollectionOplogEntry(_opCtx.get(), nextOpTime(), nss, kUuid)));
    ASSERT_OK(runOpInitialSync(
        buildIndex(BSON("createdAt" << 1), BSON("expireAfterSeconds" << 3600), kUuid)));

    auto indexChange = fromjson("{keyPattern: {createdAt:1}, expireAfterSeconds:4000}");
    auto collModCmd = BSON("collMod" << nss.coll() << "index" << indexChange);
    auto op =
        makeCommandOplogEntry(nextOpTime(), nss, collModCmd, boost::none /* object2 */, kUuid);

    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kSecondary));
    ASSERT_TRUE(applyCmdCalled);
}

TEST_F(IdempotencyTest, CollModCommandMultitenantWrongTenant) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", true);

    auto tid1{TenantId(OID::gen())};
    auto tid2{TenantId(OID::gen())};
    std::string commonNamespace("test.foo");
    NamespaceString nssTenant1 =
        NamespaceString::createNamespaceString_forTest(tid1, commonNamespace);
    NamespaceString nssTenant2 =
        NamespaceString::createNamespaceString_forTest(tid2, commonNamespace);
    ASSERT_NE(tid1, tid2);

    setNss(nssTenant1);  // IdempotencyTest keeps nss state, update with tid of the created nss

    bool applyCmdCalled = false;
    _opObserver->onCollModFn = [&](OperationContext* opCtx,
                                   const NamespaceString& targetNss,
                                   const UUID& uuid,
                                   const BSONObj& collModCmd,
                                   const CollectionOptions& oldCollOptions,
                                   boost::optional<IndexCollModInfo> indexInfo) {
        applyCmdCalled = true;  // We should not be able to reach this
    };

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_OK(runOpInitialSync(
        makeCreateCollectionOplogEntry(_opCtx.get(), nextOpTime(), nssTenant1, kUuid)));
    ASSERT_OK(runOpInitialSync(
        buildIndex(BSON("createdAt" << 1), BSON("expireAfterSeconds" << 3600), kUuid)));

    auto indexChange = fromjson("{keyPattern: {createdAt:1}, expireAfterSeconds:4000}");
    auto collModCmd = BSON("collMod" << nssTenant2.coll() << "index" << indexChange);
    auto op = makeCommandOplogEntry(
        nextOpTime(), nssTenant2, collModCmd, boost::none /* object2 */, kUuid);

    // A NamespaceNotFound error is absorbed by the applier, but we can still determine the
    // op_observer callback was never called
    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kSecondary));
    ASSERT_FALSE(applyCmdCalled);
}

/**
 * Test only subclass of OplogApplierImpl that does not apply oplog entries, but tracks ops.
 */
class TrackOpsAppliedApplier : public OplogApplierImpl {
public:
    using OplogApplierImpl::OplogApplierImpl;

    Status applyOplogBatchPerWorker(OperationContext* opCtx,
                                    std::vector<ApplierOperation>* ops,
                                    WorkerMultikeyPathInfo* workerMultikeyPathInfo,
                                    bool isDataConsistent) override;

    std::vector<OplogEntry> getOperationsApplied() {
        stdx::lock_guard lk(_mutex);
        return _operationsApplied;
    }

private:
    std::vector<OplogEntry> _operationsApplied;
    // Synchronize reads and writes to 'operationsApplied'.
    stdx::mutex _mutex;
};

Status TrackOpsAppliedApplier::applyOplogBatchPerWorker(
    OperationContext* opCtx,
    std::vector<ApplierOperation>* ops,
    WorkerMultikeyPathInfo* workerMultikeyPathInfo,
    const bool isDataConsistent) {
    stdx::lock_guard lk(_mutex);
    for (auto&& opPtr : *ops) {
        _operationsApplied.push_back(*opPtr);
    }
    return Status::OK();
}

DEATH_TEST_F(OplogApplierImplTest, MultiApplyAbortsWhenNoOperationsAreGiven, "!ops.empty()") {
    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    TrackOpsAppliedApplier oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());
    oplogApplier.applyOplogBatch(_opCtx.get(), {}).getStatus().ignore();
}

bool _testOplogEntryIsForCappedCollection(OperationContext* opCtx,
                                          ReplicationCoordinator* const replCoord,
                                          ReplicationConsistencyMarkers* const consistencyMarkers,
                                          StorageInterface* const storageInterface,
                                          const NamespaceString& nss,
                                          const CollectionOptions& options) {
    auto workerPool = makeReplWorkerPool();
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
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());
    auto lastOpTime = unittest::assertGet(oplogApplier.applyOplogBatch(opCtx, {op}));
    ASSERT_EQUALS(op.getOpTime(), lastOpTime);

    const auto opsApplied = oplogApplier.getOperationsApplied();
    ASSERT_EQUALS(1U, opsApplied.size());
    const auto& opApplied = opsApplied.front();
    ASSERT_EQUALS(op.getEntry(), opApplied.getEntry());
    // "isForCappedCollection" is not parsed from raw oplog entry document.
    return opApplied.isForCappedCollection();
}

NamespaceString makeNamespace(StringData prefix, StringData suffix = "") {
    return NamespaceString::createNamespaceString_forTest(fmt::format(
        "{}.{}_{}{}", prefix, unittest::getSuiteName(), unittest::getTestName(), suffix));
}

TEST_F(
    OplogApplierImplTest,
    MultiApplyDoesNotSetOplogEntryIsForCappedCollectionWhenProcessingNonCappedCollectionInsertOperation) {
    NamespaceString nss = makeNamespace("local");
    ASSERT_FALSE(_testOplogEntryIsForCappedCollection(_opCtx.get(),
                                                      ReplicationCoordinator::get(_opCtx.get()),
                                                      getConsistencyMarkers(),
                                                      getStorageInterface(),
                                                      nss,
                                                      CollectionOptions()));
}

TEST_F(OplogApplierImplTest,
       MultiApplySetsOplogEntryIsForCappedCollectionWhenProcessingCappedCollectionInsertOperation) {
    NamespaceString nss = makeNamespace("local");
    ASSERT_TRUE(_testOplogEntryIsForCappedCollection(_opCtx.get(),
                                                     ReplicationCoordinator::get(_opCtx.get()),
                                                     getConsistencyMarkers(),
                                                     getStorageInterface(),
                                                     nss,
                                                     createOplogCollectionOptions()));
}

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncUsesApplyOplogEntryOrGroupedInsertsToApplyOperation) {
    NamespaceString nss = makeNamespace("local");
    auto op = makeCreateCollectionOplogEntry(_opCtx.get(), {Timestamp(Seconds(1), 0), 1LL}, nss);

    std::vector<ApplierOperation> ops = {ApplierOperation{&op}};
    WorkerMultikeyPathInfo pathInfo;

    TestApplyOplogGroupApplier oplogApplier(
        nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kSecondary, false));
    const bool dataIsConsistent = true;
    ASSERT_OK(
        oplogApplier.applyOplogBatchPerWorker(_opCtx.get(), &ops, &pathInfo, dataIsConsistent));
    // Collection should be created after applyOplogEntryOrGroupedInserts() processes operation.
    ASSERT_TRUE(acquireCollForRead(_opCtx.get(), nss).exists());
}

TEST_F(OplogApplierImplTest,
       TxnTableUpdatesDoNotGetCoalescedForRetryableWritesAcrossDifferentTxnNumbers) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    const NamespaceString& nss = NamespaceString::createNamespaceString_forTest("test", "foo");
    repl::OpTime firstInsertOpTime(Timestamp(1, 0), 1);
    auto firstRetryableOp = makeInsertDocumentOplogEntryWithSessionInfo(
        firstInsertOpTime, nss, BSON("_id" << 1), sessionInfo);

    repl::OpTime secondInsertOpTime(Timestamp(2, 0), 1);
    sessionInfo.setTxnNumber(4);
    auto secondRetryableOp = makeInsertDocumentOplogEntryWithSessionInfo(
        secondInsertOpTime, nss, BSON("_id" << 2), sessionInfo);

    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());

    std::vector<std::vector<ApplierOperation>> writerVectors(
        workerPool->getStats().options.maxThreads);
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

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsInsertDocumentIncludesTenantId) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", true);
    const TenantId tid(OID::gen());
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(tid, "test.t");
    BSONObj doc = BSON("_id" << 0);

    repl::createCollection(_opCtx.get(), nss, {});

    auto op = makeOplogEntry(OpTypeEnum::kInsert, nss, boost::none, doc, boost::none);

    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nss, true);
    CollectionReader collectionReader(_opCtx.get(), nss);
    ASSERT_BSONOBJ_EQ(doc, unittest::assertGet(collectionReader.next()));
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsInsertDocumentIncorrectTenantId) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", true);
    const auto commonNss("test.t"_sd);
    const TenantId tid1(OID::gen());
    const TenantId tid2(OID::gen());
    const NamespaceString nssTenant1 =
        NamespaceString::createNamespaceString_forTest(tid1, commonNss);
    const NamespaceString nssTenant2 =
        NamespaceString::createNamespaceString_forTest(tid2, commonNss);
    BSONObj doc = BSON("_id" << 0);

    repl::createCollection(_opCtx.get(), nssTenant1, {});

    auto op = makeOplogEntry(OpTypeEnum::kInsert, nssTenant2, boost::none, doc, boost::none);

    ASSERT_THROWS(
        _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nssTenant2, false),
        ExceptionFor<ErrorCodes::NamespaceNotFound>);

    CollectionReader collectionReaderTenant1(_opCtx.get(), nssTenant1);
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, collectionReaderTenant1.next().getStatus());

    ASSERT(!collectionExists(_opCtx.get(), nssTenant2));
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsDeleteDocumentIncludesTenantId) {
    const TenantId tid(OID::gen());
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", true);
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(tid, "test.t");
    BSONObj doc = BSON("_id" << 0);

    repl::createCollection(_opCtx.get(), nss, CollectionOptions{});
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {doc}, 0));

    auto op = makeOplogEntry(OpTypeEnum::kDelete, nss, boost::none);

    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nss, true);

    // Check that the doc actually got deleted.
    CollectionReader collectionReader(_opCtx.get(), nss);
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, collectionReader.next().getStatus());
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsDeleteDocumentIncorrectTenantId) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", true);
    const auto commonNss("test.t"_sd);
    const TenantId tid1(OID::gen());
    const TenantId tid2(OID::gen());
    const NamespaceString nssTenant1 =
        NamespaceString::createNamespaceString_forTest(tid1, commonNss);
    const NamespaceString nssTenant2 =
        NamespaceString::createNamespaceString_forTest(tid2, commonNss);
    BSONObj doc = BSON("_id" << 0);

    repl::createCollection(_opCtx.get(), nssTenant1, {});
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nssTenant1, {doc}, 0));

    auto op = makeOplogEntry(OpTypeEnum::kDelete, nssTenant2, boost::none);

    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nssTenant2, false);

    CollectionReader collectionReaderTenant1(_opCtx.get(), nssTenant1);
    ASSERT_BSONOBJ_EQ(doc, unittest::assertGet(collectionReaderTenant1.next()));

    ASSERT(!collectionExists(_opCtx.get(), nssTenant2));
}

// Steady state constraints are required for secondaries in order to avoid turning an insert into an
// upsert and masking duplicateKey errors.
TEST_F(OplogApplierImplTestEnableSteadyStateConstraints,
       applyOplogEntryOrGroupedInsertsUuidIncludesTenantId) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", true);
    const auto commonNss("test.t"_sd);
    const TenantId tid1(OID::gen());
    const TenantId tid2(OID::gen());
    const NamespaceString nssTenant1 =
        NamespaceString::createNamespaceString_forTest(tid1, commonNss);
    const NamespaceString nssTenant2 =
        NamespaceString::createNamespaceString_forTest(tid2, commonNss);
    BSONObj doc = BSON("_id" << 0);

    auto uuid1 = createCollectionWithUuid(_opCtx.get(), nssTenant1);
    auto uuid2 = createCollectionWithUuid(_opCtx.get(), nssTenant2);

    // Only insert on tenant1, such that we will cause a duplicate key error on one tenant but not
    // the other.
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nssTenant1, {doc}, 0));

    auto duplicateInsertOp = makeOplogEntry(OpTypeEnum::kInsert, nssTenant1, uuid1);
    auto successfulInsertOp = makeOplogEntry(OpTypeEnum::kInsert, nssTenant2, uuid2);

    _testApplyOplogEntryOrGroupedInsertsCrudOperation(
        ErrorCodes::DuplicateKey, duplicateInsertOp, nssTenant1, false);
    _testApplyOplogEntryOrGroupedInsertsCrudOperation(
        ErrorCodes::OK, successfulInsertOp, nssTenant2, true);
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsUpdateDocumentIncludesTenantId) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", true);
    const TenantId tid(OID::gen());
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(tid, "test.t");
    BSONObj doc = BSON("_id" << 0);

    createCollection(_opCtx.get(), nss, {});
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {doc}, 0));

    auto op = makeOplogEntry(repl::OpTypeEnum::kUpdate,
                             nss,
                             boost::none,
                             update_oplog_entry::makeDeltaOplogEntry(
                                 BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                             BSON("_id" << 0));

    _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nss, true);

    // Check that the doc exists in its new updated form.
    BSONObj updatedDoc = BSON("_id" << 0 << "a" << 1);
    CollectionReader collectionReader(_opCtx.get(), nss);
    ASSERT_BSONOBJ_EQ(updatedDoc, unittest::assertGet(collectionReader.next()));
}

TEST_F(OplogApplierImplTest, applyOplogEntryOrGroupedInsertsUpdateDocumentIncorrectTenantId) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", true);
    const auto commonNss("test.t"_sd);
    const TenantId tid1(OID::gen());
    const TenantId tid2(OID::gen());
    const NamespaceString nssTenant1 =
        NamespaceString::createNamespaceString_forTest(tid1, commonNss);
    const NamespaceString nssTenant2 =
        NamespaceString::createNamespaceString_forTest(tid2, commonNss);
    BSONObj doc = BSON("_id" << 0);

    createCollection(_opCtx.get(), nssTenant1, {});
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nssTenant1, {doc}, 0));

    auto op = makeOplogEntry(repl::OpTypeEnum::kUpdate,
                             nssTenant2,
                             boost::none,
                             update_oplog_entry::makeDeltaOplogEntry(
                                 BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                             BSON("_id" << 0));

    ASSERT_THROWS(
        _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::OK, op, nssTenant2, true),
        ExceptionFor<ErrorCodes::NamespaceNotFound>);

    CollectionReader collectionReaderTenant1(_opCtx.get(), nssTenant1);
    ASSERT_BSONOBJ_EQ(doc, unittest::assertGet(collectionReaderTenant1.next()));

    ASSERT(!collectionExists(_opCtx.get(), nssTenant2));
}

TEST_F(OplogApplierImplTest, ApplySessionDeleteBeforeEarlierRetryableUpdate) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    // The namespace names here are significant.  We do a sort by namespace when we apply these
    // operations, which ensures the config.image_collection writes will be done first.
    auto configImagesUUID =
        createCollectionWithUuid(_opCtx.get(), NamespaceString::kConfigImagesNamespace);
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto collUUID = createCollectionWithUuid(_opCtx.get(), nss);
    BSONObj doc = BSON("_id" << 0);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {doc}, 0));
    Timestamp oldTs{1, 1};
    BSONObj existingRetryImage = BSON("_id" << sessionId.toBSON() << "txnNum" << 1 << "ts" << oldTs
                                            << "invalidated" << true);
    ASSERT_OK(getStorageInterface()->insertDocument(
        _opCtx.get(), NamespaceString::kConfigImagesNamespace, {existingRetryImage}, 0));
    OpTime updateOpTime{{2, 1}, 1};
    OpTime deleteOpTime{{2, 2}, 1};

    auto updateOp = makeOplogEntry(updateOpTime,
                                   repl::OpTypeEnum::kUpdate,
                                   nss,
                                   collUUID,
                                   update_oplog_entry::makeDeltaOplogEntry(BSON(
                                       doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                                   BSON("_id" << 0) /* o2 */,
                                   boost::none, /* fromMigrate*/
                                   sessionInfo,
                                   RetryImageEnum::kPreImage);

    auto deleteOp = makeOplogEntry(deleteOpTime,
                                   repl::OpTypeEnum::kDelete,
                                   NamespaceString::kConfigImagesNamespace,
                                   configImagesUUID,
                                   BSON("_id" << sessionId.toBSON()));

    setServerParameter("replWriterThreadCount", 1);
    setServerParameter("replWriterMinThreadCount", 1);
    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {updateOp, deleteOp}));
    CollectionReader collectionReaderConfigImages(_opCtx.get(),
                                                  NamespaceString::kConfigImagesNamespace);
    // We should have deleted the config.image_collection entry.
    ASSERT_NOT_OK(collectionReaderConfigImages.next());

    // The doc update should have gone through.
    BSONObj updatedDoc = BSON("_id" << 0 << "a" << 1);
    CollectionReader collectionReader(_opCtx.get(), nss);
    ASSERT_BSONOBJ_EQ(updatedDoc, unittest::assertGet(collectionReader.next()));
}

TEST_F(OplogApplierImplTest, ApplySessionDeleteBeforeEarlierFirstRetryableUpdate) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    // The namespace names here are significant.  We do a sort by namespace when we apply these
    // operations, which ensures the config.image_collection writes will be done first.
    auto configImagesUUID =
        createCollectionWithUuid(_opCtx.get(), NamespaceString::kConfigImagesNamespace);
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto collUUID = createCollectionWithUuid(_opCtx.get(), nss);
    BSONObj doc = BSON("_id" << 0);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {doc}, 0));
    OpTime updateOpTime{{2, 1}, 1};
    OpTime deleteOpTime{{2, 2}, 1};

    auto updateOp = makeOplogEntry(updateOpTime,
                                   repl::OpTypeEnum::kUpdate,
                                   nss,
                                   collUUID,
                                   update_oplog_entry::makeDeltaOplogEntry(BSON(
                                       doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                                   BSON("_id" << 0) /* o2 */,
                                   boost::none, /* fromMigrate*/
                                   sessionInfo,
                                   RetryImageEnum::kPreImage);

    auto deleteOp = makeOplogEntry(deleteOpTime,
                                   repl::OpTypeEnum::kDelete,
                                   NamespaceString::kConfigImagesNamespace,
                                   configImagesUUID,
                                   BSON("_id" << sessionId.toBSON()));

    setServerParameter("replWriterThreadCount", 1);
    setServerParameter("replWriterMinThreadCount", 1);
    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {updateOp, deleteOp}));
    CollectionReader collectionReaderConfigImages(_opCtx.get(),
                                                  NamespaceString::kConfigImagesNamespace);
    // We should have deleted the config.image_collection entry.
    ASSERT_NOT_OK(collectionReaderConfigImages.next());

    // The doc update should have gone through.
    BSONObj updatedDoc = BSON("_id" << 0 << "a" << 1);
    CollectionReader collectionReader(_opCtx.get(), nss);
    ASSERT_BSONOBJ_EQ(updatedDoc, unittest::assertGet(collectionReader.next()));
}

TEST_F(OplogApplierImplTest, ApplySessionDeleteAfterLaterRetryableUpdate) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    // The namespace names here are significant.  We do a sort by namespace when we apply these
    // operations, which ensures the config.image_collection writes will be done later.
    auto configImagesUUID =
        createCollectionWithUuid(_opCtx.get(), NamespaceString::kConfigImagesNamespace);
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("atest.t");
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto collUUID = createCollectionWithUuid(_opCtx.get(), nss);
    BSONObj doc = BSON("_id" << 0);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {doc}, 0));
    Timestamp oldTs{1, 1};
    BSONObj existingRetryImage = BSON("_id" << sessionId.toBSON() << "txnNum" << 1 << "ts" << oldTs
                                            << "invalidated" << true);
    ASSERT_OK(getStorageInterface()->insertDocument(
        _opCtx.get(), NamespaceString::kConfigImagesNamespace, {existingRetryImage}, 0));
    OpTime updateOpTime{{2, 2}, 1};
    OpTime deleteOpTime{{2, 1}, 1};

    auto updateOp = makeOplogEntry(updateOpTime,
                                   repl::OpTypeEnum::kUpdate,
                                   nss,
                                   collUUID,
                                   update_oplog_entry::makeDeltaOplogEntry(BSON(
                                       doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                                   BSON("_id" << 0) /* o2 */,
                                   boost::none, /* fromMigrate*/
                                   sessionInfo,
                                   RetryImageEnum::kPreImage);

    auto deleteOp = makeOplogEntry(deleteOpTime,
                                   repl::OpTypeEnum::kDelete,
                                   NamespaceString::kConfigImagesNamespace,
                                   configImagesUUID,
                                   BSON("_id" << sessionId.toBSON()));

    setServerParameter("replWriterThreadCount", 1);
    setServerParameter("replWriterMinThreadCount", 1);
    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {deleteOp, updateOp}));
    CollectionReader collectionReaderConfigImages(_opCtx.get(),
                                                  NamespaceString::kConfigImagesNamespace);
    BSONObj expectedPreImage = BSON("_id" << sessionId.toBSON() << "txnNum" << 2 << "ts"
                                          << updateOpTime.getTimestamp() << "imageKind"
                                          << "preImage"
                                          << "image" << BSON("_id" << 0) << "invalidated" << false);
    // We should have only the pre-image in the config.image_collection entry.
    ASSERT_BSONOBJ_EQ(expectedPreImage, unittest::assertGet(collectionReaderConfigImages.next()));
    ASSERT_NOT_OK(collectionReaderConfigImages.next());

    // The doc update should have gone through.
    BSONObj updatedDoc = BSON("_id" << 0 << "a" << 1);
    CollectionReader collectionReader(_opCtx.get(), nss);
    ASSERT_BSONOBJ_EQ(updatedDoc, unittest::assertGet(collectionReader.next()));
}

TEST_F(OplogApplierImplTest, ApplySessionDeleteBeforeEarlierRetryableInternalTransactionUpdate) {
    const auto sessionId = makeLogicalSessionIdWithTxnNumberAndUUIDForTest(
        boost::none, /* let helper generate parentLsid */
        TxnNumber(10));

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    // The namespace names here are significant.  We do a sort by namespace when we apply these
    // operations, which ensures the config.image_collection writes will be done first.
    auto configImagesUUID =
        createCollectionWithUuid(_opCtx.get(), NamespaceString::kConfigImagesNamespace);
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto collUUID = createCollectionWithUuid(_opCtx.get(), nss);
    BSONObj doc = BSON("_id" << 0);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {doc}, 0));
    Timestamp oldTs{1, 1};
    BSONObj existingRetryImage = BSON("_id" << sessionId.toBSON() << "txnNum" << 1 << "ts" << oldTs
                                            << "invalidated" << true);
    ASSERT_OK(getStorageInterface()->insertDocument(
        _opCtx.get(), NamespaceString::kConfigImagesNamespace, {existingRetryImage}, 0));
    OpTime updateOpTime{{2, 1}, 1};
    OpTime deleteOpTime{{2, 2}, 1};

    auto applyOpsOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
        updateOpTime,
        NamespaceString::kAdminCommandNamespace,
        BSON("applyOps" << BSON_ARRAY(
                 BSON("op" << "u"
                           << "ns" << nss.ns_forTest() << "ui" << collUUID << "o"
                           << update_oplog_entry::makeDeltaOplogEntry(
                                  BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}")))
                           << "o2" << BSON("_id" << 0) << "needsRetryImage"
                           << "preImage"
                           << "stmtId" << 0))),
        sessionId,
        *sessionInfo.getTxnNumber(),
        {} /* no stmt id at outer level */,
        OpTime() /* prevOpTime */);

    auto deleteOp = makeOplogEntry(deleteOpTime,
                                   repl::OpTypeEnum::kDelete,
                                   NamespaceString::kConfigImagesNamespace,
                                   configImagesUUID,
                                   BSON("_id" << sessionId.toBSON()));

    setServerParameter("replWriterThreadCount", 1);
    setServerParameter("replWriterMinThreadCount", 1);
    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {applyOpsOp, deleteOp}));
    CollectionReader collectionReaderConfigImages(_opCtx.get(),
                                                  NamespaceString::kConfigImagesNamespace);
    // We should have deleted the config.image_collection entry.
    ASSERT_NOT_OK(collectionReaderConfigImages.next());

    // The doc update should have gone through.
    BSONObj updatedDoc = BSON("_id" << 0 << "a" << 1);
    CollectionReader collectionReader(_opCtx.get(), nss);
    ASSERT_BSONOBJ_EQ(updatedDoc, unittest::assertGet(collectionReader.next()));
}

TEST_F(OplogApplierImplTest, ApplySessionDeleteAfterLaterRetryableInternalTransactionUpdate) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    // The namespace names here are significant.  We do a sort by namespace when we apply these
    // operations, which ensures the config.image_collection writes will be done later.
    auto configImagesUUID =
        createCollectionWithUuid(_opCtx.get(), NamespaceString::kConfigImagesNamespace);
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("atest.t");
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto collUUID = createCollectionWithUuid(_opCtx.get(), nss);
    BSONObj doc = BSON("_id" << 0);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {doc}, 0));
    Timestamp oldTs{1, 1};
    BSONObj existingRetryImage = BSON("_id" << sessionId.toBSON() << "txnNum" << 1 << "ts" << oldTs
                                            << "invalidated" << true);
    ASSERT_OK(getStorageInterface()->insertDocument(
        _opCtx.get(), NamespaceString::kConfigImagesNamespace, {existingRetryImage}, 0));
    OpTime updateOpTime{{2, 2}, 1};
    OpTime deleteOpTime{{2, 1}, 1};

    auto applyOpsOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
        updateOpTime,
        NamespaceString::kAdminCommandNamespace,
        BSON("applyOps" << BSON_ARRAY(
                 BSON("op" << "u"
                           << "ns" << nss.ns_forTest() << "ui" << collUUID << "o"
                           << update_oplog_entry::makeDeltaOplogEntry(
                                  BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}")))
                           << "o2" << BSON("_id" << 0) << "needsRetryImage"
                           << "preImage"
                           << "stmtId" << 0))),
        sessionId,
        *sessionInfo.getTxnNumber(),
        {} /* no stmt id at outer level */,
        OpTime() /* prevOpTime */);

    auto deleteOp = makeOplogEntry(deleteOpTime,
                                   repl::OpTypeEnum::kDelete,
                                   NamespaceString::kConfigImagesNamespace,
                                   configImagesUUID,
                                   BSON("_id" << sessionId.toBSON()));

    setServerParameter("replWriterThreadCount", 1);
    setServerParameter("replWriterMinThreadCount", 1);
    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {deleteOp, applyOpsOp}));
    CollectionReader collectionReaderConfigImages(_opCtx.get(),
                                                  NamespaceString::kConfigImagesNamespace);
    BSONObj expectedPreImage = BSON("_id" << sessionId.toBSON() << "txnNum" << 2 << "ts"
                                          << updateOpTime.getTimestamp() << "imageKind"
                                          << "preImage"
                                          << "image" << BSON("_id" << 0) << "invalidated" << false);
    // We should have only the pre-image in the config.image_collection entry.
    ASSERT_BSONOBJ_EQ(expectedPreImage, unittest::assertGet(collectionReaderConfigImages.next()));
    ASSERT_NOT_OK(collectionReaderConfigImages.next());

    // The doc update should have gone through.
    BSONObj updatedDoc = BSON("_id" << 0 << "a" << 1);
    CollectionReader collectionReader(_opCtx.get(), nss);
    ASSERT_BSONOBJ_EQ(updatedDoc, unittest::assertGet(collectionReader.next()));
}

TEST_F(OplogApplierImplTest, ApplyApplyOpsSessionDeleteBeforeEarlierRetryableUpdate) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    // The namespace names here are significant.  We do a sort by namespace when we apply these
    // operations, which ensures the config.image_collection writes will be done first.
    auto configImagesUUID =
        createCollectionWithUuid(_opCtx.get(), NamespaceString::kConfigImagesNamespace);
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto collUUID = createCollectionWithUuid(_opCtx.get(), nss);
    BSONObj doc = BSON("_id" << 0);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {doc}, 0));
    Timestamp oldTs{1, 1};
    BSONObj existingRetryImage = BSON("_id" << sessionId.toBSON() << "txnNum" << 1 << "ts" << oldTs
                                            << "invalidated" << true);
    ASSERT_OK(getStorageInterface()->insertDocument(
        _opCtx.get(), NamespaceString::kConfigImagesNamespace, {existingRetryImage}, 0));
    OpTime updateOpTime{{2, 1}, 1};
    OpTime deleteOpTime{{2, 2}, 1};

    auto updateOp = makeOplogEntry(updateOpTime,
                                   repl::OpTypeEnum::kUpdate,
                                   nss,
                                   collUUID,
                                   update_oplog_entry::makeDeltaOplogEntry(BSON(
                                       doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                                   BSON("_id" << 0) /* o2 */,
                                   boost::none, /* fromMigrate*/
                                   sessionInfo,
                                   RetryImageEnum::kPreImage);

    auto applyOpsOp = makeCommandOplogEntry(
        deleteOpTime,
        NamespaceString::kAdminCommandNamespace,
        BSON("applyOps" << BSON_ARRAY(
                 BSON("op" << "d"
                           << "ns" << redactTenant(NamespaceString::kConfigImagesNamespace) << "ui"
                           << configImagesUUID << "o" << BSON("_id" << sessionId.toBSON())))));

    setServerParameter("replWriterThreadCount", 1);
    setServerParameter("replWriterMinThreadCount", 1);
    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {updateOp, applyOpsOp}));
    CollectionReader collectionReaderConfigImages(_opCtx.get(),
                                                  NamespaceString::kConfigImagesNamespace);
    // We should have deleted the config.image_collection entry.
    ASSERT_NOT_OK(collectionReaderConfigImages.next());

    // The doc update should have gone through.
    BSONObj updatedDoc = BSON("_id" << 0 << "a" << 1);
    CollectionReader collectionReader(_opCtx.get(), nss);
    ASSERT_BSONOBJ_EQ(updatedDoc, unittest::assertGet(collectionReader.next()));
}

TEST_F(OplogApplierImplTest, ApplyApplyOpsSessionDeleteAfterLaterRetryableUpdate) {
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    // The namespace names here are significant.  We do a sort by namespace when we apply these
    // operations, which ensures the config.image_collection writes will be done later.
    auto configImagesUUID =
        createCollectionWithUuid(_opCtx.get(), NamespaceString::kConfigImagesNamespace);
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("atest.t");
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto collUUID = createCollectionWithUuid(_opCtx.get(), nss);
    BSONObj doc = BSON("_id" << 0);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {doc}, 0));
    Timestamp oldTs{1, 1};
    BSONObj existingRetryImage = BSON("_id" << sessionId.toBSON() << "txnNum" << 1 << "ts" << oldTs
                                            << "invalidated" << true);
    ASSERT_OK(getStorageInterface()->insertDocument(
        _opCtx.get(), NamespaceString::kConfigImagesNamespace, {existingRetryImage}, 0));
    OpTime updateOpTime{{2, 2}, 1};
    OpTime deleteOpTime{{2, 1}, 1};

    auto updateOp = makeOplogEntry(updateOpTime,
                                   repl::OpTypeEnum::kUpdate,
                                   nss,
                                   collUUID,
                                   update_oplog_entry::makeDeltaOplogEntry(BSON(
                                       doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                                   BSON("_id" << 0) /* o2 */,
                                   boost::none, /* fromMigrate*/
                                   sessionInfo,
                                   RetryImageEnum::kPreImage);

    auto applyOpsOp = makeCommandOplogEntry(
        deleteOpTime,
        NamespaceString::kAdminCommandNamespace,
        BSON("applyOps" << BSON_ARRAY(
                 BSON("op" << "d"
                           << "ns" << redactTenant(NamespaceString::kConfigImagesNamespace) << "ui"
                           << configImagesUUID << "o" << BSON("_id" << sessionId.toBSON())))));

    setServerParameter("replWriterThreadCount", 1);
    setServerParameter("replWriterMinThreadCount", 1);
    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {applyOpsOp, updateOp}));
    CollectionReader collectionReaderConfigImages(_opCtx.get(),
                                                  NamespaceString::kConfigImagesNamespace);
    BSONObj expectedPreImage = BSON("_id" << sessionId.toBSON() << "txnNum" << 2 << "ts"
                                          << updateOpTime.getTimestamp() << "imageKind"
                                          << "preImage"
                                          << "image" << BSON("_id" << 0) << "invalidated" << false);
    // We should have only the pre-image in the config.image_collection entry.
    ASSERT_BSONOBJ_EQ(expectedPreImage, unittest::assertGet(collectionReaderConfigImages.next()));
    ASSERT_NOT_OK(collectionReaderConfigImages.next());

    // The doc update should have gone through.
    BSONObj updatedDoc = BSON("_id" << 0 << "a" << 1);
    CollectionReader collectionReader(_opCtx.get(), nss);
    ASSERT_BSONOBJ_EQ(updatedDoc, unittest::assertGet(collectionReader.next()));
}

// TODO (SERVER-109556): Adjustments to suites coming from this ticket might result in a better
// candidate for this test's fixture.
TEST_F(OplogApplierImplTest, ApplyApplyOpsContainerOperations) {
    // TODO (SERVER-103670): Remove.
    RAIIServerParameterControllerForTest ff("featureFlagPrimaryDrivenIndexBuilds", true);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");

    auto storageEngine = serviceContext->getStorageEngine();
    auto ident = storageEngine->generateNewInternalIdent();
    auto ru = storageEngine->newRecoveryUnit();
    auto trs = storageEngine->getEngine()->makeTemporaryRecordStore(*ru, ident, KeyFormat::String);

    auto k = BSONBinData("K", 1, BinDataGeneral);
    auto v = BSONBinData("V", 1, BinDataGeneral);

    BSONArray innerOps = BSON_ARRAY(BSON("op" << "ci"
                                              << "ns" << nss.ns_forTest() << "container" << ident
                                              << "o" << BSON("k" << k << "v" << v))
                                    << BSON("op" << "cd"
                                                 << "ns" << nss.ns_forTest() << "container" << ident
                                                 << "o" << BSON("k" << k)));

    BSONObj applyOpsCmd = BSON("applyOps" << innerOps);
    auto entry = makeCommandOplogEntry(nextOpTime(), nss, applyOpsCmd, boost::none, boost::none);

    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&entry}, OplogApplication::Mode::kSecondary));
}

// TODO (SERVER-109556): Adjustments to suites coming from this ticket might result in a better
// candidate for this test's fixture.
TEST_F(OplogApplierImplTest, ApplyContainerOperations) {
    // TODO (SERVER-103670): Remove.
    RAIIServerParameterControllerForTest ff("featureFlagPrimaryDrivenIndexBuilds", true);

    auto nss = NamespaceString::createNamespaceString_forTest("test.t");

    auto storageEngine = serviceContext->getStorageEngine();
    auto ident = storageEngine->generateNewInternalIdent();
    auto ru = storageEngine->newRecoveryUnit();
    auto trs = storageEngine->getEngine()->makeTemporaryRecordStore(*ru, ident, KeyFormat::String);

    auto k = BSONBinData("K", 1, BinDataGeneral);
    auto v = BSONBinData("V", 1, BinDataGeneral);
    auto insertEntry = makeContainerInsertOplogEntry(nextOpTime(), nss, ident, k, v);
    auto deleteEntry = makeContainerDeleteOplogEntry(nextOpTime(), nss, ident, k);

    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&insertEntry}, OplogApplication::Mode::kSecondary));

    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&deleteEntry}, OplogApplication::Mode::kSecondary));
}

class MultiOplogEntryOplogApplierImplTest : public OplogApplierImplTest {
public:
    MultiOplogEntryOplogApplierImplTest()
        : _nss1(NamespaceString::createNamespaceString_forTest("test.preptxn1")),
          _nss2(NamespaceString::createNamespaceString_forTest("test.preptxn2")),
          _txnNum(1) {}

protected:
    void setUp() override {
        OplogApplierImplTest::setUp();
        const NamespaceString cmdNss =
            NamespaceString::createNamespaceString_forTest("admin", "$cmd");

        _uuid1 = createCollectionWithUuid(_opCtx.get(), _nss1);
        _uuid2 = createCollectionWithUuid(_opCtx.get(), _nss2);
        createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);

        _lsid = makeLogicalSessionId(_opCtx.get());

        _insertOp1 = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(1), 1), 1LL},
            cmdNss,
            BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                    << "ns" << _nss1.ns_forTest() << "ui" << *_uuid1
                                                    << "o" << BSON("_id" << 1)))
                            << "partialTxn" << true),
            _lsid,
            _txnNum,
            {StmtId(0)},
            OpTime());
        _insertOp2 = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(1), 2), 1LL},
            cmdNss,
            BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                    << "ns" << _nss2.ns_forTest() << "ui" << *_uuid2
                                                    << "o" << BSON("_id" << 2)))
                            << "partialTxn" << true),
            _lsid,
            _txnNum,
            {StmtId(1)},
            _insertOp1->getOpTime());
        _commitOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(1), 3), 1LL},
            cmdNss,
            BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                    << "ns" << _nss2.ns_forTest() << "ui" << *_uuid2
                                                    << "o" << BSON("_id" << 3)))),
            _lsid,
            _txnNum,
            {StmtId(2)},
            _insertOp2->getOpTime());
        _opObserver->onInsertsFn =
            [&](OperationContext*, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
                stdx::lock_guard<stdx::mutex> lock(_insertMutex);
                if (nss.isOplog()) {
                    _insertedOplogDocs.insert(_insertedOplogDocs.end(), docs.begin(), docs.end());
                } else if (nss == _nss1 || nss == _nss2 ||
                           nss == NamespaceString::kSessionTransactionsTableNamespace) {
                    // Storing the inserted documents in a sorted data structure to make checking
                    // for valid results easier. The inserts will be performed by different threads
                    // and there's no guarantee of the order.
                    _insertedDocs[nss].insert(docs.begin(), docs.end());
                } else
                    FAIL("Unexpected insert")
                        << " into " << nss.toStringForErrorMsg() << " first doc: " << docs.front();
            };

        _workerPool = makeReplWorkerPool();
    }

    void tearDown() override {
        OplogApplierImplTest::tearDown();
    }

    void checkTxnTable(const LogicalSessionId& lsid,
                       const TxnNumber& txnNum,
                       const repl::OpTime& expectedOpTime,
                       Date_t expectedWallClock,
                       boost::optional<repl::OpTime> expectedStartOpTime,
                       boost::optional<DurableTxnStateEnum> expectedState) {
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
    std::unique_ptr<ThreadPool> _workerPool;

private:
    stdx::mutex _insertMutex;
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
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        _workerPool.get());

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
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kStableRecovering),
        _workerPool.get());

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

    const NamespaceString cmdNss = NamespaceString::createNamespaceString_forTest("admin", "$cmd");
    for (int i = 0; i < 4; i++) {
        insertDocs.push_back(BSON("_id" << i));
        insertOps.push_back(makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(1), i + 1), 1LL},
            cmdNss,
            BSON("applyOps"
                 << BSON_ARRAY(BSON(
                        "op" << "i"
                             << "ns" << (i == 1 ? _nss2.ns_forTest() : _nss1.ns_forTest()) << "ui"
                             << (i == 1 ? *_uuid2 : *_uuid1) << "o" << insertDocs.back()))
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
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        _workerPool.get());

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

OplogEntry addMultiOpType(OplogEntry op, MultiOplogEntryType multiOpType) {
    return unittest::assertGet(OplogEntry::parse(op.getEntry().toBSON().addField(
        BSON(OplogEntry::kMultiOpTypeFieldName << multiOpType).firstElement())));
}

TEST_F(MultiOplogEntryOplogApplierImplTest, MultiApplyTwoTransactionsOneBatch) {
    // Tests that two transactions on the same session ID in the same batch both
    // apply correctly.
    TxnNumber txnNum1(1);
    TxnNumber txnNum2(2);


    std::vector<OplogEntry> insertOps1, insertOps2;
    const NamespaceString cmdNss = NamespaceString::createNamespaceString_forTest("admin", "$cmd");
    insertOps1.push_back(makeCommandOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(1), 1), 1LL},
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                << "ns" << _nss1.ns_forTest() << "ui" << *_uuid1
                                                << "o" << BSON("_id" << 1)))
                        << "partialTxn" << true),
        _lsid,
        txnNum1,
        {StmtId(0)},
        OpTime()));
    insertOps1.push_back(makeCommandOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(1), 2), 1LL},
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                << "ns" << _nss1.ns_forTest() << "ui" << *_uuid1
                                                << "o" << BSON("_id" << 2)))
                        << "partialTxn" << true),

        _lsid,
        txnNum1,
        {StmtId(1)},
        insertOps1.back().getOpTime()));
    insertOps2.push_back(makeCommandOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(2), 1), 1LL},
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                << "ns" << _nss1.ns_forTest() << "ui" << *_uuid1
                                                << "o" << BSON("_id" << 3)))
                        << "partialTxn" << true),
        _lsid,
        txnNum2,
        {StmtId(0)},
        OpTime()));
    insertOps2.push_back(makeCommandOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(2), 2), 1LL},
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                << "ns" << _nss1.ns_forTest() << "ui" << *_uuid1
                                                << "o" << BSON("_id" << 4)))
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
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        _workerPool.get());

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

TEST_F(MultiOplogEntryOplogApplierImplTest, MultiApplyNontransactionalRetryableWriteLastInsert) {
    // Tests a retryable write with two oplog entries, where the first is an applyOps and the
    // second is an actual insert.  These should be applied nontransactionally.  The applyOps
    // writes one document to nss1 and one to nss2, the insert writes one document to nss1.
    std::vector<OplogEntry> insertOps;
    std::vector<BSONObj> insertDocs;

    const NamespaceString cmdNss = NamespaceString::createNamespaceString_forTest("admin", "$cmd");
    insertDocs.push_back(BSON("_id" << 0));
    insertDocs.push_back(BSON("_id" << 1));
    insertDocs.push_back(BSON("_id" << 2));
    auto applyOpsBson =
        BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                << "ns" << _nss1.ns_forTest() << "ui" << *_uuid1
                                                << "o" << insertDocs[0])
                                      << BSON("op" << "i"
                                                   << "ns" << _nss2.ns_forTest() << "ui" << *_uuid2
                                                   << "o" << insertDocs[1])));
    auto applyOpsOp = addMultiOpType(
        makeCommandOplogEntryWithSessionInfoAndStmtIds({Timestamp(Seconds(1), 1), 1LL},
                                                       cmdNss,
                                                       applyOpsBson,
                                                       _lsid,
                                                       _txnNum,
                                                       {StmtId(0), StmtId(1)},
                                                       OpTime()),
        MultiOplogEntryType::kApplyOpsAppliedSeparately);
    auto singleInsertOp =
        makeInsertDocumentOplogEntryWithSessionInfoAndStmtIds({Timestamp(Seconds(1), 2), 1LL},
                                                              _nss1,
                                                              *_uuid1,
                                                              insertDocs[2],
                                                              _lsid,
                                                              _txnNum,
                                                              {StmtId(2)},
                                                              applyOpsOp.getOpTime());
    insertOps.push_back(singleInsertOp);

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        _workerPool.get());

    // Insert the applyops entry in its own batch.  This should result in the oplog entry being
    // written and the operations being applied.
    const auto expectedStartOpTime = boost::none;  // Retryable writes don't have a startOpTime
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {applyOpsOp}));
    ASSERT_EQ(1U, oplogDocs().size());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(1U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  applyOpsOp.getOpTime(),
                  applyOpsOp.getWallClockTime(),
                  expectedStartOpTime,
                  boost::none);

    // Insert the insert entry in its own batch.  This should result in the insert entry being
    // written and the operations being applied.
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {singleInsertOp}));
    ASSERT_EQ(2U, oplogDocs().size());
    ASSERT_EQ(2U, _insertedDocs[_nss1].size());
    ASSERT_EQ(1U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  singleInsertOp.getOpTime(),
                  singleInsertOp.getWallClockTime(),
                  expectedStartOpTime,
                  boost::none);

    auto nss1It = _insertedDocs[_nss1].begin();
    auto nss2It = _insertedDocs[_nss2].begin();
    ASSERT_BSONOBJ_EQ(insertDocs[0], *(nss1It++));
    ASSERT_BSONOBJ_EQ(insertDocs[1], *(nss2It++));
    ASSERT_BSONOBJ_EQ(insertDocs[2], *(nss1It++));
}

TEST_F(MultiOplogEntryOplogApplierImplTest, MultiApplyNontransactionalRetryableWriteThreeEntries) {
    // Tests a retryable write with three oplog entries. These should be applied nontransactionally.
    // Populate oplog entries with 6 inserts, three in nss1 and three in nss2.
    std::vector<OplogEntry> insertOps;
    std::vector<BSONObj> insertDocs;

    const NamespaceString cmdNss = NamespaceString::createNamespaceString_forTest("admin", "$cmd");
    for (int i = 0; i < 6; i += 2) {
        insertDocs.push_back(BSON("_id" << i));
        insertDocs.push_back(BSON("_id" << (i + 1)));
        auto applyOpsBson =
            BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                    << "ns" << _nss1.ns_forTest() << "ui" << *_uuid1
                                                    << "o" << insertDocs[i])
                                          << BSON("op" << "i"
                                                       << "ns" << _nss2.ns_forTest() << "ui"
                                                       << *_uuid2 << "o" << insertDocs[i + 1])));
        insertOps.push_back(addMultiOpType(makeCommandOplogEntryWithSessionInfoAndStmtIds(
                                               {Timestamp(Seconds(1), i + 1), 1LL},
                                               cmdNss,
                                               applyOpsBson,
                                               _lsid,
                                               _txnNum,
                                               {StmtId(i), StmtId(i + 1)},
                                               i == 0 ? OpTime() : insertOps.back().getOpTime()),
                                           MultiOplogEntryType::kApplyOpsAppliedSeparately));
    }

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        _workerPool.get());

    // Insert the first entry in its own batch.  This should result in the oplog entry being written
    // and the operations being applied.
    const auto expectedStartOpTime = boost::none;  // Retryable writes don't have a startOpTime
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {insertOps[0]}));
    ASSERT_EQ(1U, oplogDocs().size());
    ASSERT_EQ(1U, _insertedDocs[_nss1].size());
    ASSERT_EQ(1U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  insertOps[0].getOpTime(),
                  insertOps[0].getWallClockTime(),
                  expectedStartOpTime,
                  boost::none);

    // Insert the second entry in its own batch.  This should result in the oplog entry being
    // written and the operations being applied.
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {insertOps[1]}));
    ASSERT_EQ(2U, oplogDocs().size());
    ASSERT_EQ(2U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  insertOps[1].getOpTime(),
                  insertOps[1].getWallClockTime(),
                  expectedStartOpTime,
                  boost::none);

    // Insert the last entry. This should result in the oplog entry being
    // written and the operations being applied.
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {insertOps[2]}));
    ASSERT_EQ(3U, oplogDocs().size());
    ASSERT_EQ(3U, _insertedDocs[_nss1].size());
    ASSERT_EQ(3U, _insertedDocs[_nss2].size());
    checkTxnTable(_lsid,
                  _txnNum,
                  insertOps[2].getOpTime(),
                  insertOps[2].getWallClockTime(),
                  expectedStartOpTime,
                  boost::none);

    // Check that we inserted the expected documents
    auto nss1It = _insertedDocs[_nss1].begin();
    auto nss2It = _insertedDocs[_nss2].begin();
    ASSERT_BSONOBJ_EQ(insertDocs[0], *(nss1It++));
    ASSERT_BSONOBJ_EQ(insertDocs[1], *(nss2It++));
    ASSERT_BSONOBJ_EQ(insertDocs[2], *(nss1It++));
    ASSERT_BSONOBJ_EQ(insertDocs[3], *(nss2It++));
    ASSERT_BSONOBJ_EQ(insertDocs[4], *(nss1It++));
    ASSERT_BSONOBJ_EQ(insertDocs[5], *(nss2It++));
}

class MultiOplogEntryOplogApplierImplTestMultitenant : public OplogApplierImplTest {
public:
    MultiOplogEntryOplogApplierImplTestMultitenant()
        : _tenantId(OID::gen()),
          _nss(NamespaceString::createNamespaceString_forTest(_tenantId, "test.preptxn")),
          _cmdNss(NamespaceString::createNamespaceString_forTest(_tenantId, "admin", "$cmd")),
          _txnNum(1) {}

protected:
    void setUp() override {
        OplogApplierImplTest::setUp();

        createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);

        _uuid = UUID::gen();
        _lsid = makeLogicalSessionId(_opCtx.get());

        _opObserver->onCreateCollectionFn =
            [&](OperationContext* opCtx,
                const NamespaceString& collNss,
                const CollectionOptions&,
                const BSONObj&,
                const boost::optional<CreateCollCatalogIdentifier>&) {
                stdx::lock_guard<stdx::mutex> lock(_mutex);
                if (collNss.isOplog()) {
                    _oplogDocs.insert(_oplogDocs.end(), BSON("create" << collNss.coll()));
                } else if (collNss == _nss ||
                           collNss == NamespaceString::kSessionTransactionsTableNamespace) {
                    // Storing the documents in a sorted data structure to make checking for valid
                    // results easier. The inserts will be performed by different threads and
                    // there's no guarantee of the order.
                    (_docs[collNss]).push_back(BSON("create" << collNss.coll()));
                } else
                    FAIL("Unexpected create") << " on " << collNss.toStringForErrorMsg();
            };

        _opObserver->onInsertsFn = [&](OperationContext*,
                                       const NamespaceString& nss,
                                       const std::vector<BSONObj>& docs) {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            if (nss.isOplog()) {
                _oplogDocs.insert(_oplogDocs.end(), docs.begin(), docs.end());
            } else if (nss == _nss || nss == NamespaceString::kSessionTransactionsTableNamespace) {
                // Storing the inserted documents in a sorted data structure to make checking
                // for valid results easier. The inserts will be performed by different threads
                // and there's no guarantee of the order.
                (_docs[nss]).insert(_docs[nss].end(), docs.begin(), docs.end());
            } else
                FAIL("Unexpected insert")
                    << " into " << nss.toStringForErrorMsg() << " first doc: " << docs.front();
        };

        _workerPool = makeReplWorkerPool();
    }

    void tearDown() override {
        OplogApplierImplTest::tearDown();
    }

    std::vector<BSONObj>& oplogDocs() {
        return _oplogDocs;
    }

protected:
    TenantId _tenantId;
    NamespaceString _nss;
    NamespaceString _cmdNss;
    boost::optional<UUID> _uuid;
    LogicalSessionId _lsid;
    TxnNumber _txnNum;
    boost::optional<OplogEntry> _commitOp;
    std::map<NamespaceString, std::vector<BSONObj>> _docs;
    std::vector<BSONObj> _oplogDocs;
    std::unique_ptr<ThreadPool> _workerPool;

private:
    stdx::mutex _mutex;
};

TEST_F(MultiOplogEntryOplogApplierImplTestMultitenant,
       MultiApplyUnpreparedTransactionTwoBatchesFeatureFlagOn) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", true);
    // Tests an unprepared transaction with ops both in the batch with the commit and prior
    // batches. Populate transaction with 2 linked entries - a create collection and an insert.
    std::vector<OplogEntry> ops;
    ops.push_back(makeCommandOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(1), 1), 1LL},
        _cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op" << "c"
                                                << "tid" << _tenantId << "ns" << _nss.ns_forTest()
                                                << "ui" << *_uuid << "o"
                                                << BSON("create" << _nss.coll())))
                        << "partialTxn" << true),
        _lsid,
        _txnNum,
        {StmtId(0)},
        OpTime()));

    ops.push_back(makeCommandOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(1), 2), 1LL},
        _cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                << "tid" << _tenantId << "ns" << _nss.ns_forTest()
                                                << "ui" << *_uuid << "o" << BSON("_id" << 1)))
                        << "partialTxn" << true),
        _lsid,
        _txnNum,
        {StmtId(1)},
        ops.back().getOpTime()));

    auto commitOp = makeCommandOplogEntryWithSessionInfoAndStmtIds({Timestamp(Seconds(1), 3), 1LL},
                                                                   _cmdNss,
                                                                   BSON("applyOps" << BSONArray()),
                                                                   _lsid,
                                                                   _txnNum,
                                                                   {StmtId(2)},
                                                                   ops.back().getOpTime());

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        _workerPool.get());

    // Insert the first entry in its own batch.  This should result in the oplog entry being written
    // but the entry should not be applied as it is part of a pending transaction.
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {ops[0]}));
    ASSERT_EQ(1U, oplogDocs().size());
    ASSERT_EQ(0U, _docs[_nss].size());

    // Insert the rest of the entries, including the commit.  These entries should be added to the
    // oplog, and all the entries including the first should be applied.
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {ops[1], commitOp}));
    ASSERT_EQ(3U, oplogDocs().size());
    ASSERT_EQ(2U, _docs[_nss].size());

    // Check that we applied the expected documents
    auto nssIt = _docs[_nss].begin();
    ASSERT_BSONOBJ_EQ(BSON("create" << _nss.coll()), *nssIt);
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), *(++nssIt));
}

TEST_F(MultiOplogEntryOplogApplierImplTestMultitenant,
       MultiApplyUnpreparedTransactionTwoBatchesFeatureFlagOff) {
    setServerParameter("multitenancySupport", true);
    setServerParameter("featureFlagRequireTenantID", false);
    // Tests an unprepared transaction with ops both in the batch with the commit and prior
    // batches. Populate transaction with 2 linked entries - a create collection and an insert.
    std::vector<OplogEntry> ops;
    ops.push_back(makeCommandOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(1), 1), 1LL},
        _cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op" << "c"
                                                << "ns" << _nss.toStringWithTenantId_forTest()
                                                << "ui" << *_uuid << "o"
                                                << BSON("create" << _nss.coll())))
                        << "partialTxn" << true),
        _lsid,
        _txnNum,
        {StmtId(0)},
        OpTime()));

    ops.push_back(makeCommandOplogEntryWithSessionInfoAndStmtIds(
        {Timestamp(Seconds(1), 2), 1LL},
        _cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                << "ns" << _nss.toStringWithTenantId_forTest()
                                                << "ui" << *_uuid << "o" << BSON("_id" << 1)))
                        << "partialTxn" << true),
        _lsid,
        _txnNum,
        {StmtId(1)},
        ops.back().getOpTime()));

    auto commitOp = makeCommandOplogEntryWithSessionInfoAndStmtIds({Timestamp(Seconds(1), 3), 1LL},
                                                                   _cmdNss,
                                                                   BSON("applyOps" << BSONArray()),
                                                                   _lsid,
                                                                   _txnNum,
                                                                   {StmtId(2)},
                                                                   ops.back().getOpTime());

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        _workerPool.get());

    // Insert the first entry in its own batch.  This should result in the oplog entry being written
    // but the entry should not be applied as it is part of a pending transaction.
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {ops[0]}));
    ASSERT_EQ(1U, oplogDocs().size());
    ASSERT_EQ(0U, _docs[_nss].size());

    // Insert the rest of the entries, including the commit.  These entries should be added to the
    // oplog, and all the entries including the first should be applied.
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {ops[1], commitOp}));
    ASSERT_EQ(3U, oplogDocs().size());
    ASSERT_EQ(2U, _docs[_nss].size());

    // Check that we applied the expected documents
    auto nssIt = _docs[_nss].begin();
    ASSERT_BSONOBJ_EQ(BSON("create" << _nss.coll()), *nssIt);
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), *(++nssIt));
}

class MultiOplogEntryPreparedTransactionTest : public MultiOplogEntryOplogApplierImplTest {
protected:
    void setUp() override {
        MultiOplogEntryOplogApplierImplTest::setUp();

        _prepareWithPrevOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(1), 3), 1LL},
            _nss1,
            BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                    << "ns" << _nss2.ns_forTest() << "ui" << *_uuid2
                                                    << "o" << BSON("_id" << 3)))
                            << "prepare" << true),
            _lsid,
            _txnNum,
            {StmtId(2)},
            _insertOp2->getOpTime());
        _singlePrepareApplyOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(1), 3), 1LL},
            _nss1,
            BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                    << "ns" << _nss1.ns_forTest() << "ui" << *_uuid1
                                                    << "o" << BSON("_id" << 0)))
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
            _singlePrepareApplyOp->getOpTime());
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
    stdx::mutex _insertMutex;
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
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        _workerPool.get());

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
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        _workerPool.get());

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
        _workerPool.get());
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
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kStableRecovering),
        _workerPool.get());

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
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        _workerPool.get());
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
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        _workerPool.get());

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
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        _workerPool.get());

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
        _workerPool.get());

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
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kStableRecovering),
        _workerPool.get());

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

class MultiPreparedTransactionsInOneBatchTest : public MultiOplogEntryOplogApplierImplTest {
protected:
    void setUp() override {
        MultiOplogEntryOplogApplierImplTest::setUp();

        const NamespaceString cmdNss = NamespaceString::createNamespaceString_forTest("admin.$cmd");
        _lsid1 = makeLogicalSessionId(_opCtx.get());
        _lsid2 = makeLogicalSessionId(_opCtx.get());
        _txnNum1 = _txnNum2 = 1;

        _prepareWithInsertsOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(2), 1), 1LL},
            cmdNss,
            BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                    << "ns" << _nss1.ns_forTest() << "ui" << *_uuid1
                                                    << "o" << BSON("_id" << 1))
                                          << BSON("op" << "i"
                                                       << "ns" << _nss1.ns_forTest() << "ui"
                                                       << *_uuid1 << "o" << BSON("_id" << 2)))
                            << "prepare" << true),
            _lsid1,
            _txnNum1,
            {StmtId(0)},
            OpTime());
        _nonTxnInsertOp1 =
            makeInsertDocumentOplogEntry({Timestamp(Seconds(2), 2), 1LL}, _nss1, BSON("_id" << 3));
        _nonTxnInsertOp2 =
            makeInsertDocumentOplogEntry({Timestamp(Seconds(2), 3), 1LL}, _nss1, BSON("_id" << 4));
        _prepareWithDeletesOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(2), 4), 1LL},
            cmdNss,
            BSON("applyOps" << BSON_ARRAY(BSON("op" << "d"
                                                    << "ns" << _nss1.ns_forTest() << "ui" << *_uuid1
                                                    << "o" << BSON("_id" << 3))
                                          << BSON("op" << "d"
                                                       << "ns" << _nss1.ns_forTest() << "ui"
                                                       << *_uuid1 << "o" << BSON("_id" << 4)))
                            << "prepare" << true),
            _lsid2,
            _txnNum2,
            {StmtId(0)},
            OpTime());
        _commitPrepareWithInsertsOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            {Timestamp(Seconds(3), 1), 1LL},
            cmdNss,
            BSON("commitTransaction" << 1 << "commitTimestamp" << Timestamp(Seconds(3), 1)),
            _lsid1,
            _txnNum1,
            {StmtId(1)},
            _prepareWithInsertsOp->getOpTime());
        _abortPrepareWithDeletesOp =
            makeCommandOplogEntryWithSessionInfoAndStmtIds({Timestamp(Seconds(3), 2), 1LL},
                                                           cmdNss,
                                                           BSON("abortTransaction" << 1),
                                                           _lsid2,
                                                           _txnNum2,
                                                           {StmtId(1)},
                                                           _prepareWithDeletesOp->getOpTime());

        _workerPool = makeReplWorkerPool(64);

        _opObserver->onDeleteFn = [&](OperationContext*,
                                      const CollectionPtr& coll,
                                      StmtId,
                                      const BSONObj&,
                                      const OplogDeleteEntryArgs& args) {
            stdx::lock_guard<stdx::mutex> lock(_deleteMutex);
            auto nss = coll->ns();
            if (nss == _nss1 || nss == _nss2 ||
                nss == NamespaceString::kSessionTransactionsTableNamespace) {
                // Storing the deleted documents in a sorted data structure to make checking
                // for valid results easier. The delete will be performed by different threads
                // and there's no guarantee of the order.
                _deletedDocs[nss]++;
            } else
                FAIL("Unexpected delete") << " from " << nss.toStringForErrorMsg();
        };
    }

protected:
    LogicalSessionId _lsid1, _lsid2;
    TxnNumber _txnNum1, _txnNum2;
    boost::optional<OplogEntry> _nonTxnInsertOp1, _nonTxnInsertOp2;
    boost::optional<OplogEntry> _prepareWithInsertsOp, _prepareWithDeletesOp;
    boost::optional<OplogEntry> _commitPrepareWithInsertsOp, _abortPrepareWithDeletesOp;
    std::map<NamespaceString, int> _deletedDocs;

private:
    stdx::mutex _insertMutex;
    stdx::mutex _deleteMutex;
};

TEST_F(MultiPreparedTransactionsInOneBatchTest, CommitAndAbortMultiPreparedTransactionsInOneBatch) {
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        _workerPool.get());

    // Apply a batch with multiple prepares and regular CRUD ops.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _prepareWithDeletesOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(
        _opCtx.get(),
        {*_prepareWithInsertsOp, *_nonTxnInsertOp1, *_nonTxnInsertOp2, *_prepareWithDeletesOp}));
    ASSERT_EQ(4U, oplogDocs().size());
    ASSERT_EQ(4U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2, _deletedDocs[_nss1]);
    checkTxnTable(_lsid1,
                  _txnNum1,
                  _prepareWithInsertsOp->getOpTime(),
                  _prepareWithInsertsOp->getWallClockTime(),
                  _prepareWithInsertsOp->getOpTime(),
                  DurableTxnStateEnum::kPrepared);
    checkTxnTable(_lsid2,
                  _txnNum2,
                  _prepareWithDeletesOp->getOpTime(),
                  _prepareWithDeletesOp->getWallClockTime(),
                  _prepareWithDeletesOp->getOpTime(),
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the commit for the first prepare.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _commitPrepareWithInsertsOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_commitPrepareWithInsertsOp}));

    ASSERT_BSONOBJ_EQ(_commitPrepareWithInsertsOp->getEntry().toBSON(), oplogDocs().back());
    ASSERT_EQ(5U, oplogDocs().size());
    ASSERT_EQ(4U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2, _deletedDocs[_nss1]);
    checkTxnTable(_lsid1,
                  _txnNum1,
                  _commitPrepareWithInsertsOp->getOpTime(),
                  _commitPrepareWithInsertsOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
    checkTxnTable(_lsid2,
                  _txnNum2,
                  _prepareWithDeletesOp->getOpTime(),
                  _prepareWithDeletesOp->getWallClockTime(),
                  _prepareWithDeletesOp->getOpTime(),
                  DurableTxnStateEnum::kPrepared);

    // Apply a batch with only the abort for the second prepare.
    getStorageInterface()->oplogDiskLocRegister(
        _opCtx.get(), _abortPrepareWithDeletesOp->getTimestamp(), true);
    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {*_abortPrepareWithDeletesOp}));

    ASSERT_BSONOBJ_EQ(_abortPrepareWithDeletesOp->getEntry().toBSON(), oplogDocs().back());
    ASSERT_EQ(6U, oplogDocs().size());
    ASSERT_EQ(4U, _insertedDocs[_nss1].size());
    ASSERT_EQ(2, _deletedDocs[_nss1]);
    checkTxnTable(_lsid1,
                  _txnNum1,
                  _commitPrepareWithInsertsOp->getOpTime(),
                  _commitPrepareWithInsertsOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kCommitted);
    checkTxnTable(_lsid2,
                  _txnNum2,
                  _abortPrepareWithDeletesOp->getOpTime(),
                  _abortPrepareWithDeletesOp->getWallClockTime(),
                  boost::none,
                  DurableTxnStateEnum::kAborted);
}

void testWorkerMultikeyPaths(OperationContext* opCtx,
                             const OplogEntry& op,
                             unsigned long numPaths) {
    TestApplyOplogGroupApplier oplogApplier(
        nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kSecondary, false));
    WorkerMultikeyPathInfo pathInfo;
    std::vector<ApplierOperation> ops = {ApplierOperation{&op}};
    const bool dataIsConsistent = true;
    ASSERT_OK(oplogApplier.applyOplogBatchPerWorker(opCtx, &ops, &pathInfo, dataIsConsistent));
    ASSERT_EQ(pathInfo.size(), numPaths);
}

TEST_F(OplogApplierImplTest, OplogApplicationThreadFuncAddsWorkerMultikeyPathInfoOnInsert) {
    // Set the state as secondary as we are going to apply createIndexes oplog entry.
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_SECONDARY));

    NamespaceString nss = makeNamespace("test");

    {
        auto op = makeCreateCollectionOplogEntry(
            _opCtx.get(), {Timestamp(Seconds(1), 0), 1LL}, nss, kUuid);
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

    NamespaceString nss = makeNamespace("test");

    {
        auto op = makeCreateCollectionOplogEntry(
            _opCtx.get(), {Timestamp(Seconds(1), 0), 1LL}, nss, kUuid);
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
            nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kSecondary, false));
        WorkerMultikeyPathInfo pathInfo;
        std::vector<ApplierOperation> ops = {ApplierOperation{&opA}, ApplierOperation{&opB}};
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
    NamespaceString nss = makeNamespace("test");

    {
        auto op = makeCreateCollectionOplogEntry(
            _opCtx.get(), {Timestamp(Seconds(1), 0), 1LL}, nss, kUuid);
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
    NamespaceString nss = makeNamespace("foo");
    auto op = makeCreateCollectionOplogEntry(
        _opCtx.get(), {Timestamp(Seconds(1), 0), 1LL}, nss, CollectionOptions{});

    TestApplyOplogGroupApplier oplogApplier(
        nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kSecondary, false));
    std::vector<ApplierOperation> ops = {ApplierOperation{&op}};
    const bool dataIsConsistent = true;
    ASSERT_EQUALS(
        ErrorCodes::InvalidOptions,
        oplogApplier.applyOplogBatchPerWorker(_opCtx.get(), &ops, nullptr, dataIsConsistent));
}

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncDisablesDocumentValidationWhileApplyingOperations) {
    NamespaceString nss = makeNamespace("local");
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn =
        [&](OperationContext* opCtx, const NamespaceString&, const std::vector<BSONObj>&) {
            onInsertsCalled = true;
            ASSERT_FALSE(opCtx->writesAreReplicated());
            ASSERT_TRUE(DocumentValidationSettings::get(opCtx).isSchemaValidationDisabled());
        };
    createCollectionWithUuid(_opCtx.get(), nss);
    auto op = makeInsertDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, BSON("_id" << 0));
    ASSERT_OK(runOpSteadyState(op));
    ASSERT(onInsertsCalled);
}

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncSetsVersionContextDecorationWhileApplyingOperations) {
    NamespaceString nss = makeNamespace("local");
    VersionContext vCtx{serverGlobalParams.featureCompatibility.acquireFCVSnapshot()};
    auto op =
        BSON("op" << "c"
                  << "ns" << nss.getCommandNS().ns_forTest() << "wall" << Date_t() << "o"
                  << BSON("create" << nss.coll()) << "ts" << Timestamp(1, 1) << "ui" << UUID::gen()
                  << OplogEntryBase::kVersionContextFieldName << vCtx.toBSON());

    bool applyCmdCalled = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&,
                                            const boost::optional<CreateCollCatalogIdentifier>&) {
        applyCmdCalled = true;
        ASSERT_EQUALS(vCtx, VersionContext::getDecoration(opCtx));
    };
    auto entry = OplogEntry(op);
    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&entry}, OplogApplication::Mode::kSecondary));
    ASSERT_TRUE(applyCmdCalled);

    // The decoration must be unset after applying, so that it can not have an effect on other ops.
    ASSERT_EQUALS(VersionContext{}, VersionContext::getDecoration(_opCtx.get()));
}

TEST_F(
    OplogApplierImplTest,
    OplogApplicationThreadFuncPassesThroughApplyOplogEntryOrGroupedInsertsErrorAfterFailingToApplyOperation) {
    NamespaceString nss = makeNamespace("local");
    // Delete operation without _id in 'o' field.
    auto op = makeDeleteDocumentOplogEntry({Timestamp(Seconds(1), 0), 1LL}, nss, {});
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, runOpSteadyState(op));
}

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncPassesThroughApplyOplogEntryOrGroupedInsertsException) {
    NamespaceString nss = makeNamespace("local");
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
    NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("test.t1");
    NamespaceString nss2 = NamespaceString::createNamespaceString_forTest("test.t2");
    NamespaceString nss3 = NamespaceString::createNamespaceString_forTest("test.t3");

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
    NamespaceString nss1 = makeNamespace("test", "_1");
    NamespaceString nss2 = makeNamespace("test", "_2");
    auto createOp1 =
        makeCreateCollectionOplogEntry(_opCtx.get(), {Timestamp(Seconds(seconds++), 0), 1LL}, nss1);
    auto createOp2 =
        makeCreateCollectionOplogEntry(_opCtx.get(), {Timestamp(Seconds(seconds++), 0), 1LL}, nss2);
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
    NamespaceString nss = makeNamespace("test", "_1");
    auto createOp =
        makeCreateCollectionOplogEntry(_opCtx.get(), {Timestamp(Seconds(seconds++), 0), 1LL}, nss);

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
    NamespaceString nss = makeNamespace("test");
    auto createOp =
        makeCreateCollectionOplogEntry(_opCtx.get(), {Timestamp(Seconds(seconds++), 0), 1LL}, nss);

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
    NamespaceString nss = makeNamespace("test");
    auto createOp =
        makeCreateCollectionOplogEntry(_opCtx.get(), {Timestamp(Seconds(seconds++), 0), 1LL}, nss);

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

    auto testNs = "test." + unittest::getSuiteName() + "_" + unittest::getTestName();

    // Create a sequence of 3 'insert' ops that can't be grouped because they are from different
    // namespaces.
    std::vector<OplogEntry> operationsToApply = {
        makeOp(NamespaceString::createNamespaceString_forTest(testNs + "_1")),
        makeOp(NamespaceString::createNamespaceString_forTest(testNs + "_2")),
        makeOp(NamespaceString::createNamespaceString_forTest(testNs + "_3"))};

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
    NamespaceString nss = makeNamespace("test", "_1");
    auto createOp =
        makeCreateCollectionOplogEntry(_opCtx.get(), {Timestamp(Seconds(seconds++), 0), 1LL}, nss);

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
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
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
    std::vector<ApplierOperation> ops = {ApplierOperation{&op}};
    WorkerMultikeyPathInfo pathInfo;
    const bool dataIsConsistent = true;
    ASSERT_OK(
        oplogApplier.applyOplogBatchPerWorker(_opCtx.get(), &ops, &pathInfo, dataIsConsistent));

    // Since the document was missing when we cloned data from the sync source, the collection
    // referenced by the failed operation should not be automatically created.
    ASSERT_FALSE(acquireCollForRead(_opCtx.get(), nss).exists());
}

TEST_F(OplogApplierImplTest,
       OplogApplicationThreadFuncSkipsDocumentOnNamespaceNotFoundDuringInitialSync) {
    BSONObj emptyDoc;
    TestApplyOplogGroupApplier oplogApplier(
        nullptr, nullptr, OplogApplier::Options(OplogApplication::Mode::kInitialSync));
    NamespaceString nss = makeNamespace("local");
    NamespaceString badNss = makeNamespace("local", "bad");
    auto doc1 = BSON("_id" << 1);
    auto doc2 = BSON("_id" << 2);
    auto doc3 = BSON("_id" << 3);
    auto op0 = makeCreateCollectionOplogEntry(_opCtx.get(), {Timestamp(Seconds(1), 0), 1LL}, nss);
    auto op1 = makeInsertDocumentOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss, doc1);
    auto op2 = makeInsertDocumentOplogEntry({Timestamp(Seconds(3), 0), 1LL}, badNss, doc2);
    auto op3 = makeInsertDocumentOplogEntry({Timestamp(Seconds(4), 0), 1LL}, nss, doc3);
    std::vector<ApplierOperation> ops = {ApplierOperation{&op0},
                                         ApplierOperation{&op1},
                                         ApplierOperation{&op2},
                                         ApplierOperation{&op3}};
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
    NamespaceString nss = makeNamespace("test");
    NamespaceString badNss = makeNamespace("test", "bad");
    auto doc1 = BSON("_id" << 1);
    auto keyPattern = BSON("a" << 1);
    auto doc3 = BSON("_id" << 3);
    auto op0 =
        makeCreateCollectionOplogEntry(_opCtx.get(), {Timestamp(Seconds(1), 0), 1LL}, nss, kUuid);
    auto op1 = makeInsertDocumentOplogEntry({Timestamp(Seconds(2), 0), 1LL}, nss, doc1);
    auto op2 = makeCreateIndexOplogEntry(
        {Timestamp(Seconds(3), 0), 1LL}, badNss, "a_1", keyPattern, kUuid);
    auto op3 = makeInsertDocumentOplogEntry({Timestamp(Seconds(4), 0), 1LL}, nss, doc3);
    std::vector<ApplierOperation> ops = {ApplierOperation{&op0},
                                         ApplierOperation{&op1},
                                         ApplierOperation{&op2},
                                         ApplierOperation{&op3}};
    WorkerMultikeyPathInfo pathInfo;
    const bool dataIsConsistent = true;
    ASSERT_OK(
        oplogApplier.applyOplogBatchPerWorker(_opCtx.get(), &ops, &pathInfo, dataIsConsistent));

    CollectionReader collectionReader(_opCtx.get(), nss);
    ASSERT_BSONOBJ_EQ(doc1, unittest::assertGet(collectionReader.next()));
    ASSERT_BSONOBJ_EQ(doc3, unittest::assertGet(collectionReader.next()));
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, collectionReader.next().getStatus());

    // 'badNss' collection should not be implicitly created while attempting to create an index.
    ASSERT_FALSE(acquireCollForRead(_opCtx.get(), badNss).exists());
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

    auto uuid = kUuid;
    auto runOpsAndValidate = [this, uuid]() {
        auto options1 = CollectionOptions{
            .uuid = uuid, .validator = fromjson("{'phone' : {'$type' : 'string' } }")};
        auto createColl1 =
            makeCreateCollectionOplogEntry(_opCtx.get(), nextOpTime(), _nss, options1);
        auto dropColl = makeCommandOplogEntry(nextOpTime(), _nss, BSON("drop" << _nss.coll()));

        auto options2 = CollectionOptions{
            .uuid = uuid, .validator = fromjson("{'phone' : {'$type' : 'number' } }")};
        auto createColl2 =
            makeCreateCollectionOplogEntry(_opCtx.get(), nextOpTime(), _nss, options2);

        auto ops = {createColl1, dropColl, createColl2};
        ASSERT_OK(runOpsInitialSync(ops));
        auto state = validate(_nss);

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
        auto collationOpts =
            BSON("locale" << "en"
                          << "caseLevel" << false << "caseFirst"
                          << "off"
                          << "strength" << 1 << "numericOrdering" << false << "alternate"
                          << "non-ignorable"
                          << "maxVariable"
                          << "punct"
                          << "normalization" << false << "backwards" << false << "version"
                          << "57.1");
        CollectionOptions options{
            .uuid = uuid,
            .idIndex = BSON("collation" << collationOpts << "key" << BSON("_id" << 1) << "name"
                                        << IndexConstants::kIdIndexName << "v" << 2),

            .collation = collationOpts};
        auto createColl = makeCreateCollectionOplogEntry(_opCtx.get(), nextOpTime(), _nss, options);
        auto insertOp1 = insert(fromjson("{ _id: 'foo' }"));
        auto updateOp1 = update("foo",
                                update_oplog_entry::makeDeltaOplogEntry(
                                    BSON(doc_diff::kUpdateSectionFieldName << fromjson("{x: 2}"))));
        auto deleteOp1 =
            makeDeleteDocumentOplogEntry(nextOpTime(), _nss, fromjson("{ _id: 'foo' }"));
        auto insertOp2 = insert(fromjson("{ _id: 'Foo', x: 1 }"));
        auto updateOp2 = update("Foo",
                                update_oplog_entry::makeDeltaOplogEntry(
                                    BSON(doc_diff::kUpdateSectionFieldName << fromjson("{x: 2}"))));

        // We don't drop and re-create the collection since we don't have ways
        // to wait until second-phase drop to completely finish.
        auto ops = {createColl, insertOp1, updateOp1, deleteOp1, insertOp2, updateOp2};
        ASSERT_OK(runOpsInitialSync(ops));
        auto state = validate(_nss);

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
    auto viewNss = NamespaceString::makeSystemDotViewsNamespace(_nss.dbName());
    ASSERT_OK(runOpInitialSync(
        makeCreateCollectionOplogEntry(_opCtx.get(), nextOpTime(), viewNss, options)));

    auto viewDoc = BSON(
        "_id"
        << NamespaceString::createNamespaceString_forTest(_nss.db_forTest(), "view").ns_forTest()
        << "viewOn" << _nss.coll() << "pipeline" << fromjson("[ { '$project' : { 'x' : 1 } } ]"));
    auto insertViewOp = makeInsertDocumentOplogEntry(nextOpTime(), viewNss, viewDoc);
    auto dropColl = makeCommandOplogEntry(nextOpTime(), _nss, BSON("drop" << _nss.coll()));

    auto ops = {insertViewOp, dropColl};
    testOpsAreIdempotent(ops);
}

TEST_F(IdempotencyTest, CollModNamespaceNotFound) {
    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    ASSERT_OK(runOpInitialSync(
        buildIndex(BSON("createdAt" << 1), BSON("expireAfterSeconds" << 3600), kUuid)));

    auto indexChange = fromjson("{keyPattern: {createdAt:1}, expireAfterSeconds:4000}");
    auto collModCmd = BSON("collMod" << _nss.coll() << "index" << indexChange);
    auto collModOp =
        makeCommandOplogEntry(nextOpTime(), _nss, collModCmd, boost::none /* object2 */, kUuid);
    auto dropCollOp = makeCommandOplogEntry(
        nextOpTime(), _nss, BSON("drop" << _nss.coll()), boost::none /* object2 */, kUuid);

    auto ops = {collModOp, dropCollOp};
    testOpsAreIdempotent(ops);
}

TEST_F(IdempotencyTest, CollModIndexNotFound) {
    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    ASSERT_OK(runOpInitialSync(
        buildIndex(BSON("createdAt" << 1), BSON("expireAfterSeconds" << 3600), kUuid)));

    auto indexChange = fromjson("{keyPattern: {createdAt:1}, expireAfterSeconds:4000}");
    auto collModCmd = BSON("collMod" << _nss.coll() << "index" << indexChange);
    auto collModOp =
        makeCommandOplogEntry(nextOpTime(), _nss, collModCmd, boost::none /* object2 */, kUuid);
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

    auto op = makeInsertDocumentOplogEntry(nextOpTime(), fcvNS, BSON("_id" << "other"));
    ASSERT_OK(runOpInitialSync(op));
}

TEST_F(IdempotencyTest, DropDatabaseSucceeds) {
    // Choose `system.profile` so the storage engine doesn't expect the drop to be timestamped.
    auto ns = NamespaceString::createNamespaceString_forTest("foo.system.profile");
    ::mongo::repl::createCollection(_opCtx.get(), ns, CollectionOptions());
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto op = makeCommandOplogEntry(nextOpTime(), ns, BSON("dropDatabase" << 1));
    ASSERT_OK(runOpInitialSync(op));
}

TEST_F(OplogApplierImplTest, DropDatabaseSucceedsInRecovering) {
    // Choose `system.profile` so the storage engine doesn't expect the drop to be timestamped.
    auto ns = NamespaceString::createNamespaceString_forTest("foo.system.profile");
    ::mongo::repl::createCollection(_opCtx.get(), ns, CollectionOptions());
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    auto op = makeCommandOplogEntry(nextOpTime(), ns, BSON("dropDatabase" << 1));
    ASSERT_OK(runOpSteadyState(op));
}

TEST_F(OplogApplierImplWithFastAutoAdvancingClockTest, LogSlowOpApplicationWhenSuccessful) {
    // This duration is greater than "slowMS", so the op would be considered slow.
    auto applyDuration = serverGlobalParams.slowMS.load() * 10;

    // We are inserting into an existing collection.
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    repl::createCollection(_opCtx.get(), nss, {});
    auto entry = makeOplogEntry(OpTypeEnum::kInsert, nss, {});

    unittest::LogCaptureGuard logs;
    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&entry}, OplogApplication::Mode::kSecondary));

    ASSERT_EQUALS(
        1,
        logs.countBSONContainingSubset(BSON(
            "attr" << BSON("CRUD" << BSON("ts" << Timestamp(1, 1) << "t" << 1LL << "v" << 2 << "op"
                                               << "i"
                                               << "ns"
                                               << "test.t"
                                               << "wall" << Date_t::fromMillisSinceEpoch(0))
                                  << "durationMillis" << applyDuration))));
}

TEST_F(OplogApplierImplWithFastAutoAdvancingClockTest, DoNotLogSlowOpApplicationWhenFailed) {
    // This duration is greater than "slowMS", so the op would be considered slow.
    auto applyDuration = serverGlobalParams.slowMS.load() * 10;

    // We are trying to insert into a non-existing database.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    auto entry = makeOplogEntry(OpTypeEnum::kInsert, nss, {});

    unittest::LogCaptureGuard logs;
    ASSERT_THROWS(_applyOplogEntryOrGroupedInsertsWrapper(
                      _opCtx.get(), ApplierOperation{&entry}, OplogApplication::Mode::kSecondary),
                  ExceptionFor<ErrorCodes::NamespaceNotFound>);

    // Use a builder for easier escaping. We expect the operation to *not* be logged
    // even thought it was slow, since we couldn't apply it successfully.
    StringBuilder expected;
    expected << "applied op: CRUD { op: \"i\", ns: \"test.t\", o: { _id: 0 }, ts: Timestamp(1, 1), "
                "t: 1, h: 1, v: 2 }, took "
             << applyDuration << "ms";
    ASSERT_EQUALS(0, logs.countTextContaining(expected.str()));
}

TEST_F(OplogApplierImplWithSlowAutoAdvancingClockTest, DoNotLogNonSlowOpApplicationWhenSuccessful) {
    // This duration is below "slowMS", so the op would *not* be considered slow.
    auto applyDuration = serverGlobalParams.slowMS.load() / 10;

    // We are inserting into an existing collection.
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    repl::createCollection(_opCtx.get(), nss, {});
    auto entry = makeOplogEntry(OpTypeEnum::kInsert, nss, {});

    unittest::LogCaptureGuard logs;
    ASSERT_OK(_applyOplogEntryOrGroupedInsertsWrapper(
        _opCtx.get(), ApplierOperation{&entry}, OplogApplication::Mode::kSecondary));

    // Use a builder for easier escaping. We expect the operation to *not* be logged,
    // since it wasn't slow to apply.
    StringBuilder expected;
    expected << "applied op: CRUD { op: \"i\", ns: \"test.t\", o: { _id: 0 }, ts: Timestamp(1, 1), "
                "t: 1, h: 1, v: 2 }, took "
             << applyDuration << "ms";
    ASSERT_EQUALS(0, logs.countTextContaining(expected.str()));
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

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(_opCtx.get());
        mongoDSessionCatalog->onStepUp(_opCtx.get());

        DBDirectClient client(_opCtx.get());
        BSONObj result;
        ASSERT(client.runCommand(kNs.dbName(), BSON("create" << kNs.coll()), result));
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
            boost::none,    // checkExistenceForDiffInsert
            boost::none,    // versionContext
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
            boost::none,    // checkExistenceForDiffInsert
            boost::none,    // versionContext
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

const NamespaceString OplogApplierImplTxnTableTest::kNs =
    NamespaceString::createNamespaceString_forTest("test.foo");

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

    auto workerPool = makeReplWorkerPool();

    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());

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

    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());


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

    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());

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

    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());

    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {insertOp, updateOp}));

    checkTxnTable(sessionInfo, newWriteOpTime, date);
}

TEST_F(OplogApplierImplTxnTableTest, RetryableWriteThenMultiStatementTxnWriteOnSameSession) {
    const NamespaceString cmdNss = NamespaceString::createNamespaceString_forTest("admin", "$cmd");
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    auto date = Date_t::now();
    auto uuid = [&] {
        return acquireCollForRead(_opCtx.get(), nss()).uuid();
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
        BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                << "ns" << nss().ns_forTest() << "ui" << uuid << "o"
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

    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());

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
    const NamespaceString cmdNss = NamespaceString::createNamespaceString_forTest("admin", "$cmd");
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);
    auto date = Date_t::now();
    auto uuid = [&] {
        return acquireCollForRead(_opCtx.get(), nss()).uuid();
    }();

    repl::OpTime txnInsertOpTime(Timestamp(1, 0), 1);
    auto txnInsertOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
        txnInsertOpTime,
        cmdNss,
        BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                << "ns" << nss().ns_forTest() << "ui" << uuid << "o"
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

    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());

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
    NamespaceString ns0 = NamespaceString::createNamespaceString_forTest("test.0");
    NamespaceString ns1 = NamespaceString::createNamespaceString_forTest("test.1");
    NamespaceString ns2 = NamespaceString::createNamespaceString_forTest("test.2");
    NamespaceString ns3 = NamespaceString::createNamespaceString_forTest("test.3");

    DBDirectClient client(_opCtx.get());
    BSONObj result;
    ASSERT(client.runCommand(ns0.dbName(), BSON("create" << ns0.coll()), result));
    ASSERT(client.runCommand(ns1.dbName(), BSON("create" << ns1.coll()), result));
    ASSERT(client.runCommand(ns2.dbName(), BSON("create" << ns2.coll()), result));
    ASSERT(client.runCommand(ns3.dbName(), BSON("create" << ns3.coll()), result));
    auto uuid0 = [&] {
        return acquireCollForRead(_opCtx.get(), ns0).uuid();
    }();
    auto uuid1 = [&] {
        return acquireCollForRead(_opCtx.get(), ns1).uuid();
    }();
    auto uuid2 = [&] {
        return acquireCollForRead(_opCtx.get(), ns2).uuid();
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

    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());

    ASSERT_OK(oplogApplier.applyOplogBatch(
        _opCtx.get(),
        {opSingle, opDiffTxnSmaller, opDiffTxnLarger, opSameTxnSooner, opSameTxnLater, opNoTxn}));

    // The txnNum and optime of the only write were saved.
    auto resultSingleDoc =
        client.findOne(NamespaceString::kSessionTransactionsTableNamespace,
                       BSON(SessionTxnRecord::kSessionIdFieldName << lsidSingle.toBSON()));
    ASSERT_TRUE(!resultSingleDoc.isEmpty());

    auto resultSingle =
        SessionTxnRecord::parse(resultSingleDoc, IDLParserContext("resultSingleDoc test"));

    ASSERT_EQ(resultSingle.getTxnNum(), 5LL);
    ASSERT_EQ(resultSingle.getLastWriteOpTime(), repl::OpTime(Timestamp(Seconds(1), 0), 1));

    // The txnNum and optime of the write with the larger txnNum were saved.
    auto resultDiffTxnDoc =
        client.findOne(NamespaceString::kSessionTransactionsTableNamespace,
                       BSON(SessionTxnRecord::kSessionIdFieldName << lsidDiffTxn.toBSON()));
    ASSERT_TRUE(!resultDiffTxnDoc.isEmpty());

    auto resultDiffTxn =
        SessionTxnRecord::parse(resultDiffTxnDoc, IDLParserContext("resultDiffTxnDoc test"));

    ASSERT_EQ(resultDiffTxn.getTxnNum(), 20LL);
    ASSERT_EQ(resultDiffTxn.getLastWriteOpTime(), repl::OpTime(Timestamp(Seconds(3), 0), 1));

    // The txnNum and optime of the write with the later optime were saved.
    auto resultSameTxnDoc =
        client.findOne(NamespaceString::kSessionTransactionsTableNamespace,
                       BSON(SessionTxnRecord::kSessionIdFieldName << lsidSameTxn.toBSON()));
    ASSERT_TRUE(!resultSameTxnDoc.isEmpty());

    auto resultSameTxn =
        SessionTxnRecord::parse(resultSameTxnDoc, IDLParserContext("resultSameTxnDoc test"));

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

    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());

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

    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());

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

    auto workerPool = makeReplWorkerPool();
    NoopOplogApplierObserver observer;
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
        workerPool.get());

    ASSERT_OK(oplogApplier.applyOplogBatch(_opCtx.get(), {oplog}));

    ASSERT_FALSE(docExists(
        _opCtx.get(),
        NamespaceString::kSessionTransactionsTableNamespace,
        BSON(SessionTxnRecord::kSessionIdFieldName << sessionInfo.getSessionId()->toBSON())));
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
    // Create a BSON "collMod" command.
    auto collModCmd = BSON("collMod" << _nss.coll());

    // Create a "collMod" oplog entry.
    auto collModOp = makeCommandOplogEntry(
        nextOpTime(), _nss, collModCmd, boost::none /* object2 */, UUID::gen());

    // Ensure that NamespaceNotFound is "acceptable" but counted.
    int prevAcceptableError = replOpCounters.getAcceptableErrorInCommand()->load();
    ASSERT_OK(runOpSteadyState(collModOp));

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
    // Create a BSON "collMod" command.
    auto collModCmd = BSON("collMod" << _nss.coll());

    // Create a "collMod" oplog entry.
    auto collModOp = makeCommandOplogEntry(
        nextOpTime(), _nss, collModCmd, boost::none /* object2 */, UUID::gen());

    // Ensure that NamespaceNotFound is returned.
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, runOpSteadyState(collModOp));
}

class IdempotencyTestTxns : public IdempotencyTest {};

// Document used by transaction idempotency tests.
const BSONObj doc = fromjson("{_id: 1}");
const BSONObj doc2 = fromjson("{_id: 2}");

TEST_F(IdempotencyTestTxns, CommitUnpreparedTransaction) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto commitOp = commitUnprepared(
        lsid, txnNum, StmtId(0), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));

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
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc));
}

TEST_F(IdempotencyTestTxns, CommitUnpreparedTransactionDataPartiallyApplied) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    NamespaceString nss2 = NamespaceString::createNamespaceString_forTest("test.coll2");
    auto uuid2 = createCollectionWithUuid(_opCtx.get(), nss2);

    auto commitOp = commitUnprepared(lsid,
                                     txnNum,
                                     StmtId(0),
                                     BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)
                                                << makeInsertApplyOpsEntry(nss2, uuid2, doc)));

    // Manually insert one of the documents so that the data will partially reflect the transaction
    // when the commitTransaction oplog entry is applied during initial sync.
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(),
                                                    _nss,
                                                    {doc, commitOp.getOpTime().getTimestamp()},
                                                    commitOp.getOpTime().getTerm()));
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc));
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
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), nss2, doc));
}

TEST_F(IdempotencyTestTxns, CommitPreparedTransaction) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto prepareOp =
        prepare(lsid, txnNum, StmtId(0), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));

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
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc));
}

TEST_F(IdempotencyTestTxns, CommitPreparedTransactionDataPartiallyApplied) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    NamespaceString nss2 = NamespaceString::createNamespaceString_forTest("test.coll2");
    auto uuid2 = createCollectionWithUuid(_opCtx.get(), nss2);

    auto prepareOp = prepare(lsid,
                             txnNum,
                             StmtId(0),
                             BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)
                                        << makeInsertApplyOpsEntry(nss2, uuid2, doc)));

    auto commitOp = commitPrepared(lsid, txnNum, StmtId(1), prepareOp.getOpTime());

    // Manually insert one of the documents so that the data will partially reflect the transaction
    // when the commitTransaction oplog entry is applied during initial sync.
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(),
                                                    _nss,
                                                    {doc, commitOp.getOpTime().getTimestamp()},
                                                    commitOp.getOpTime().getTerm()));
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc));
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
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), nss2, doc));
}

TEST_F(IdempotencyTestTxns, AbortPreparedTransaction) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto prepareOp =
        prepare(lsid, txnNum, StmtId(0), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));
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
    ASSERT_FALSE(docExists(_opCtx.get(), _nss, doc));
}

TEST_F(IdempotencyTestTxns, SinglePartialTxnOp) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));

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
    ASSERT_FALSE(docExists(_opCtx.get(), _nss, doc));
}

TEST_F(IdempotencyTestTxns, MultiplePartialTxnOps) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp1 = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));
    auto partialOp2 = partialTxn(lsid,
                                 txnNum,
                                 StmtId(1),
                                 partialOp1.getOpTime(),
                                 BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc2)));
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
    ASSERT_FALSE(docExists(_opCtx.get(), _nss, doc));
    ASSERT_FALSE(docExists(_opCtx.get(), _nss, doc2));
}

TEST_F(IdempotencyTestTxns, CommitUnpreparedTransactionWithPartialTxnOps) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));

    auto commitOp = commitUnprepared(lsid,
                                     txnNum,
                                     StmtId(1),
                                     BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc2)),
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
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc2));
}

TEST_F(IdempotencyTestTxns, CommitTwoUnpreparedTransactionsWithPartialTxnOpsAtOnce) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum1(1);
    TxnNumber txnNum2(2);

    auto partialOp1 = partialTxn(
        lsid, txnNum1, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));
    auto commitOp1 =
        commitUnprepared(lsid, txnNum1, StmtId(1), BSONArray(), partialOp1.getOpTime());

    // The second transaction (with a different transaction number) in the same session.
    auto partialOp2 = partialTxn(
        lsid, txnNum2, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc2)));
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
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc2));
}

TEST_F(IdempotencyTestTxns, CommitAndAbortTwoTransactionsWithPartialTxnOpsAtOnce) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum1(1);
    TxnNumber txnNum2(2);

    auto partialOp1 = partialTxn(
        lsid, txnNum1, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));
    auto abortOp1 = abortPrepared(lsid, txnNum1, StmtId(1), partialOp1.getOpTime());

    // The second transaction (with a different transaction number) in the same session.
    auto partialOp2 = partialTxn(
        lsid, txnNum2, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc2)));
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
    ASSERT_FALSE(docExists(_opCtx.get(), _nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc2));
}

TEST_F(IdempotencyTestTxns, CommitUnpreparedTransactionWithPartialTxnOpsAndDataPartiallyApplied) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));

    auto commitOp = commitUnprepared(lsid,
                                     txnNum,
                                     StmtId(1),
                                     BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc2)),
                                     partialOp.getOpTime());

    // Manually insert the first document so that the data will partially reflect the transaction
    // when the commitTransaction oplog entry is applied during initial sync. This simulates the
    // case where the transaction committed on the sync source at a point during the initial sync,
    // such that we cloned 'doc' but missed 'doc2'.
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(),
                                                    _nss,
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
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc2));
}

TEST_F(IdempotencyTestTxns, PrepareTransactionWithPartialTxnOps) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));
    auto prepareOp = prepare(lsid,
                             txnNum,
                             StmtId(1),
                             BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc2)),
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
    ASSERT_FALSE(docExists(_opCtx.get(), _nss, doc));
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
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));
    auto prepareOp = prepare(lsid,
                             txnNum,
                             StmtId(1),
                             BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc2)),
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
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc2));
}

TEST_F(IdempotencyTestTxns, CommitTwoPreparedTransactionsWithPartialTxnOpsAtOnce) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum1(1);
    TxnNumber txnNum2(2);

    auto partialOp1 = partialTxn(
        lsid, txnNum1, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));
    auto prepareOp1 = prepare(lsid, txnNum1, StmtId(1), BSONArray(), partialOp1.getOpTime());
    auto commitOp1 = commitPrepared(lsid, txnNum1, StmtId(2), prepareOp1.getOpTime());

    // The second transaction (with a different transaction number) in the same session.
    auto partialOp2 = partialTxn(
        lsid, txnNum2, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc2)));
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
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc2));
}

TEST_F(IdempotencyTestTxns, CommitPreparedTransactionWithPartialTxnOpsAndDataPartiallyApplied) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));
    auto prepareOp = prepare(lsid,
                             txnNum,
                             StmtId(1),
                             BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc2)),
                             partialOp.getOpTime());
    auto commitOp = commitPrepared(lsid, txnNum, StmtId(2), prepareOp.getOpTime());

    // Manually insert the first document so that the data will partially reflect the transaction
    // when the commitTransaction oplog entry is applied during initial sync. This simulates the
    // case where the transaction committed on the sync source at a point during the initial sync,
    // such that we cloned 'doc' but missed 'doc2'.
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(),
                                                    _nss,
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
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc));
    ASSERT_TRUE(docExists(_opCtx.get(), _nss, doc2));
}

TEST_F(IdempotencyTestTxns, AbortPreparedTransactionWithPartialTxnOps) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));
    auto prepareOp = prepare(lsid,
                             txnNum,
                             StmtId(1),
                             BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc2)),
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
    ASSERT_FALSE(docExists(_opCtx.get(), _nss, doc));
    ASSERT_FALSE(docExists(_opCtx.get(), _nss, doc2));
}

TEST_F(IdempotencyTestTxns, AbortInProgressTransaction) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    auto uuid = createCollectionWithUuid(_opCtx.get(), _nss);
    auto lsid = makeLogicalSessionId(_opCtx.get());
    TxnNumber txnNum(0);

    auto partialOp = partialTxn(
        lsid, txnNum, StmtId(0), OpTime(), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)));
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
    ASSERT_FALSE(docExists(_opCtx.get(), _nss, doc));
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
        lsid, txnNum, StmtId(1), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)), OpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));

    testOpsAreIdempotent({commitOp});

    // The op should have thrown a NamespaceNotFound error, which should have been ignored, so the
    // operation has no effect.
    ASSERT_FALSE(docExists(_opCtx.get(), _nss, doc));
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
        lsid, txnNum, StmtId(0), BSON_ARRAY(makeInsertApplyOpsEntry(_nss, uuid, doc)), OpTime());
    auto commitOp = commitPrepared(lsid, txnNum, StmtId(1), prepareOp.getOpTime());

    ASSERT_OK(ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(MemberState::RS_RECOVERING));
    testOpsAreIdempotent({prepareOp, commitOp});

    // The op should have thrown a NamespaceNotFound error, which should have been ignored, so the
    // operation has no effect.
    ASSERT_FALSE(docExists(_opCtx.get(), _nss, doc));
}

class PreparedTxnSplitTest : public OplogApplierImplTest {
public:
    PreparedTxnSplitTest()
        : _nss(NamespaceString::createNamespaceString_forTest("test.prepTxnSplit")),
          _cmdNss(NamespaceString::createNamespaceString_forTest("admin.$cmd")),
          _uuid(UUID::gen()),
          _txnNum1(1),
          _txnNum2(2) {}

protected:
    using WriterVectors = std::vector<std::vector<ApplierOperation>>;

    void setUp() override {
        OplogApplierImplTest::setUp();

        _lsid1 = makeLogicalSessionId(_opCtx.get());
        _lsid2 = makeLogicalSessionId(_opCtx.get());

        NoopOplogApplierObserver observer;
        _workerPool = makeReplWorkerPool();
        _applier = std::make_unique<TrackOpsAppliedApplier>(
            nullptr,  // executor
            nullptr,  // oplogBuffer
            &observer,
            ReplicationCoordinator::get(_opCtx.get()),
            getConsistencyMarkers(),
            getStorageInterface(),
            repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary, false),
            _workerPool.get());
    }

    auto makePrepareOplogEntry(const std::vector<BSONObj>& docs,
                               const OpTime opTime,
                               const LogicalSessionId& lsid,
                               TxnNumber txnNumber) {
        BSONArrayBuilder arrBuilder;
        arrBuilder.append(docs.begin(), docs.end());
        auto command = BSON("applyOps" << arrBuilder.arr() << "prepare" << true);

        return makeCommandOplogEntryWithSessionInfoAndStmtIds(
            opTime, _cmdNss, command, lsid, TxnNumber(txnNumber), {StmtId(0)}, OpTime());
    }

    auto makeCommitOplogEntry(const OpTime opTime,
                              const Timestamp commitTs,
                              const LogicalSessionId& lsid,
                              TxnNumber txnNumber,
                              const OpTime prevOpTime) {
        auto command = BSON("commitTransaction" << 1 << "commitTimestamp" << commitTs);

        return makeCommandOplogEntryWithSessionInfoAndStmtIds(
            opTime, _cmdNss, command, lsid, TxnNumber(txnNumber), {StmtId(0)}, prevOpTime);
    }

    auto makeAbortOplogEntry(const OpTime opTime,
                             const LogicalSessionId& lsid,
                             TxnNumber txnNumber,
                             const OpTime prevOpTime) {
        auto command = BSON("abortTransaction" << 1);

        return makeCommandOplogEntryWithSessionInfoAndStmtIds(
            opTime, _cmdNss, command, lsid, TxnNumber(txnNumber), {StmtId(0)}, prevOpTime);
    }

    int filterConfigTransactionsEntriesFromWriterVectors(WriterVectors& writerVectors) {
        int txnTableOps = 0;
        for (auto& vector : writerVectors) {
            for (auto it = vector.begin(); it != vector.end();) {
                if ((*it)->getNss() == NamespaceString::kSessionTransactionsTableNamespace) {
                    txnTableOps++;
                    it = vector.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return txnTableOps;
    }

    int filterTopLevelTxnEntriesFromWriterVectors(WriterVectors& writerVectors) {
        int topLevelTxnOps = 0;
        for (auto& vector : writerVectors) {
            for (auto it = vector.begin(); it != vector.end();) {
                if ((*it).instruction == ApplicationInstruction::applyTopLevelPreparedTxnOp) {
                    topLevelTxnOps++;
                    it = vector.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return topLevelTxnOps;
    }

    int getDocumentIdFromOp(const OplogEntry* op) {
        switch (op->getOpType()) {
            case OpTypeEnum::kInsert:
            case OpTypeEnum::kDelete:
                return op->getObject()["_id"].Int();
            case OpTypeEnum::kUpdate:
                return (*op->getObject2())["_id"].Int();
            default:
                MONGO_UNREACHABLE
        }
    }

protected:
    std::unique_ptr<TrackOpsAppliedApplier> _applier;
    std::unique_ptr<ThreadPool> _workerPool;
    NamespaceString _nss;
    NamespaceString _cmdNss;
    boost::optional<UUID> _uuid;
    LogicalSessionId _lsid1;
    LogicalSessionId _lsid2;
    TxnNumber _txnNum1;
    TxnNumber _txnNum2;
};

TEST_F(PreparedTxnSplitTest, MultiplePrepareTxnsInSameBatch) {
    // Scale the test by the number of writer threads, so it does not start failing if maxThreads
    // changes.
    const int kNumEntries = _workerPool->getStats().options.maxThreads * 1000;

    std::vector<OplogEntry> prepareOps;
    std::vector<BSONObj> cruds1;
    std::vector<BSONObj> cruds2;
    cruds1.reserve(kNumEntries);
    cruds2.reserve(kNumEntries);

    for (int i = 0; i < kNumEntries; i++) {
        cruds1.push_back(BSON("op" << "i"
                                   << "ns" << _nss.ns_forTest() << "ui" << *_uuid << "o"
                                   << BSON("_id" << i)));
        cruds2.push_back(BSON("op" << "i"
                                   << "ns" << _nss.ns_forTest() << "ui" << *_uuid << "o"
                                   << BSON("_id" << i + kNumEntries)));
    }

    prepareOps.push_back(makePrepareOplogEntry(cruds1, {Timestamp(1, 1), 1}, _lsid1, _txnNum1));
    prepareOps.push_back(makePrepareOplogEntry(cruds2, {Timestamp(1, 2), 1}, _lsid2, _txnNum2));

    WriterVectors prepareWriterVectors(_workerPool->getStats().options.maxThreads);
    std::vector<std::vector<OplogEntry>> derivedPrepareOps;
    _applier->fillWriterVectors_forTest(
        _opCtx.get(), &prepareOps, &prepareWriterVectors, &derivedPrepareOps);

    // Verify the config.transactions collection got one entry for each prepared transaction.
    int txnTableOps = filterConfigTransactionsEntriesFromWriterVectors(prepareWriterVectors);
    ASSERT_EQ(2, txnTableOps);

    // Verify each top-level transaction has a corresponding prepare entry in the writerVectors.
    int topLevelTxnOps = filterTopLevelTxnEntriesFromWriterVectors(prepareWriterVectors);
    ASSERT_EQ(2, topLevelTxnOps);

    // Verify each writer has been assigned operations for both prepared transactions.
    for (auto& writer : prepareWriterVectors) {
        ASSERT_EQ(2, writer.size());

        ASSERT_EQ(ApplicationInstruction::applySplitPreparedTxnOp, writer[0].instruction);
        ASSERT_TRUE(writer[0]->shouldPrepare());
        ASSERT_NE(boost::none, writer[0].subSession);
        ASSERT_NE(boost::none, writer[0].preparedTxnOps);
        ASSERT_FALSE(writer[0].preparedTxnOps->empty());

        ASSERT_EQ(ApplicationInstruction::applySplitPreparedTxnOp, writer[1].instruction);
        ASSERT_TRUE(writer[1]->shouldPrepare());
        ASSERT_NE(boost::none, writer[1].subSession);
        ASSERT_NE(boost::none, writer[1].preparedTxnOps);
        ASSERT_FALSE(writer[1].preparedTxnOps->empty());

        ASSERT_NE(writer[0].subSession->getSessionId(), writer[1].subSession->getSessionId());
    }

    // Test that applying a commitTransaction or abortTransaction entry in the next batch will
    // correctly split the entry and add them into those writer vectors that previously got
    // assigned the prepare entry.
    std::vector<OplogEntry> commitOps;
    commitOps.push_back(makeCommitOplogEntry(
        {Timestamp(3, 1), 1}, Timestamp(2, 1), _lsid1, _txnNum1, prepareOps[0].getOpTime()));
    commitOps.push_back(
        makeAbortOplogEntry({Timestamp(3, 2), 1}, _lsid2, _txnNum2, prepareOps[1].getOpTime()));

    WriterVectors commitWriterVectors(_workerPool->getStats().options.maxThreads);
    std::vector<std::vector<OplogEntry>> derivedCommitOps;
    _applier->fillWriterVectors_forTest(
        _opCtx.get(), &commitOps, &commitWriterVectors, &derivedCommitOps);

    // Verify the config.transactions collection got one entry for each commit/abort op.
    txnTableOps = filterConfigTransactionsEntriesFromWriterVectors(commitWriterVectors);
    ASSERT_EQ(2, txnTableOps);

    // Verify each top-level transaction has a corresponding commit entry in the writerVectors.
    topLevelTxnOps = filterTopLevelTxnEntriesFromWriterVectors(commitWriterVectors);
    ASSERT_EQ(2, topLevelTxnOps);

    // Verify each writer has been assigned a split commit and abort op.
    for (size_t i = 0; i < commitWriterVectors.size(); ++i) {
        auto& commitWriter = commitWriterVectors[i];
        auto& prepareWriter = prepareWriterVectors[i];

        ASSERT_EQ(2, commitWriter.size());
        ASSERT_EQ(2, prepareWriter.size());

        ASSERT_EQ(ApplicationInstruction::applySplitPreparedTxnOp, commitWriter[0].instruction);
        ASSERT_TRUE(commitWriter[0]->isPreparedCommit());
        ASSERT_EQ(prepareWriter[0].subSession->getSessionId(),
                  commitWriter[0].subSession->getSessionId());
        ASSERT_EQ(boost::none, commitWriter[0].preparedTxnOps);

        ASSERT_EQ(ApplicationInstruction::applySplitPreparedTxnOp, commitWriter[1].instruction);
        ASSERT_EQ(prepareWriter[1].subSession->getSessionId(),
                  commitWriter[1].subSession->getSessionId());
        ASSERT_NE(boost::none, commitWriter[1].subSession);
        ASSERT_EQ(boost::none, commitWriter[1].preparedTxnOps);

        ASSERT_NE(commitWriter[0].subSession->getSessionId(),
                  commitWriter[1].subSession->getSessionId());
    }
}

TEST_F(PreparedTxnSplitTest, SingleEmptyPrepareTransaction) {
    std::vector<OplogEntry> prepareOps;
    prepareOps.push_back(makePrepareOplogEntry({}, {Timestamp(1, 1), 1}, _lsid1, _txnNum1));

    WriterVectors prepareWriterVectors(_workerPool->getStats().options.maxThreads);
    std::vector<std::vector<OplogEntry>> derivedPrepareOps;
    _applier->fillWriterVectors_forTest(
        _opCtx.get(), &prepareOps, &prepareWriterVectors, &derivedPrepareOps);

    // Verify the config.transactions collection got an entry for the empty prepared transaction.
    int txnTableOps = filterConfigTransactionsEntriesFromWriterVectors(prepareWriterVectors);
    ASSERT_EQ(1, txnTableOps);

    // Verify each top-level transaction has a corresponding prepare entry in the writerVectors.
    int topLevelTxnOps = filterTopLevelTxnEntriesFromWriterVectors(prepareWriterVectors);
    ASSERT_EQ(1, topLevelTxnOps);

    // Verify exactly one writer has been assigned operation for the empty prepared transaction.
    boost::optional<uint32_t> writerId;
    for (size_t i = 0; i < prepareWriterVectors.size(); ++i) {
        auto& writer = prepareWriterVectors[i];
        ASSERT_LTE(writer.size(), 1);
        if (writer.size() == 1) {
            ASSERT_EQ(boost::none, writerId);
            writerId = i;
            ASSERT_EQ(ApplicationInstruction::applySplitPreparedTxnOp, writer[0].instruction);
            ASSERT_TRUE(writer[0]->shouldPrepare());
            ASSERT_NE(boost::none, writer[0].subSession);
            ASSERT_NE(boost::none, writer[0].preparedTxnOps);
            ASSERT_TRUE(writer[0].preparedTxnOps->empty());
        }
    }

    // Test that applying a commitTransaction in the next batch will correctly split the entry
    // and add it into those writer vectors that previously got assigned the prepare entry.
    std::vector<OplogEntry> commitOps;
    commitOps.push_back(makeCommitOplogEntry(
        {Timestamp(3, 1), 1}, Timestamp(2, 1), _lsid1, _txnNum1, prepareOps[0].getOpTime()));

    WriterVectors commitWriterVectors(_workerPool->getStats().options.maxThreads);
    std::vector<std::vector<OplogEntry>> derivedCommitOps;
    _applier->fillWriterVectors_forTest(
        _opCtx.get(), &commitOps, &commitWriterVectors, &derivedCommitOps);

    // Verify the config.transactions collection got an entry for the commit op.
    txnTableOps = filterConfigTransactionsEntriesFromWriterVectors(commitWriterVectors);
    ASSERT_EQ(1, txnTableOps);

    // Verify each top-level transaction has a corresponding commit entry in the writerVectors.
    topLevelTxnOps = filterTopLevelTxnEntriesFromWriterVectors(commitWriterVectors);
    ASSERT_EQ(1, topLevelTxnOps);

    // Verify exactly one writer has been assigned a split commit op.
    for (size_t i = 0; i < commitWriterVectors.size(); ++i) {
        auto& commitWriter = commitWriterVectors[i];
        auto& prepareWriter = prepareWriterVectors[i];
        ASSERT_LTE(commitWriter.size(), 1);
        ASSERT_LTE(prepareWriter.size(), 1);
        ASSERT_EQ(prepareWriter.size(), commitWriter.size());

        if (commitWriter.size() == 1) {
            ASSERT_EQ(writerId.get(), i);
            ASSERT_EQ(ApplicationInstruction::applySplitPreparedTxnOp, commitWriter[0].instruction);
            ASSERT_TRUE(commitWriter[0]->isPreparedCommit());
            ASSERT_EQ(prepareWriter[0].subSession->getSessionId(),
                      commitWriter[0].subSession->getSessionId());
            ASSERT_EQ(boost::none, commitWriter[0].preparedTxnOps);
        }
    }
}

// For the SinglePreparedTxnMultipleOpsOnOneDoc, we need to make sure we don't fail spuriously
// due to a hash collision.
int findDocIdWithSeparateWriterId(
    OperationContext* opCtx, const NamespaceString& nss, UUID uuid, int docId, int nWriters) {

    invariant(nWriters > 1);
    const int itersToCheck = std::min(nWriters, 10);
    CachedCollectionProperties collPropertiesCache;
    auto oplogEntry1 =
        makeOplogEntry(OpTypeEnum::kInsert, nss, uuid, BSON("_id" << docId), boost::none);
    uint32_t id1 =
        OplogApplierUtils::getOplogEntryHash(opCtx, &oplogEntry1, &collPropertiesCache) % nWriters;
    for (int newDocId = 1 + docId; newDocId <= docId + itersToCheck; newDocId++) {
        auto oplogEntry2 =
            makeOplogEntry(OpTypeEnum::kInsert, nss, uuid, BSON("_id" << newDocId), boost::none);
        uint32_t id2 =
            OplogApplierUtils::getOplogEntryHash(opCtx, &oplogEntry2, &collPropertiesCache) %
            nWriters;
        if (id1 != id2)
            return newDocId;
    }
    invariant(false,
              str::stream() << "Unable to find a non-colliding doc ID between " << docId + 1
                            << " and " << docId + itersToCheck << " inclusive");
    MONGO_UNREACHABLE;
}

TEST_F(PreparedTxnSplitTest, SinglePreparedTxnMultipleOpsOnOneDoc) {
    std::vector<OplogEntry> prepareOps;
    const int kNumDocuments = 2;
    const int kNumOpsPerDoc = 3;
    const int kNumApplierThreads = 100;
    const int kDocID1 = 1001;
    const int kDocID2 =
        findDocIdWithSeparateWriterId(_opCtx.get(), _nss, *_uuid, kDocID1, kNumApplierThreads);

    // Construct one insert, one update and one delete op for each document.
    std::vector<BSONObj> cruds;
    cruds.push_back(BSON("op" << "i"
                              << "ns" << _nss.ns_forTest() << "ui" << *_uuid << "o"
                              << BSON("_id" << kDocID1)));
    cruds.push_back(BSON("op" << "i"
                              << "ns" << _nss.ns_forTest() << "ui" << *_uuid << "o"
                              << BSON("_id" << kDocID2)));
    cruds.push_back(BSON("op" << "u"
                              << "ns" << _nss.ns_forTest() << "ui" << *_uuid << "o"
                              << BSON("a" << 11) << "o2" << BSON("_id" << kDocID1)));
    cruds.push_back(BSON("op" << "u"
                              << "ns" << _nss.ns_forTest() << "ui" << *_uuid << "o"
                              << BSON("a" << 12) << "o2" << BSON("_id" << kDocID2)));
    cruds.push_back(BSON("op" << "d"
                              << "ns" << _nss.ns_forTest() << "ui" << *_uuid << "o"
                              << BSON("_id" << kDocID1)));
    cruds.push_back(BSON("op" << "d"
                              << "ns" << _nss.ns_forTest() << "ui" << *_uuid << "o"
                              << BSON("_id" << kDocID2)));

    prepareOps.push_back(makePrepareOplogEntry(cruds, {Timestamp(1, 1), 1}, _lsid1, _txnNum1));

    WriterVectors prepareWriterVectors(kNumApplierThreads);
    std::vector<std::vector<OplogEntry>> derivedPrepareOps;
    _applier->fillWriterVectors_forTest(
        _opCtx.get(), &prepareOps, &prepareWriterVectors, &derivedPrepareOps);

    // Verify the config.transactions collection got an entry for the prepared transaction.
    int txnTableOps = filterConfigTransactionsEntriesFromWriterVectors(prepareWriterVectors);
    ASSERT_EQ(1, txnTableOps);

    // Verify each top-level transaction has a corresponding prepare entry in the writerVectors.
    int topLevelTxnOps = filterTopLevelTxnEntriesFromWriterVectors(prepareWriterVectors);
    ASSERT_EQ(1, topLevelTxnOps);

    // Test that applying a commitTransaction in the next batch will correctly split the entry
    // and add it into those writer vectors that previously got assigned the prepare entry.
    std::vector<OplogEntry> commitOps;
    commitOps.push_back(makeCommitOplogEntry(
        {Timestamp(3, 1), 1}, Timestamp(2, 1), _lsid1, _txnNum1, prepareOps[0].getOpTime()));

    WriterVectors commitWriterVectors(kNumApplierThreads);
    std::vector<std::vector<OplogEntry>> derivedCommitOps;
    _applier->fillWriterVectors_forTest(
        _opCtx.get(), &commitOps, &commitWriterVectors, &derivedCommitOps);

    // Verify the config.transactions collection got an entry for the commit op.
    txnTableOps = filterConfigTransactionsEntriesFromWriterVectors(commitWriterVectors);
    ASSERT_EQ(1, txnTableOps);

    // Verify each top-level transaction has a corresponding commit entry in the writerVectors.
    topLevelTxnOps = filterTopLevelTxnEntriesFromWriterVectors(commitWriterVectors);
    ASSERT_EQ(1, topLevelTxnOps);

    // Verify that ops on the same document are grouped into the same split transaction.
    int splitTxnCount = 0;
    std::set<int> docIDs;
    std::set<int> expectDocIDs{kDocID1, kDocID2};

    for (size_t i = 0; i < kNumApplierThreads; ++i) {
        auto& prepareWriter = prepareWriterVectors[i];
        auto& commitWriter = commitWriterVectors[i];

        if (prepareWriter.size()) {
            ++splitTxnCount;
            ASSERT_EQ(1, prepareWriter.size());
            ASSERT_EQ(1, commitWriter.size());

            ASSERT_EQ(ApplicationInstruction::applySplitPreparedTxnOp,
                      prepareWriter[0].instruction);
            ASSERT_TRUE(prepareWriter[0]->shouldPrepare());
            ASSERT_NE(boost::none, prepareWriter[0].subSession);
            ASSERT_NE(boost::none, prepareWriter[0].preparedTxnOps);
            ASSERT_EQ(kNumOpsPerDoc, prepareWriter[0].preparedTxnOps->size());

            // Check all ops in one split transaction are on the same document.
            const auto& ops = *prepareWriter[0].preparedTxnOps;
            const int firstDocID = getDocumentIdFromOp(ops[0]);
            ASSERT_TRUE(std::all_of(ops.begin(), ops.end(), [=, this](const auto op) {
                return getDocumentIdFromOp(op) == firstDocID;
            }));
            docIDs.insert(firstDocID);

            ASSERT_EQ(ApplicationInstruction::applySplitPreparedTxnOp, commitWriter[0].instruction);
            ASSERT_TRUE(commitWriter[0]->isPreparedCommit());
            ASSERT_EQ(prepareWriter[0].subSession->getSessionId(),
                      commitWriter[0].subSession->getSessionId());
            ASSERT_EQ(boost::none, commitWriter[0].preparedTxnOps);
        }
    }
    ASSERT_EQ(kNumDocuments, splitTxnCount);
    ASSERT_EQ(expectDocIDs, docIDs);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
