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

#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class WriteOpsExecTest : public CatalogTestFixture {
protected:
    using CatalogTestFixture::setUp;
};

TEST_F(WriteOpsExecTest, PerformAtomicTimeseriesWritesWithTransform) {
    NamespaceString ns{"db_write_ops_exec_test", "ts"};
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
    auto compressionResult = timeseries::compressBucket(bucketDoc, "time", ns, true, false);
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
