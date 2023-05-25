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

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(WriteOpsExecTest, TestUpdateSizeEstimationLogic) {
    // Basic test case.
    OID id = OID::createFromString("629e1e680958e279dc29a989"_sd);
    BSONObj updateStmt = fromjson("{$set: {a: 5}}");
    write_ops::UpdateModification mod(std::move(updateStmt),
                                      write_ops::UpdateModification::ClassicTag{},
                                      false /* isReplacement */);
    write_ops::UpdateOpEntry updateOpEntry(BSON("_id" << id), std::move(mod));
    ASSERT(write_ops::verifySizeEstimate(updateOpEntry));

    // Add 'let' constants.
    BSONObj constants = fromjson("{constOne: 'foo'}");
    updateOpEntry.setC(constants);
    ASSERT(write_ops::verifySizeEstimate(updateOpEntry));

    // Add 'upsertSupplied'.
    updateOpEntry.setUpsertSupplied(OptionalBool(false));
    ASSERT(write_ops::verifySizeEstimate(updateOpEntry));

    // Set 'upsertSupplied' to true.
    updateOpEntry.setUpsertSupplied(OptionalBool(true));
    ASSERT(write_ops::verifySizeEstimate(updateOpEntry));

    // Set 'upsertSupplied' to boost::none.
    updateOpEntry.setUpsertSupplied(OptionalBool(boost::none));
    ASSERT(write_ops::verifySizeEstimate(updateOpEntry));

    // Add a collation.
    BSONObj collation = fromjson("{locale: 'simple'}");
    updateOpEntry.setCollation(collation);
    ASSERT(write_ops::verifySizeEstimate(updateOpEntry));

    // Add a hint.
    BSONObj hint = fromjson("{_id: 1}");
    updateOpEntry.setHint(hint);
    ASSERT(write_ops::verifySizeEstimate(updateOpEntry));

    // Add arrayFilters.
    auto arrayFilter = std::vector<BSONObj>{fromjson("{'x.a': {$gt: 85}}")};
    updateOpEntry.setArrayFilters(arrayFilter);
    ASSERT(write_ops::verifySizeEstimate(updateOpEntry));
}

TEST(WriteOpsExecTest, TestDeleteSizeEstimationLogic) {
    // Basic test case.
    OID id = OID::createFromString("629e1e680958e279dc29a989"_sd);
    write_ops::DeleteOpEntry deleteOpEntry(BSON("_id" << id), false /* multi */);
    ASSERT(write_ops::verifySizeEstimate(deleteOpEntry));

    // Add a collation.
    BSONObj collation = fromjson("{locale: 'simple'}");
    deleteOpEntry.setCollation(collation);
    ASSERT(write_ops::verifySizeEstimate(deleteOpEntry));

    // Add a hint.
    BSONObj hint = fromjson("{_id: 1}");
    deleteOpEntry.setHint(hint);
    ASSERT(write_ops::verifySizeEstimate(deleteOpEntry));
}

TEST(WriteOpsExecTest, TestInsertRequestSizeEstimationLogic) {
    NamespaceString ns("db_write_ops_exec_test", "insert_test");
    write_ops::InsertCommandRequest insert(ns);
    BSONObj docToInsert(fromjson("{_id: 1, foo: 1}"));
    insert.setDocuments({docToInsert});
    ASSERT(write_ops::verifySizeEstimate(insert));

    // Configure different fields for 'wcb'.
    write_ops::WriteCommandRequestBase wcb;

    // stmtId
    wcb.setStmtId(2);
    insert.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(insert));

    // stmtIds
    wcb.setStmtIds(std::vector<int32_t>{2, 3});
    insert.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(insert));

    // isTimeseries
    wcb.setIsTimeseriesNamespace(true);
    insert.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(insert));

    // collUUID
    wcb.setCollectionUUID(UUID::gen());
    insert.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(insert));

    // encryptionInfo
    wcb.setEncryptionInformation(
        EncryptionInformation(fromjson("{schema: 'I love encrypting and protecting my data'}")));
    insert.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(insert));
}

TEST(WriteOpsExecTest, TestUpdateRequestSizeEstimationLogic) {
    NamespaceString ns("db_write_ops_exec_test", "update_test");
    write_ops::UpdateCommandRequest update(ns);

    const BSONObj updateStmt = fromjson("{$set: {a: 5}}");
    auto mod = write_ops::UpdateModification::parseFromClassicUpdate(updateStmt);
    write_ops::UpdateOpEntry updateOpEntry(BSON("_id" << 1), std::move(mod));
    update.setUpdates({updateOpEntry});

    ASSERT(write_ops::verifySizeEstimate(update));

    // Configure different fields for 'wcb'.
    write_ops::WriteCommandRequestBase wcb;

    // stmtId
    wcb.setStmtId(2);
    update.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(update));

    // stmtIds
    wcb.setStmtIds(std::vector<int32_t>{2, 3});
    update.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(update));

    // isTimeseries
    wcb.setIsTimeseriesNamespace(true);
    update.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(update));

    // collUUID
    wcb.setCollectionUUID(UUID::gen());
    update.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(update));

    // encryptionInfo
    wcb.setEncryptionInformation(
        EncryptionInformation(fromjson("{schema: 'I love encrypting and protecting my data'}")));
    update.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(update));

    // Configure different fields specific to 'UpdateStatementRequest'.
    LegacyRuntimeConstants legacyRuntimeConstants;
    const auto now = Date_t::now();

    // At a minimum, $$NOW and $$CLUSTER_TIME must be set.
    legacyRuntimeConstants.setLocalNow(now);
    legacyRuntimeConstants.setClusterTime(Timestamp(now));
    update.setLegacyRuntimeConstants(legacyRuntimeConstants);
    ASSERT(write_ops::verifySizeEstimate(update));

    // $$JS_SCOPE
    BSONObj jsScope = fromjson("{constant: 'I love mapReduce and javascript :D'}");
    legacyRuntimeConstants.setJsScope(jsScope);
    update.setLegacyRuntimeConstants(legacyRuntimeConstants);
    ASSERT(write_ops::verifySizeEstimate(update));

    // $$IS_MR
    legacyRuntimeConstants.setIsMapReduce(true);
    update.setLegacyRuntimeConstants(legacyRuntimeConstants);
    ASSERT(write_ops::verifySizeEstimate(update));

    const std::string kLargeString(100 * 1024, 'b');
    BSONObj letParams = BSON("largeStrParam" << kLargeString);
    update.setLet(letParams);
    ASSERT(write_ops::verifySizeEstimate(update));
}

TEST(WriteOpsExecTest, TestDeleteRequestSizeEstimationLogic) {
    NamespaceString ns("db_write_ops_exec_test", "delete_test");
    write_ops::DeleteCommandRequest deleteReq(ns);
    // Basic test case.
    write_ops::DeleteOpEntry deleteOpEntry(BSON("_id" << 1), false /* multi */);
    deleteReq.setDeletes({deleteOpEntry});

    ASSERT(write_ops::verifySizeEstimate(deleteReq));

    // Configure different fields for 'wcb'.
    write_ops::WriteCommandRequestBase wcb;

    // stmtId
    wcb.setStmtId(2);
    deleteReq.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(deleteReq));

    // stmtIds
    wcb.setStmtIds(std::vector<int32_t>{2, 3});
    deleteReq.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(deleteReq));

    // isTimeseries
    wcb.setIsTimeseriesNamespace(true);
    deleteReq.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(deleteReq));

    // collUUID
    wcb.setCollectionUUID(UUID::gen());
    deleteReq.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(deleteReq));

    // encryptionInfo
    wcb.setEncryptionInformation(
        EncryptionInformation(fromjson("{schema: 'I love encrypting and protecting my data'}")));
    deleteReq.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(deleteReq));
}

}  // namespace
}  // namespace mongo
