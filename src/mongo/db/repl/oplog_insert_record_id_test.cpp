/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>

namespace mongo {
namespace repl {
namespace {

/**
 * Creates a insert oplog entry without a recordId. Uses DurableOplogEntryParams to construct
 * the entry.
 */
OplogEntry makeInsertOplogEntry(OpTime opTime,
                                const NamespaceString& nss,
                                const UUID& uuid,
                                const BSONObj& docToInsert) {
    return {DurableOplogEntry{DurableOplogEntryParams{
        .opTime = opTime,
        .opType = OpTypeEnum::kInsert,
        .nss = nss,
        .uuid = uuid,
        .oField = docToInsert,
        .wallClockTime = Date_t::now(),
    }}};
}

/**
 * Creates a insert oplog entry with the given recordId. Uses DurableOplogEntryParams to construct
 * the entry, then adds the recordId since it's not included in the params struct.
 */
OplogEntry makeInsertOplogEntryWithRecordId(OpTime opTime,
                                            const NamespaceString& nss,
                                            const UUID& uuid,
                                            const BSONObj& docToInsert,
                                            const RecordId& rid) {
    OplogEntry baseEntry = makeInsertOplogEntry(opTime, nss, uuid, docToInsert);

    // Add the recordId field since it's not included in DurableOplogEntryParams.
    BSONObjBuilder builder;
    builder.appendElements(baseEntry.getEntry().toBSON());
    rid.serializeToken("rid", &builder);

    return {DurableOplogEntry(builder.obj())};
}

/**
 * Creates a collection with recordIdsReplicated enabled.
 */
UUID createCollectionWithRecordIdsReplicated(OperationContext* opCtx, const NamespaceString& nss) {
    CollectionOptions options;
    options.uuid = UUID::gen();
    options.recordIdsReplicated = true;
    createCollection(opCtx, nss, options);
    return options.uuid.value();
}

/**
 * Inserts a document into a collection at a specific recordId.
 */
void insertDocumentAtRecordId(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& doc,
                              const RecordId& rid) {
    WriteUnitOfWork wuow(opCtx);
    AutoGetCollection coll(opCtx, nss, MODE_IX);
    ASSERT(coll);

    InsertStatement stmt{doc};
    stmt.replicatedRecordId = rid;
    ASSERT_OK(collection_internal::insertDocument(opCtx, *coll, stmt, nullptr /* opDebug */));

    wuow.commit();
}

/**
 * Returns document at a specific recordId if it exists.
 */
boost::optional<BSONObj> documentAtRecordId(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const RecordId& rid) {
    AutoGetCollection coll(opCtx, nss, MODE_IS);
    if (!coll) {
        return boost::none;
    }
    auto cursor = coll->getCursor(opCtx);
    auto record = cursor->seekExact(rid);
    if (record.has_value()) {
        return record->data.getOwned().releaseToBson();
    }
    return boost::none;
}

/**
 * Returns true if a document exists at a specific recordId.
 */
bool documentExistsAtRecordId(OperationContext* optCtx,
                              const NamespaceString& nss,
                              const RecordId& rid) {
    return documentAtRecordId(optCtx, nss, rid).has_value();
}

/**
 * Test fixture for insert oplog entries with recordId.
 */
class InsertTest : public OplogApplierImplTest {
protected:
    void setUp() override {
        OplogApplierImplTest::setUp();
        _nss = NamespaceString::createNamespaceString_forTest("test.insertRecordId");
        _uuid = createCollectionWithRecordIdsReplicated(_opCtx.get(), _nss);
    }

    NamespaceString _nss;
    UUID _uuid = UUID::gen();
};

template <typename T, bool enable>
class SetSteadyStateConstraints : public T {
protected:
    void setUp() override {
        T::setUp();
        _constraintsEnabled = oplogApplicationEnforcesSteadyStateConstraints.load();
        oplogApplicationEnforcesSteadyStateConstraints.store(enable);
    }

    void tearDown() override {
        oplogApplicationEnforcesSteadyStateConstraints.store(_constraintsEnabled);
        T::tearDown();
    }

private:
    bool _constraintsEnabled;
};

// =============================================================================
// Tests for kSecondary mode (steady state replication)
// =============================================================================


typedef SetSteadyStateConstraints<InsertTest, true> InsertEnabledSteadyStateConstraintsTest;
typedef SetSteadyStateConstraints<InsertTest, false> InsertDisabledSteadyStateConstraintsTest;

using InsertEnabledSteadyStateConstraintsDeathTest = InsertEnabledSteadyStateConstraintsTest;
using InsertDisabledSteadyStateConstraintsDeathTest = InsertDisabledSteadyStateConstraintsTest;

TEST_F(InsertEnabledSteadyStateConstraintsTest,
       InsertByRecordId_NonExistentDocumentAtRecordId_NewId_Inserts) {
    // Insert multiple documents.
    const RecordId rid1(1);
    const RecordId rid2(2);
    const RecordId rid3(3);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 2 << "x" << 200), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 3 << "x" << 300), rid3);

    // Insert a document at a non-existent recordId with a non-existent _id.
    const RecordId rid(4);
    const BSONObj doc = BSON("_id" << 4 << "x" << 100);

    // Verify no document exists at recordId.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Verify the _id does not exists.
    StorageInterfaceImpl storage;
    ASSERT_NOT_OK(storage.findById(_opCtx.get(), _nss, doc["_id"]).getStatus());

    // Create and apply a insert oplog entry WITH recordId in kSecondary mode.
    auto op = makeInsertOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, doc, rid);
    ASSERT_OK(runOpSteadyState(op));

    // Verify the document was inserted.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    ASSERT_BSONOBJ_EQ(doc, *documentAtRecordId(_opCtx.get(), _nss, rid));
}

TEST_F(InsertEnabledSteadyStateConstraintsTest,
       InsertByRecordId_ExistsDocumentAtRecordId_MatchingId_fails) {
    // Insert multiple documents.
    const RecordId rid1(1);
    const RecordId rid2(2);
    const RecordId rid3(3);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 2 << "x" << 200), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 3 << "x" << 300), rid3);

    // Insert a document at a recordId that exists and with the same _id.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 200 << "y" << 100);

    // Verify a document exists at the recordId.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Verify the _id is the same
    ASSERT_BSONELT_EQ(doc["_id"], (*documentAtRecordId(_opCtx.get(), _nss, rid))["_id"]);

    // Create and apply a insert oplog entry WITH recordId in kSecondary mode.
    auto op = makeInsertOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, doc, rid);
    ASSERT_EQ(ErrorCodes::DuplicateKey, runOpSteadyState(op).code());
}

DEATH_TEST_F(InsertDisabledSteadyStateConstraintsDeathTest,
             InsertByRecordId_ExistsDocumentAtRecordId_MatchingId_fails,
             "8830900") {
    // Insert multiple documents.
    const RecordId rid1(1);
    const RecordId rid2(2);
    const RecordId rid3(3);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 2 << "x" << 200), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 3 << "x" << 300), rid3);

    // Insert a document at a recordId that exists and with the same _id.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 200 << "y" << 100);

    // Verify a document exists at the recordId.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    //
    // Verify the _id is the same
    ASSERT_BSONELT_EQ(doc["_id"], (*documentAtRecordId(_opCtx.get(), _nss, rid))["_id"]);

    // Create and apply a insert oplog entry WITH recordId in kSecondary mode.
    auto op = makeInsertOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, doc, rid);
    // This should throw
    std::ignore = runOpSteadyState(op);
}

TEST_F(InsertEnabledSteadyStateConstraintsTest,
       InsertByRecordId_ExistsDocumentAtRecordId_NonMatchingId_Fails) {
    // Insert multiple documents.
    const RecordId rid1(1);
    const RecordId rid2(2);
    const RecordId rid3(3);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 2 << "x" << 200), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 3 << "x" << 300), rid3);

    // Insert a document at a recordId that exists but with different _id value.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 4 << "x" << 200 << "y" << 100);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Verify the _id are different
    ASSERT_BSONELT_NE(doc["_id"], (*documentAtRecordId(_opCtx.get(), _nss, rid))["_id"]);

    // Create and apply an insert oplog entry WITH recordId in secondary mode.
    auto op = makeInsertOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, doc, rid);
    ASSERT_EQ(ErrorCodes::DuplicateKey, runOpSteadyState(op).code());
}

TEST_F(InsertEnabledSteadyStateConstraintsTest,
       InsertByRecordId_NonExistentDocumentAtRecordId_ExistentId_Fails) {
    // Insert multiple documents.
    const RecordId rid1(1);
    const RecordId rid2(2);
    const RecordId rid3(3);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 2 << "x" << 200), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 3 << "x" << 300), rid3);

    // Insert a document at a non-existent recordId but with an _id that does exists.
    const RecordId rid(4);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);

    // Verify no document exists at the recordId.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Verify the _id does exists.
    StorageInterfaceImpl storage;
    ASSERT_OK(storage.findById(_opCtx.get(), _nss, doc["_id"]).getStatus());

    // Create and apply a insert oplog entry WITH recordId in secondary mode.
    auto op = makeInsertOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, doc, rid);
    ASSERT_EQ(ErrorCodes::DuplicateKey, runOpSteadyState(op).code());
}

DEATH_TEST_F(InsertDisabledSteadyStateConstraintsTest,
             InsertByRecordId_NonExistentDocumentAtRecordId_ExistentId_Fails,
             "8830900") {
    // Insert multiple documents.
    const RecordId rid1(1);
    const RecordId rid2(2);
    const RecordId rid3(3);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 2 << "x" << 200), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 3 << "x" << 300), rid3);

    // Insert a document at a non-existent recordId but with an _id that does exists.
    const RecordId rid(4);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);

    // Verify no document exists at the recordId.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Verify the _id does exists.
    StorageInterfaceImpl storage;
    ASSERT_OK(storage.findById(_opCtx.get(), _nss, doc["_id"]).getStatus());

    // Create and apply a insert oplog entry WITH recordId in secondary mode.
    auto op = makeInsertOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, doc, rid);
    std::ignore = runOpSteadyState(op);
}

// =============================================================================
// Tests for kApplyOpsCmd mode (applyOps command)
// =============================================================================

/**
 * Test fixture for insert oplog entries in applyOps mode on a recordIdsReplicated collection.
 */
class ApplyOpsInsertTest : public InsertTest {
protected:
    Status runOpApplyOpsCmd(const OplogEntry& op) {
        Status status = _applyOplogEntryOrGroupedInsertsWrapper(
            _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kApplyOpsCmd);
        return status;
    }
};

typedef SetSteadyStateConstraints<ApplyOpsInsertTest, true>
    ApplyOpsInsertEnabledSteadyStateConstraintsTest;
typedef SetSteadyStateConstraints<ApplyOpsInsertTest, false>
    ApplyOpsInsertDisabledSteadyStateConstraintsTest;

using ApplyOpsInsertEnabledSteadyStateConstraintsDeathTest =
    ApplyOpsInsertEnabledSteadyStateConstraintsTest;
using ApplyOpsInsertDisabledSteadyStateConstraintsDeathTest =
    ApplyOpsInsertDisabledSteadyStateConstraintsTest;

TEST_F(ApplyOpsInsertEnabledSteadyStateConstraintsTest, InsertById_NewId_Inserts) {
    // Insert multiple documents.
    const RecordId rid1(1);
    const RecordId rid2(2);
    const RecordId rid3(3);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 2 << "x" << 200), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 3 << "x" << 300), rid3);

    // Insert a document with non existent _id.
    const BSONObj doc = BSON("_id" << 4 << "x" << 100);

    // Verify the _id does not exists.
    StorageInterfaceImpl storage;
    ASSERT_NOT_OK(storage.findById(_opCtx.get(), _nss, doc["_id"]).getStatus());

    // Create and apply a insert oplog entry WITHOUT recordId in applyOps mode.
    auto op = makeInsertOplogEntry(nextOpTime(), _nss, _uuid, doc);
    ASSERT_OK(runOpApplyOpsCmd(op));

    // Verify the document was inserted.
    ASSERT_OK(storage.findById(_opCtx.get(), _nss, doc["_id"]).getStatus());
    ASSERT_BSONOBJ_EQ(doc, storage.findById(_opCtx.get(), _nss, doc["_id"]).getValue());
}

TEST_F(ApplyOpsInsertEnabledSteadyStateConstraintsTest, InsertById_ExistentId_Upserts) {
    // Insert multiple documents.
    const RecordId rid1(1);
    const RecordId rid2(2);
    const RecordId rid3(3);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 2 << "x" << 200), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 3 << "x" << 300), rid3);

    // Insert a document with existent _id.
    const BSONObj doc = BSON("_id" << 1 << "x" << 200 << "y" << 100);

    // Verify the _id exists
    StorageInterfaceImpl storage;
    ASSERT_OK(storage.findById(_opCtx.get(), _nss, doc["_id"]).getStatus());

    // Verify the document at that _id is different
    ASSERT_BSONOBJ_NE(doc, storage.findById(_opCtx.get(), _nss, doc["_id"]).getValue());

    // Create and apply a insert oplog entry WITHOUT recordId in applyOps mode.
    auto op = makeInsertOplogEntry(nextOpTime(), _nss, _uuid, doc);
    ASSERT_OK(runOpApplyOpsCmd(op));

    // Verify the operation resulted in upsert.
    ASSERT_BSONOBJ_EQ(doc, storage.findById(_opCtx.get(), _nss, doc["_id"]).getValue());
}

TEST_F(ApplyOpsInsertEnabledSteadyStateConstraintsTest,
       InsertByRecordId_NonExistentDocumentAtRecordId_NewId_Inserts) {
    // Insert multiple documents.
    const RecordId rid1(1);
    const RecordId rid2(2);
    const RecordId rid3(3);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 2 << "x" << 200), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 3 << "x" << 300), rid3);

    // Insert a document at a non-existent recordId but with an _id that does exists.
    const RecordId rid(4);
    const BSONObj doc = BSON("_id" << 4 << "x" << 100);

    // Verify no document exists at recordId.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Verify the _id does not exists.
    StorageInterfaceImpl storage;
    ASSERT_NOT_OK(storage.findById(_opCtx.get(), _nss, doc["_id"]).getStatus());

    // Create and apply a insert oplog entry WITH recordId in applyOps mode.
    auto op = makeInsertOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, doc, rid);
    ASSERT_OK(runOpApplyOpsCmd(op));

    // Verify the document was inserted.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    ASSERT_BSONOBJ_EQ(doc, *documentAtRecordId(_opCtx.get(), _nss, rid));
}

TEST_F(ApplyOpsInsertEnabledSteadyStateConstraintsTest,
       InsertByRecordId_ExistsDocumentAtRecordId_MatchingId_Upserts) {
    // Insert multiple documents.
    const RecordId rid1(1);
    const RecordId rid2(2);
    const RecordId rid3(3);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 2 << "x" << 200), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 3 << "x" << 300), rid3);

    // Insert a document at a recordId that exists and same _id value.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 200 << "y" << 100);

    // Verify a document exists at the recordId.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    //
    // Verify the _id is the same
    ASSERT_BSONELT_EQ(doc["_id"], (*documentAtRecordId(_opCtx.get(), _nss, rid))["_id"]);

    // Create and apply a insert oplog entry WITH recordId in applyOps mode.
    auto op = makeInsertOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, doc, rid);
    ASSERT_OK(runOpApplyOpsCmd(op));

    // Verify the operation resulted in upsert.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    ASSERT_BSONOBJ_EQ(doc, *documentAtRecordId(_opCtx.get(), _nss, rid));
}

DEATH_TEST_F(ApplyOpsInsertEnabledSteadyStateConstraintsDeathTest,
             InsertByRecordId_ExistsDocumentAtRecordId_NonMatchingId_Fails,
             "8830901") {
    // Insert multiple documents.
    const RecordId rid1(1);
    const RecordId rid2(2);
    const RecordId rid3(3);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 2 << "x" << 200), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 3 << "x" << 300), rid3);

    // Insert a document at a recordId that exists but with different _id value.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 4 << "x" << 200 << "y" << 100);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Verify the _id are different
    ASSERT_BSONELT_NE(doc["_id"], (*documentAtRecordId(_opCtx.get(), _nss, rid))["_id"]);

    // Create and apply a insert oplog entry WITH recordId in applyOps mode.
    auto op = makeInsertOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, doc, rid);
    std::ignore = runOpApplyOpsCmd(op);
}

DEATH_TEST_F(ApplyOpsInsertDisabledSteadyStateConstraintsDeathTest,
             InsertByRecordId_ExistsDocumentAtRecordId_NonMatchingId_Fails,
             "8830901") {
    // Insert multiple documents.
    const RecordId rid1(1);
    const RecordId rid2(2);
    const RecordId rid3(3);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 2 << "x" << 200), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 3 << "x" << 300), rid3);

    // Insert a document at a recordId that exists but with different _id value.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 4 << "x" << 200 << "y" << 100);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Verify the _id are different
    ASSERT_BSONELT_NE(doc["_id"], (*documentAtRecordId(_opCtx.get(), _nss, rid))["_id"]);

    // Create and apply a insert oplog entry WITH recordId in applyOps mode.
    auto op = makeInsertOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, doc, rid);
    std::ignore = runOpApplyOpsCmd(op);
}

}  // namespace
}  // namespace repl
}  // namespace mongo

