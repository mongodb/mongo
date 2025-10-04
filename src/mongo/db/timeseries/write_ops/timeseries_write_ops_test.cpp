/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/timeseries/write_ops/timeseries_write_ops.h"

#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/collection_pre_conditions_util.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"
#include "mongo/db/timeseries/write_ops/internal/timeseries_write_ops_internal.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class TimeseriesWriteOpsTest : public timeseries::TimeseriesTestFixture {};

TEST_F(TimeseriesWriteOpsTest, PerformAtomicTimeseriesWritesWithTransform) {
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
    auto compressionResult = timeseries::compressBucket(bucketDoc, "time", _nsNoMeta, false);
    ASSERT_TRUE(compressionResult.compressedBucket.has_value());
    const BSONObj compressedBucket = compressionResult.compressedBucket.value();

    // Insert the compressed bucket.
    AutoGetCollection bucketsColl(
        _opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{_opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx, *bucketsColl, InsertStatement{compressedBucket}, nullptr));
        wunit.commit();
    }

    // Decompress via transform.
    {
        auto bucketDecompressionFunc = [&](const BSONObj& bucketDoc) -> boost::optional<BSONObj> {
            return timeseries::decompressBucket(bucketDoc);
        };


        write_ops::UpdateModification u(std::move(bucketDecompressionFunc));
        write_ops::UpdateOpEntry update(BSON("_id" << bucketId), std::move(u));
        write_ops::UpdateCommandRequest op(_nsNoMeta.makeTimeseriesBucketsNamespace(), {update});

        write_ops::WriteCommandRequestBase base;
        base.setBypassDocumentValidation(true);
        base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});

        op.setWriteCommandRequestBase(std::move(base));
        op.setCollectionUUID(bucketsColl->uuid());

        ASSERT_OK(timeseries::write_ops::internal::performAtomicTimeseriesWrites(_opCtx, {}, {op}));
    }

    // Check the document is actually decompressed on disk.
    {
        auto recordId = record_id_helpers::keyForOID(bucketId);
        auto retrievedBucket = bucketsColl->docFor(_opCtx, recordId);

        UnorderedFieldsBSONObjComparator comparator;
        ASSERT_EQ(0, comparator.compare(retrievedBucket.value(), bucketDoc));
    }
}

TEST_F(TimeseriesWriteOpsTest, TimeseriesWritesMismatchedUUID) {
    // Ordered
    auto insertCommandReq =
        write_ops::InsertCommandRequest(_nsNoMeta.makeTimeseriesBucketsNamespace());
    insertCommandReq.setCollectionUUID(UUID::gen());
    ASSERT_THROWS_CODE(
        timeseries::write_ops::internal::performAtomicTimeseriesWrites(
            _opCtx, std::vector<write_ops::InsertCommandRequest>{insertCommandReq}, {}),
        DBException,
        9748800);

    // Unordered
    auto insertStatements = std::vector<InsertStatement>{InsertStatement{fromjson("{_id: 0}")}};
    auto fixer = write_ops_exec::LastOpFixer(_opCtx);
    write_ops_exec::WriteResult result;
    auto preConditions = timeseries::CollectionPreConditions::getCollectionPreConditions(
        _opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace(), UUID::gen());
    ASSERT_THROWS_CODE(
        write_ops_exec::insertBatchAndHandleErrors(_opCtx,
                                                   _nsNoMeta,
                                                   preConditions,
                                                   false,
                                                   insertStatements,
                                                   OperationSource::kTimeseriesInsert,
                                                   &fixer,
                                                   &result),
        DBException,
        9748801);
}

// It is possible that a collection is dropped after an insert starts but before it finishes. In
// those cases, since time-series inserts do not implicitly create the collection, the insert should
// fail. BatchInsertMissingCollection, PerformTimeseriesWritesNoCollection, and
// PerformTimeseriesUpdatesNoCollection test paths where the collection can be removed out from
// under an insert in progress.

// TODO SERVER-95114: insertBatchAndHandleErrors(), performTimeseriesWrites(), and performUpdates()
// (as well other functions in write_ops_exec.h) should be unit tested in a broader set of
// scenarios. This is ticketed out to clarify that these tests are not exhaustive.

TEST_F(TimeseriesWriteOpsTest, BatchInsertMissingCollection) {
    auto nss = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_test", "system.buckets.batch_insert_missing");
    auto insertStatements =
        std::vector<InsertStatement>{InsertStatement{fromjson("{_id: 0, foo: 1}")}};
    auto fixer = write_ops_exec::LastOpFixer(_opCtx);
    write_ops_exec::WriteResult result;
    auto preConditions = timeseries::CollectionPreConditions::getCollectionPreConditions(
        _opCtx, nss, /*expectedUUID=*/boost::none);
    auto shouldInsertMore =
        write_ops_exec::insertBatchAndHandleErrors(_opCtx,
                                                   nss,
                                                   preConditions,
                                                   true,
                                                   insertStatements,
                                                   OperationSource::kTimeseriesInsert,
                                                   &fixer,
                                                   &result);
    ASSERT_FALSE(shouldInsertMore);
    ASSERT_EQ(1, result.results.size());
    ASSERT_EQ(ErrorCodes::NamespaceNotFound, result.results[0].getStatus());

    result.results.clear();
    insertStatements.push_back(InsertStatement{fromjson("{_id: 1, foo: 2}")});
    insertStatements.push_back(InsertStatement{fromjson("{_id: 2, foo: 3}")});

    shouldInsertMore =
        write_ops_exec::insertBatchAndHandleErrors(_opCtx,
                                                   nss,
                                                   preConditions,
                                                   true,
                                                   insertStatements,
                                                   OperationSource::kTimeseriesInsert,
                                                   &fixer,
                                                   &result);
    ASSERT_FALSE(shouldInsertMore);
    ASSERT_EQ(1, result.results.size());
    ASSERT_EQ(ErrorCodes::NamespaceNotFound, result.results[0].getStatus());
}

TEST_F(TimeseriesWriteOpsTest, PerformInsertsNoCollection) {
    auto nss = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_test", "system.buckets.perform_inserts_no_collection");
    write_ops::InsertCommandRequest request(nss);
    request.setDocuments({fromjson("{_id: 0, foo: 1}")});
    auto source = OperationSource::kTimeseriesInsert;
    auto writeResult =
        write_ops_exec::performInserts(_opCtx, request, /*preConditions=*/boost::none, source);
    ASSERT_FALSE(writeResult.canContinue);
    ASSERT_EQ(1, writeResult.results.size());
    ASSERT_EQ(ErrorCodes::NamespaceNotFound, writeResult.results[0].getStatus());
}

TEST_F(TimeseriesWriteOpsTest, PerformTimeseriesDeletesNoCollection) {
    auto nss = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_test", "system.buckets.perform_timeseries_deletes_no_collection");
    write_ops::DeleteCommandRequest request(nss);
    request.setDeletes(
        {write_ops::DeleteOpEntry(BSON("_id" << 0), false /* multi */, boost::none)});
    auto source = OperationSource::kTimeseriesDelete;
    auto writeResult =
        write_ops_exec::performDeletes(_opCtx, request, /*preConditions=*/boost::none, source);
    ASSERT_FALSE(writeResult.canContinue);
    ASSERT_EQ(1, writeResult.results.size());
    ASSERT_EQ(8555700, writeResult.results[0].getStatus().code());
}

TEST_F(TimeseriesWriteOpsTest, PerformTimeseriesWritesNoCollection) {
    auto nss = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_test", "system.buckets.perform_timeseries_writes_no_collection");
    write_ops::InsertCommandRequest request(nss);
    auto preConditions = timeseries::CollectionPreConditions::getCollectionPreConditions(
        _opCtx, nss, /*expectedUUID=*/boost::none);
    ASSERT_THROWS_CODE(
        timeseries::write_ops::performTimeseriesWrites(_opCtx, request, preConditions),
        DBException,
        8555700);

    write_ops::InsertCommandRequest requestUnordered(nss);
    requestUnordered.setOrdered(false);

    ASSERT_THROWS_CODE(
        timeseries::write_ops::performTimeseriesWrites(_opCtx, request, preConditions),
        DBException,
        8555700);
}

}  // namespace
}  // namespace mongo
