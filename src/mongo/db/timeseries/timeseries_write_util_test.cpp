/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/timeseries/timeseries_write_util.h"

namespace mongo::timeseries {
namespace {

class TimeseriesWriteUtilTest : public CatalogTestFixture {
protected:
    using CatalogTestFixture::setUp;
};


TEST_F(TimeseriesWriteUtilTest, MakeNewBucketFromWriteBatch) {
    NamespaceString ns{"db_timeseries_write_util_test", "MakeNewBucketFromWriteBatch"};

    // Builds a write batch.
    OID oid = OID::createFromString("629e1e680958e279dc29a517"_sd);
    bucket_catalog::BucketId bucketId(ns, oid);
    std::uint8_t stripe = 0;
    auto opId = 0;
    bucket_catalog::ExecutionStats globalStats;
    auto collectionStats = std::make_shared<bucket_catalog::ExecutionStats>();
    bucket_catalog::ExecutionStatsController stats(collectionStats, globalStats);
    auto batch = std::make_shared<bucket_catalog::WriteBatch>(
        bucket_catalog::BucketHandle{bucketId, stripe}, opId, stats);
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":1,"b":1})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":2,"b":2})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3})")};
    batch->measurements = {measurements.begin(), measurements.end()};
    batch->min = fromjson(R"({"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1})");
    batch->max = fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3})");

    // Makes the new document for write.
    auto newDoc = timeseries::makeNewDocumentForWrite(batch, /*metadata=*/{});

    // Checks the measurements are stored in the bucket format.
    const BSONObj bucketDoc = fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    UnorderedFieldsBSONObjComparator comparator;
    ASSERT_EQ(0, comparator.compare(newDoc, bucketDoc));
}

TEST_F(TimeseriesWriteUtilTest, MakeNewBucketFromWriteBatchWithMeta) {
    NamespaceString ns{"db_timeseries_write_util_test", "MakeNewBucketFromWriteBatchWithMeta"};

    // Builds a write batch.
    OID oid = OID::createFromString("629e1e680958e279dc29a517"_sd);
    bucket_catalog::BucketId bucketId(ns, oid);
    std::uint8_t stripe = 0;
    auto opId = 0;
    bucket_catalog::ExecutionStats globalStats;
    auto collectionStats = std::make_shared<bucket_catalog::ExecutionStats>();
    bucket_catalog::ExecutionStatsController stats(collectionStats, globalStats);
    auto batch = std::make_shared<bucket_catalog::WriteBatch>(
        bucket_catalog::BucketHandle{bucketId, stripe}, opId, stats);
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"meta":{"tag":1},"a":1,"b":1})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"meta":{"tag":1},"a":2,"b":2})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"meta":{"tag":1},"a":3,"b":3})")};
    batch->measurements = {measurements.begin(), measurements.end()};
    batch->min = fromjson(R"({"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1})");
    batch->max = fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3})");
    auto metadata = fromjson(R"({"meta":{"tag":1}})");

    // Makes the new document for write.
    auto newDoc = timeseries::makeNewDocumentForWrite(batch, metadata);

    // Checks the measurements are stored in the bucket format.
    const BSONObj bucketDoc = fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "meta":{"tag":1},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    UnorderedFieldsBSONObjComparator comparator;
    ASSERT_EQ(0, comparator.compare(newDoc, bucketDoc));
}

TEST_F(TimeseriesWriteUtilTest, MakeNewBucketFromMeasurements) {
    OID oid = OID::createFromString("629e1e680958e279dc29a517"_sd);
    TimeseriesOptions options("time");
    options.setGranularity(BucketGranularityEnum::Seconds);
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":1,"b":1})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":2,"b":2})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:33:30.000Z"},"a":3,"b":3})")};

    // Makes the new document for write.
    auto newDoc = timeseries::makeNewDocumentForWrite(
        oid, measurements, /*metadata=*/{}, options, /*comparator=*/nullptr);

    // Checks the measurements are stored in the bucket format.
    const BSONObj bucketDoc = fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:33:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:33:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    UnorderedFieldsBSONObjComparator comparator;
    ASSERT_EQ(0, comparator.compare(newDoc, bucketDoc));
}

TEST_F(TimeseriesWriteUtilTest, MakeNewBucketFromMeasurementsWithMeta) {
    OID oid = OID::createFromString("629e1e680958e279dc29a517"_sd);
    TimeseriesOptions options("time");
    options.setGranularity(BucketGranularityEnum::Seconds);
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"meta":{"tag":1},"a":1,"b":1})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"meta":{"tag":1},"a":2,"b":2})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:33:30.000Z"},"meta":{"tag":1},"a":3,"b":3})")};
    auto metadata = fromjson(R"({"meta":{"tag":1}})");

    // Makes the new document for write.
    auto newDoc = timeseries::makeNewDocumentForWrite(
        oid, measurements, metadata, options, /*comparator=*/nullptr);

    // Checks the measurements are stored in the bucket format.
    const BSONObj bucketDoc = fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:33:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "meta":{"tag":1},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:33:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    UnorderedFieldsBSONObjComparator comparator;
    ASSERT_EQ(0, comparator.compare(newDoc, bucketDoc));
}

TEST_F(TimeseriesWriteUtilTest, PerformAtomicDelete) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "PerformAtomicDelete");
    auto opCtx = operationContext();
    ASSERT_OK(createCollection(opCtx,
                               ns.dbName(),
                               BSON("create" << ns.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"))));

    // Inserts a bucket document.
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
    auto recordId = record_id_helpers::keyForOID(bucketId);

    AutoGetCollection bucketsColl(opCtx, ns.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            opCtx, *bucketsColl, InsertStatement{bucketDoc}, nullptr));
        wunit.commit();
    }

    // Deletes the bucket document.
    {
        write_ops::DeleteOpEntry deleteEntry(BSON("_id" << bucketId), false);
        write_ops::DeleteCommandRequest op(ns.makeTimeseriesBucketsNamespace(), {deleteEntry});

        write_ops::WriteCommandRequestBase base;
        base.setBypassDocumentValidation(true);
        base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});

        op.setWriteCommandRequestBase(std::move(base));

        ASSERT_OK(performAtomicWrites(opCtx,
                                      bucketsColl.getCollection(),
                                      recordId,
                                      op,
                                      /*fromMigrate=*/false,
                                      /*stmtId=*/kUninitializedStmtId));
    }

    // Checks the document is removed.
    {
        Snapshotted<BSONObj> doc;
        bool found = bucketsColl->findDoc(opCtx, recordId, &doc);
        ASSERT_FALSE(found);
    }
}

TEST_F(TimeseriesWriteUtilTest, PerformAtomicUpdate) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "PerformAtomicUpdate");
    auto opCtx = operationContext();
    ASSERT_OK(createCollection(opCtx,
                               ns.dbName(),
                               BSON("create" << ns.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"))));

    // Inserts a bucket document.
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
    auto recordId = record_id_helpers::keyForOID(bucketId);

    AutoGetCollection bucketsColl(opCtx, ns.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            opCtx, *bucketsColl, InsertStatement{bucketDoc}, nullptr));
        wunit.commit();
    }

    // Replaces the bucket document.
    const BSONObj replaceDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":3,"b":3},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":3},
                    "b":{"0":3}}})");

    {
        write_ops::UpdateModification u(replaceDoc);
        write_ops::UpdateOpEntry update(BSON("_id" << bucketId), std::move(u));
        write_ops::UpdateCommandRequest op(ns.makeTimeseriesBucketsNamespace(), {update});

        write_ops::WriteCommandRequestBase base;
        base.setBypassDocumentValidation(true);
        base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});

        op.setWriteCommandRequestBase(std::move(base));

        ASSERT_OK(performAtomicWrites(opCtx,
                                      bucketsColl.getCollection(),
                                      recordId,
                                      op,
                                      /*fromMigrate=*/false,
                                      /*stmtId=*/kUninitializedStmtId));
    }

    // Checks the document is updated.
    {
        Snapshotted<BSONObj> doc;
        bool found = bucketsColl->findDoc(opCtx, recordId, &doc);

        ASSERT_TRUE(found);
        UnorderedFieldsBSONObjComparator comparator;
        ASSERT_EQ(0, comparator.compare(doc.value(), replaceDoc));
    }
}

}  // namespace
}  // namespace mongo::timeseries
