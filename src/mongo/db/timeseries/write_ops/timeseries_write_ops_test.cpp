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

#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

class TimeseriesWriteOpsTest : public CatalogTestFixture {
public:
    explicit TimeseriesWriteOpsTest(Options options = {})
        : CatalogTestFixture(options.useReplSettings(true)),
          _replicateVectoredInsertsTransactionally(
              "featureFlagReplicateVectoredInsertsTransactionally", true) {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
    }

    RAIIServerParameterControllerForTest _replicateVectoredInsertsTransactionally;
};


TEST_F(TimeseriesWriteOpsTest, PerformAtomicTimeseriesWritesWithTransform) {
    NamespaceString ns =
        NamespaceString::createNamespaceString_forTest("db_timeseries_write_ops_test", "ts");
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

        ASSERT_OK(timeseries::write_ops::details::performAtomicTimeseriesWrites(opCtx, {}, {op}));
    }

    // Check the document is actually decompressed on disk.
    {
        auto recordId = record_id_helpers::keyForOID(bucketId);
        auto retrievedBucket = bucketsColl->docFor(opCtx, recordId);

        UnorderedFieldsBSONObjComparator comparator;
        ASSERT_EQ(0, comparator.compare(retrievedBucket.value(), bucketDoc));
    }
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
    auto opCtx = operationContext();
    auto nss = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_test", "system.buckets.batch_insert_missing");
    auto insertStatements =
        std::vector<InsertStatement>{InsertStatement{fromjson("{_id: 0, foo: 1}")}};
    auto fixer = write_ops_exec::LastOpFixer(opCtx);
    write_ops_exec::WriteResult result;
    auto shouldInsertMore =
        write_ops_exec::insertBatchAndHandleErrors(opCtx,
                                                   nss,
                                                   boost::none,
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
        write_ops_exec::insertBatchAndHandleErrors(opCtx,
                                                   nss,
                                                   boost::none,
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
    auto opCtx = operationContext();
    auto nss = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_test", "system.buckets.perform_inserts_no_collection");
    write_ops::InsertCommandRequest request(nss);
    request.setDocuments({fromjson("{_id: 0, foo: 1}")});
    auto source = OperationSource::kTimeseriesInsert;
    auto writeResult = write_ops_exec::performInserts(opCtx, request, source);
    ASSERT_FALSE(writeResult.canContinue);
    ASSERT_EQ(1, writeResult.results.size());
    ASSERT_EQ(ErrorCodes::NamespaceNotFound, writeResult.results[0].getStatus());
}

TEST_F(TimeseriesWriteOpsTest, PerformTimeseriesUpdatesNoCollection) {
    auto opCtx = operationContext();
    auto nss = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_test", "system.buckets.perform_timeseries_updates_no_collection");
    write_ops::UpdateCommandRequest request(nss);
    request.setUpdates(
        {write_ops::UpdateOpEntry(BSON("_id" << 0), write_ops::UpdateModification())});
    auto source = OperationSource::kTimeseriesUpdate;
    auto writeResult = write_ops_exec::performUpdates(opCtx, request, source);
    ASSERT_FALSE(writeResult.canContinue);
    ASSERT_EQ(1, writeResult.results.size());
    ASSERT_EQ(ErrorCodes::NamespaceNotFound, writeResult.results[0].getStatus());
}

TEST_F(TimeseriesWriteOpsTest, PerformTimeseriesDeletesNoCollection) {
    auto opCtx = operationContext();
    auto nss = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_test", "system.buckets.perform_timeseries_deletes_no_collection");
    write_ops::DeleteCommandRequest request(nss);
    request.setDeletes(
        {write_ops::DeleteOpEntry(BSON("_id" << 0), false /* multi */, boost::none)});
    auto source = OperationSource::kTimeseriesDelete;
    auto writeResult = write_ops_exec::performDeletes(opCtx, request, source);
    ASSERT_FALSE(writeResult.canContinue);
    ASSERT_EQ(1, writeResult.results.size());
    ASSERT_EQ(8555700, writeResult.results[0].getStatus().code());
}

TEST_F(TimeseriesWriteOpsTest, PerformTimeseriesWritesNoCollection) {
    auto opCtx = operationContext();
    auto nss = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_test", "system.buckets.perform_timeseries_writes_no_collection");
    write_ops::InsertCommandRequest request(nss);
    ASSERT_THROWS_CODE(
        timeseries::write_ops::performTimeseriesWrites(opCtx, request), DBException, 8555700);

    write_ops::InsertCommandRequest requestUnordered(nss);
    requestUnordered.setOrdered(false);

    ASSERT_THROWS_CODE(
        timeseries::write_ops::performTimeseriesWrites(opCtx, request), DBException, 8555700);
}

TEST_F(TimeseriesWriteOpsTest, CommitTimeseriesBucketNoCollection) {
    auto opCtx = operationContext();
    auto uuid = UUID::gen();

    tracking::Context trackingContext;
    timeseries::bucket_catalog::TrackingContexts trackingContexts;
    timeseries::bucket_catalog::BucketId bucketId{uuid, OID::gen(), 0};
    timeseries::bucket_catalog::BucketKey key{uuid, {trackingContext, {}, boost::none}};
    timeseries::bucket_catalog::ExecutionStatsController stats;

    auto batch = std::make_shared<timeseries::bucket_catalog::WriteBatch>(
        trackingContexts, bucketId, key, 0, stats, "");

    absl::flat_hash_map<int, int> map;
    auto nss =
        NamespaceString::createNamespaceString_forTest("db_timeseries_write_ops_test", "dne");

    write_ops::InsertCommandRequest insertCmdReq(nss.makeTimeseriesBucketsNamespace());
    ASSERT(timeseries::bucket_catalog::claimWriteBatchCommitRights(*batch));

    ASSERT_THROWS_CODE(
        timeseries::write_ops::details::commitTimeseriesBucket(
            opCtx, batch, 0, 0, {}, {}, nullptr, nullptr, nullptr, map, insertCmdReq),
        DBException,
        8555700);
}

}  // namespace
}  // namespace mongo
