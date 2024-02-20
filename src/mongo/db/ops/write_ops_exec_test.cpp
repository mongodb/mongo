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

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

class WriteOpsExecTest : public CatalogTestFixture {
protected:
    using CatalogTestFixture::setUp;
};

TEST_F(WriteOpsExecTest, TestUpdateSizeEstimationLogic) {
    // Basic test case.
    OID id = OID::createFromString("629e1e680958e279dc29a989"_sd);
    BSONObj updateStmt = fromjson("{$set: {a: 5}}");
    write_ops::UpdateModification mod(std::move(updateStmt));
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

    // Add a sort.
    BSONObj sort = fromjson("{a: 1}");
    updateOpEntry.setSort(sort);
    ASSERT(write_ops::verifySizeEstimate(updateOpEntry));

    // Add arrayFilters.
    auto arrayFilter = std::vector<BSONObj>{fromjson("{'x.a': {$gt: 85}}")};
    updateOpEntry.setArrayFilters(arrayFilter);
    ASSERT(write_ops::verifySizeEstimate(updateOpEntry));

    // Add a sampleId.
    updateOpEntry.setSampleId(UUID::gen());
    ASSERT(write_ops::verifySizeEstimate(updateOpEntry));

    // Add $_allowShardKeyUpdatesWithoutFullShardKeyInQuery.
    updateOpEntry.setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(OptionalBool(false));
    ASSERT(write_ops::verifySizeEstimate(updateOpEntry));

    // Set '$_allowShardKeyUpdatesWithoutFullShardKeyInQuery' to true.
    updateOpEntry.setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(OptionalBool(true));
    ASSERT(write_ops::verifySizeEstimate(updateOpEntry));

    // Set '$_allowShardKeyUpdatesWithoutFullShardKeyInQuery' to boost::none.
    updateOpEntry.setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(OptionalBool(boost::none));
    ASSERT(write_ops::verifySizeEstimate(updateOpEntry));
}

TEST_F(WriteOpsExecTest, TestDeleteSizeEstimationLogic) {
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

    // Add a sampleId.
    deleteOpEntry.setSampleId(UUID::gen());
    ASSERT(write_ops::verifySizeEstimate(deleteOpEntry));
}

TEST_F(WriteOpsExecTest, TestInsertRequestSizeEstimationLogic) {
    NamespaceString ns =
        NamespaceString::createNamespaceString_forTest("db_write_ops_exec_test", "insert_test");
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

    // originalQuery
    wcb.setOriginalQuery(fromjson("{field: 'value'}"));
    insert.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(insert));

    // originalCollation
    wcb.setOriginalCollation(fromjson("{locale: 'fr'}"));
    insert.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(insert));
}

TEST_F(WriteOpsExecTest, TestUpdateRequestSizeEstimationLogic) {
    NamespaceString ns =
        NamespaceString::createNamespaceString_forTest("db_write_ops_exec_test", "update_test");
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

    // originalQuery
    wcb.setOriginalQuery(fromjson("{field: 'value'}"));
    update.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(update));

    // originalCollation
    wcb.setOriginalCollation(fromjson("{locale: 'fr'}"));
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

    // $$USER_ROLES
    BSONArray arr = BSON_ARRAY(fromjson("{role: 'readWriteAnyDatabase', db: 'admin'}"));
    legacyRuntimeConstants.setUserRoles(arr);
    update.setLegacyRuntimeConstants(legacyRuntimeConstants);
    ASSERT(write_ops::verifySizeEstimate(update));

    const std::string kLargeString(100 * 1024, 'b');
    BSONObj letParams = BSON("largeStrParam" << kLargeString);
    update.setLet(letParams);
    ASSERT(write_ops::verifySizeEstimate(update));
}

TEST_F(WriteOpsExecTest, TestDeleteRequestSizeEstimationLogic) {
    NamespaceString ns =
        NamespaceString::createNamespaceString_forTest("db_write_ops_exec_test", "delete_test");
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

    // originalQuery
    wcb.setOriginalQuery(fromjson("{field: 'value'}"));
    deleteReq.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(deleteReq));

    // originalCollation
    wcb.setOriginalCollation(fromjson("{locale: 'fr'}"));
    deleteReq.setWriteCommandRequestBase(wcb);
    ASSERT(write_ops::verifySizeEstimate(deleteReq));
}

TEST_F(WriteOpsExecTest, PerformAtomicTimeseriesWritesWithTransform) {
    NamespaceString ns =
        NamespaceString::createNamespaceString_forTest("db_write_ops_exec_test", "ts");
    auto opCtx = operationContext();
    ASSERT_OK(createCollection(opCtx,
                               ns.dbName(),
                               BSON("create" << ns.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"))));

    // We're going to insert a compressed bucket and ensure we can successfully decompress it via a
    // transform update using performAtomicTimeseriesWrites.
    const BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
    OID bucketId = OID::createFromString("629e1e680958e279dc29a517"_sd);
    auto compressionResult = timeseries::compressBucket(bucketDoc, "time", ns, false);
    ASSERT_TRUE(compressionResult.compressedBucket.has_value());
    const BSONObj compressedBucket = compressionResult.compressedBucket.value();

    // Insert the compressed bucket.
    AutoGetCollection bucketsColl(opCtx, ns.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            opCtx, *bucketsColl, InsertStatement{compressedBucket}, nullptr));
        wunit.commit();
    }

    // Decompress via transform.
    {
        auto bucketDecompressionFunc = [&](const BSONObj& bucketDoc) -> boost::optional<BSONObj> {
            return timeseries::decompressBucket(bucketDoc);
        };


        write_ops::UpdateModification u(std::move(bucketDecompressionFunc));
        write_ops::UpdateOpEntry update(BSON("_id" << bucketId), std::move(u));
        write_ops::UpdateCommandRequest op(ns.makeTimeseriesBucketsNamespace(), {update});

        write_ops::WriteCommandRequestBase base;
        base.setBypassDocumentValidation(true);
        base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});

        op.setWriteCommandRequestBase(std::move(base));

        ASSERT_OK(write_ops_exec::performAtomicTimeseriesWrites(opCtx, {}, {op}));
    }

    // Check the document is actually decompressed on disk.
    {
        auto recordId = record_id_helpers::keyForOID(bucketId);
        auto retrievedBucket = bucketsColl->docFor(opCtx, recordId);

        UnorderedFieldsBSONObjComparator comparator;
        ASSERT_EQ(0, comparator.compare(retrievedBucket.value(), bucketDoc));
    }
}

}  // namespace
}  // namespace mongo
