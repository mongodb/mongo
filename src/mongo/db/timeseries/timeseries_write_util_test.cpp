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

#include <boost/cstdint.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/tracking_contexts.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/timeseries/write_ops/measurement.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils_internal.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <string>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/posix_time/time_parsers.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::timeseries {
namespace {

class TimeseriesWriteUtilTest : public TimeseriesTestFixture {
protected:
    std::shared_ptr<bucket_catalog::WriteBatch> generateBatch(
        const UUID& uuid, bucket_catalog::BucketMetadata bucketMetadata) {
        OID oid = OID::createFromString("629e1e680958e279dc29a517"_sd);
        std::uint8_t stripe = 0;
        bucket_catalog::BucketId bucketId(uuid, oid, stripe);
        auto opId = 0;
        auto collectionStats = std::make_shared<bucket_catalog::ExecutionStats>();
        bucket_catalog::ExecutionStatsController stats(collectionStats, _globalStats);
        return std::make_shared<bucket_catalog::WriteBatch>(
            _trackingContexts,
            bucketId,
            bucket_catalog::BucketKey{uuid, std::move(bucketMetadata)},
            opId,
            stats,
            _timeField);
    }

    std::shared_ptr<bucket_catalog::WriteBatch> generateBatch(const UUID& uuid) {
        return generateBatch(
            uuid,
            {bucket_catalog::getTrackingContext(_trackingContexts,
                                                bucket_catalog::TrackingScope::kOpenBucketsByKey),
             {},
             boost::none});
    }

protected:
    bucket_catalog::TrackingContexts _trackingContexts;

private:
    bucket_catalog::ExecutionStats _globalStats;
};

TEST_F(TimeseriesWriteUtilTest, MakeNewBucketFromWriteBatch) {
    // Builds a write batch.
    auto batch = generateBatch(UUID::gen());
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":1,"b":1})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":2,"b":2})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3})")};
    batch->measurements = {measurements.begin(), measurements.end()};
    batch->min = fromjson(R"({"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1})");
    batch->max = fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3})");

    // Makes the new document for write.
    auto newDoc =
        timeseries::write_ops_utils::makeNewDocumentForWrite(_nsNoMeta, batch, /*metadata=*/{})
            .uncompressedBucket;

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
    // Builds a write batch.
    auto batch = generateBatch(UUID::gen());
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"meta":{"tag":1},"a":1,"b":1})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"meta":{"tag":1},"a":2,"b":2})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"meta":{"tag":1},"a":3,"b":3})")};
    batch->measurements = {measurements.begin(), measurements.end()};
    batch->min = fromjson(R"({"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1})");
    batch->max = fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3})");
    auto metadata = fromjson(R"({"meta":{"tag":1}})");

    // Makes the new document for write.
    auto newDoc = timeseries::write_ops_utils::makeNewDocumentForWrite(_ns1, batch, metadata)
                      .uncompressedBucket;

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

TEST_F(TimeseriesWriteUtilTest, MakeNewCompressedBucketFromWriteBatch) {
    // Builds a write batch with out-of-order time to verify that bucket compression sorts by time.
    auto batch = generateBatch(UUID::gen());
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":1,"b":1})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:50.000Z"},"a":3,"b":3})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},"a":2,"b":2})")};
    batch->measurements = {measurements.begin(), measurements.end()};
    batch->min = fromjson(R"({"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1})");
    batch->max = fromjson(R"({"time":{"$date":"2022-06-06T15:34:50.000Z"},"a":3,"b":3})");

    // Makes the new compressed document for write.
    auto bucketDoc =
        timeseries::write_ops_utils::makeNewDocumentForWrite(_nsNoMeta, batch, /*metadata=*/{});

    // makeNewDocumentForWrite() can return the uncompressed bucket if an error was encountered
    // during compression. Check that compression was successful.
    ASSERT(!bucketDoc.compressionFailed);
    ASSERT_EQ(timeseries::kTimeseriesControlCompressedSortedVersion,
              bucketDoc.compressedBucket->getObjectField(timeseries::kBucketControlFieldName)
                  .getIntField(timeseries::kBucketControlVersionFieldName));

    auto decompressedDoc = decompressBucket(*bucketDoc.compressedBucket);
    ASSERT(decompressedDoc);

    // Checks the measurements are stored in the bucket format.
    const BSONObj expectedDoc = fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:50.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:40.000Z"},
                            "2":{"$date":"2022-06-06T15:34:50.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    UnorderedFieldsBSONObjComparator comparator;
    ASSERT_EQ(0, comparator.compare(*decompressedDoc, expectedDoc));
}

TEST_F(TimeseriesWriteUtilTest, MakeNewCompressedBucketFromWriteBatchWithMeta) {
    // Builds a write batch with out-of-order time to verify that bucket compression sorts by time.
    auto batch = generateBatch(UUID::gen());
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"meta":{"tag":1},"a":1,"b":1})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:50.000Z"},"meta":{"tag":1},"a":3,"b":3})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},"meta":{"tag":1},"a":2,"b":2})")};
    batch->measurements = {measurements.begin(), measurements.end()};
    batch->min = fromjson(R"({"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1})");
    batch->max = fromjson(R"({"time":{"$date":"2022-06-06T15:34:50.000Z"},"a":3,"b":3})");
    auto metadata = fromjson(R"({"meta":{"tag":1}})");

    // Makes the new compressed document for write.
    auto bucketDoc = timeseries::write_ops_utils::makeNewDocumentForWrite(_ns1, batch, metadata);

    // makeNewDocumentForWrite() can return the uncompressed bucket if an error was encountered
    // during compression. Check that compression was successful.
    ASSERT(!bucketDoc.compressionFailed);
    ASSERT_EQ(timeseries::kTimeseriesControlCompressedSortedVersion,
              bucketDoc.compressedBucket->getObjectField(timeseries::kBucketControlFieldName)
                  .getIntField(timeseries::kBucketControlVersionFieldName));

    auto decompressedDoc = decompressBucket(*bucketDoc.compressedBucket);
    ASSERT(decompressedDoc);

    // Checks the measurements are stored in the bucket format.
    const BSONObj expectedDoc = fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:50.000Z"},"a":3,"b":3}},
            "meta":{"tag":1},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:40.000Z"},
                            "2":{"$date":"2022-06-06T15:34:50.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    UnorderedFieldsBSONObjComparator comparator;
    ASSERT_EQ(0, comparator.compare(*decompressedDoc, expectedDoc));
}

TEST_F(TimeseriesWriteUtilTest, MakeNewBucketFromMeasurements) {
    UUID uuid = UUID::gen();
    OID oid = OID::createFromString("629e1e680958e279dc29a517"_sd);
    TimeseriesOptions options("time");
    options.setGranularity(BucketGranularityEnum::Seconds);
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":1,"b":1})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":2,"b":2})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:33:30.000Z"},"a":3,"b":3})")};

    // Makes the new document for write.
    auto newDoc = timeseries::write_ops_utils::makeNewDocumentForWrite(_nsNoMeta,
                                                                       uuid,
                                                                       oid,
                                                                       measurements,
                                                                       /*metadata=*/{},
                                                                       options,
                                                                       /*comparator=*/nullptr,
                                                                       boost::none)
                      .uncompressedBucket;

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
    UUID uuid = UUID::gen();
    OID oid = OID::createFromString("629e1e680958e279dc29a517"_sd);
    TimeseriesOptions options("time");
    options.setGranularity(BucketGranularityEnum::Seconds);
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"meta":{"tag":1},"a":1,"b":1})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"meta":{"tag":1},"a":2,"b":2})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:33:30.000Z"},"meta":{"tag":1},"a":3,"b":3})")};
    auto metadata = fromjson(R"({"meta":{"tag":1}})");

    // Makes the new document for write.
    auto newDoc =
        timeseries::write_ops_utils::makeNewDocumentForWrite(
            _ns1, uuid, oid, measurements, metadata, options, /*comparator=*/nullptr, boost::none)
            .uncompressedBucket;

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

/**
 * Test that makeTimeseriesCompressedDiffUpdateOpFromBatch returns the expected diff object when
 * inserting measurements into a compressed bucket, and that out-of-order
 * measurements cause a bucket to upgraded to a v3 bucket.
 */
TEST_F(TimeseriesWriteUtilTest, MakeTimeseriesCompressedDiffUpdateOpFromBatch) {
    // Builds a write batch for an update and sets the decompressed field of the batch.
    auto batch = generateBatch(UUID::gen());
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":0,"b":0})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:34.000Z"},"a":4,"b":4})"),
    };

    batch->min = fromjson(R"({"u": {"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":0,"b":0}})");
    batch->max = fromjson(R"({"u": {"time":{"$date":"2022-06-06T15:34:34.000Z"},"a":4,"b":4}})");
    batch->measurements = {measurements.begin(), measurements.end()};

    const BSONObj uncompressedPreImage = fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:31.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:33.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:31.000Z"},
                            "1":{"$date":"2022-06-06T15:34:32.000Z"},
                            "2":{"$date":"2022-06-06T15:34:33.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    const auto preImageCompressionResult = timeseries::compressBucket(uncompressedPreImage,
                                                                      _timeField,
                                                                      _nsNoMeta,
                                                                      /*validateCompression=*/true);
    ASSERT_TRUE(preImageCompressionResult.compressedBucket);

    batch->numPreviouslyCommittedMeasurements = 3;
    BSONObj bucketDataDoc =
        preImageCompressionResult.compressedBucket->getObjectField(kBucketDataFieldName).getOwned();
    batch->measurementMap.initBuilders(bucketDataDoc, batch->numPreviouslyCommittedMeasurements);

    const BSONObj expectedDiff = fromjson(
        R"({
        "scontrol":{"u":{"count":5,"version":3},
                    "smin":{"u":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":0,"b":0}},
                    "smax":{"u":{"time":{"$date":"2022-06-06T15:34:34.000Z"},"a":4,"b":4}}},
        "sdata":{
            "b":{"time":{"o":10,"d":{"$binary":"gAt9AAD8fGBtAA==","$type":"00"}},
                 "a":{"o":6,"d":{"$binary":"gCsAEAAUABAAAA==","$type":"00"}},
                 "b":{"o":6,"d":{"$binary":"gCsAEAAUABAAAA==","$type":"00"}}}}
        })");

    auto request = write_ops_utils::makeTimeseriesCompressedDiffUpdateOpFromBatch(
        _opCtx, batch, _nsNoMeta.makeTimeseriesBucketsNamespace());
    auto& updates = request.getUpdates();

    ASSERT_EQ(updates.size(), 1);
    // The update command request should return the document diff of the batch applied on the pre
    // image.
    UnorderedFieldsBSONObjComparator comparator;
    ASSERT_EQ(0, comparator.compare(updates[0].getU().getDiff(), expectedDiff));
}

/**
 * Test that makeTimeseriesCompressedDiffUpdateOpFromBatch returns the expected diff object when
 * inserting measurements with meta fields into a compressed bucket, and that out-of-order
 * measurements cause a bucket to upgraded to a v3 bucket.
 */
TEST_F(TimeseriesWriteUtilTest, MakeTimeseriesCompressedDiffUpdateOpFromBatchWithMeta) {
    const BSONObj uncompressedPreImage = fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:31.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:33.000Z"},"a":3,"b":3}},
            "meta":{"tag":1},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:31.000Z"},
                            "1":{"$date":"2022-06-06T15:34:32.000Z"},
                            "2":{"$date":"2022-06-06T15:34:33.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    // Builds a write batch for an update and sets the decompressed field of the batch.
    auto batch =
        generateBatch(UUID::gen(),
                      {bucket_catalog::getTrackingContext(
                           _trackingContexts, bucket_catalog::TrackingScope::kOpenBucketsByKey),
                       uncompressedPreImage.getField("meta"),
                       boost::none});
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"meta":{"tag":1},"a":0,"b":0})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:34.000Z"},"meta":{"tag":1},"a":4,"b":4})"),
    };

    batch->min = fromjson(R"({"u": {"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":0,"b":0}})");
    batch->max = fromjson(R"({"u": {"time":{"$date":"2022-06-06T15:34:34.000Z"},"a":4,"b":4}})");
    batch->measurements = {measurements.begin(), measurements.end()};
    auto metadata = fromjson(R"({"meta":{"tag":1}})");

    const auto preImageCompressionResult = timeseries::compressBucket(uncompressedPreImage,
                                                                      _timeField,
                                                                      _nsNoMeta,
                                                                      /*validateCompression=*/true);
    ASSERT_TRUE(preImageCompressionResult.compressedBucket);

    batch->numPreviouslyCommittedMeasurements = 3;
    BSONObj bucketDataDoc =
        preImageCompressionResult.compressedBucket->getObjectField(kBucketDataFieldName).getOwned();
    batch->measurementMap.initBuilders(bucketDataDoc, batch->numPreviouslyCommittedMeasurements);

    const BSONObj expectedDiff = fromjson(
        R"({
        "scontrol":{"u":{"count":5, "version":3},
                    "smin":{"u":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":0,"b":0}},
                    "smax":{"u":{"time":{"$date":"2022-06-06T15:34:34.000Z"},"a":4,"b":4}}},
        "sdata":{
            "b":{"time":{"o":10,"d":{"$binary":"gAt9AAD8fGBtAA==","$type":"00"}},
                 "a":{"o":6,"d":{"$binary":"gCsAEAAUABAAAA==","$type":"00"}},
                 "b":{"o":6,"d":{"$binary":"gCsAEAAUABAAAA==","$type":"00"}}}}
        })");

    auto request = write_ops_utils::makeTimeseriesCompressedDiffUpdateOpFromBatch(
        _opCtx, batch, _nsNoMeta.makeTimeseriesBucketsNamespace());
    auto& updates = request.getUpdates();

    ASSERT_EQ(updates.size(), 1);

    // The update command request should return the document diff of the batch applied on the pre
    // image.
    UnorderedFieldsBSONObjComparator comparator;
    ASSERT_EQ(0, comparator.compare(updates[0].getU().getDiff(), expectedDiff));
}

TEST_F(TimeseriesWriteUtilTest, PerformAtomicDelete) {
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

    AutoGetCollection bucketsColl(
        _opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{_opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx, *bucketsColl, InsertStatement{bucketDoc}, nullptr));
        wunit.commit();
    }

    // Deletes the bucket document.
    {
        mongo::write_ops::DeleteOpEntry deleteEntry(BSON("_id" << bucketId), false);
        mongo::write_ops::DeleteCommandRequest op(_nsNoMeta.makeTimeseriesBucketsNamespace(),
                                                  {deleteEntry});

        mongo::write_ops::WriteCommandRequestBase base;
        base.setBypassDocumentValidation(true);
        base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});

        op.setWriteCommandRequestBase(std::move(base));

        ASSERT_DOES_NOT_THROW(
            performAtomicWrites(_opCtx,
                                *bucketsColl,
                                recordId,
                                std::variant<mongo::write_ops::UpdateCommandRequest,
                                             mongo::write_ops::DeleteCommandRequest>{op},
                                {},
                                {},
                                /*fromMigrate=*/false,
                                /*stmtId=*/kUninitializedStmtId));
    }

    // Checks the document is removed.
    {
        Snapshotted<BSONObj> doc;
        bool found = bucketsColl->findDoc(_opCtx, recordId, &doc);
        ASSERT_FALSE(found);
    }
}

TEST_F(TimeseriesWriteUtilTest, PerformAtomicUpdate) {
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

    AutoGetCollection bucketsColl(
        _opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{_opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx, *bucketsColl, InsertStatement{bucketDoc}, nullptr));
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
        mongo::write_ops::UpdateModification u(replaceDoc);
        mongo::write_ops::UpdateOpEntry update(BSON("_id" << bucketId), std::move(u));
        mongo::write_ops::UpdateCommandRequest op(_nsNoMeta.makeTimeseriesBucketsNamespace(),
                                                  {update});

        mongo::write_ops::WriteCommandRequestBase base;
        base.setBypassDocumentValidation(true);
        base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});

        op.setWriteCommandRequestBase(std::move(base));

        ASSERT_DOES_NOT_THROW(
            performAtomicWrites(_opCtx,
                                *bucketsColl,
                                recordId,
                                std::variant<mongo::write_ops::UpdateCommandRequest,
                                             mongo::write_ops::DeleteCommandRequest>{op},
                                {},
                                {},
                                /*fromMigrate=*/false,
                                /*stmtId=*/kUninitializedStmtId));
    }

    // Checks the document is updated.
    {
        Snapshotted<BSONObj> doc;
        bool found = bucketsColl->findDoc(_opCtx, recordId, &doc);

        ASSERT_TRUE(found);
        UnorderedFieldsBSONObjComparator comparator;
        ASSERT_EQ(0, comparator.compare(doc.value(), replaceDoc));
    }
}

TEST_F(TimeseriesWriteUtilTest, PerformAtomicDeleteAndInsert) {
    // Inserts a bucket document.
    const BSONObj bucketDoc1 = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
    OID bucketId1 = bucketDoc1["_id"].OID();
    auto recordId1 = record_id_helpers::keyForOID(bucketId1);

    AutoGetCollection bucketsColl(
        _opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{_opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx, *bucketsColl, InsertStatement{bucketDoc1}, nullptr));
        wunit.commit();
    }

    // Deletes the bucket document and inserts a new bucket document.
    const BSONObj bucketDoc2 = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a518"},
                "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},
                                              "a":10,
                                              "b":10},
                                       "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},
                                              "a":30,
                                              "b":30}},
                "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                                "1":{"$date":"2022-06-06T15:34:30.000Z"},
                                "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                        "a":{"0":10,"1":20,"2":30},
                        "b":{"0":10,"1":20,"2":30}}})");
    OID bucketId2 = bucketDoc2["_id"].OID();
    auto recordId2 = record_id_helpers::keyForOID(bucketId2);
    {
        mongo::write_ops::DeleteOpEntry deleteEntry(BSON("_id" << bucketId1), false);
        mongo::write_ops::DeleteCommandRequest deleteOp(_nsNoMeta.makeTimeseriesBucketsNamespace(),
                                                        {deleteEntry});
        mongo::write_ops::WriteCommandRequestBase base;
        base.setBypassDocumentValidation(true);
        base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});
        deleteOp.setWriteCommandRequestBase(base);

        mongo::write_ops::InsertCommandRequest insertOp(_nsNoMeta.makeTimeseriesBucketsNamespace(),
                                                        {bucketDoc2});
        insertOp.setWriteCommandRequestBase(base);

        ASSERT_DOES_NOT_THROW(
            performAtomicWrites(_opCtx,
                                *bucketsColl,
                                recordId1,
                                std::variant<mongo::write_ops::UpdateCommandRequest,
                                             mongo::write_ops::DeleteCommandRequest>{deleteOp},
                                {insertOp},
                                {},
                                /*fromMigrate=*/false,
                                /*stmtId=*/kUninitializedStmtId));
    }

    // Checks document1 is removed and document2 is added.
    {
        Snapshotted<BSONObj> doc;
        bool found = bucketsColl->findDoc(_opCtx, recordId1, &doc);
        ASSERT_FALSE(found);

        found = bucketsColl->findDoc(_opCtx, recordId2, &doc);
        ASSERT_TRUE(found);
        UnorderedFieldsBSONObjComparator comparator;
        ASSERT_EQ(0, comparator.compare(doc.value(), bucketDoc2));
    }
}

TEST_F(TimeseriesWriteUtilTest, PerformAtomicUpdateAndInserts) {
    // Inserts a bucket document.
    const BSONObj bucketDoc1 = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "meta":1,
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
    OID bucketId1 = bucketDoc1["_id"].OID();
    auto recordId1 = record_id_helpers::keyForOID(bucketId1);

    AutoGetCollection bucketsColl(
        _opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{_opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx, *bucketsColl, InsertStatement{bucketDoc1}, nullptr));
        wunit.commit();
    }

    // Updates the bucket document and inserts two new bucket documents.
    const BSONObj replaceDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":3,"b":3},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "meta":1,
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":3},
                    "b":{"0":3}}})");
    const BSONObj bucketDoc2 = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a518"},
                "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},
                                              "a":1,
                                              "b":1},
                                       "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},
                                              "a":1,
                                              "b":1}},
                "meta":2,
                "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"}},
                        "a":{"0":1},
                        "b":{"0":1}}})");
    OID bucketId2 = bucketDoc2["_id"].OID();
    auto recordId2 = record_id_helpers::keyForOID(bucketId2);
    const BSONObj bucketDoc3 = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a519"},
                "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},
                                              "a":2,
                                              "b":2},
                                       "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},
                                              "a":2,
                                              "b":2}},
                "meta":3,
                "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"}},
                        "a":{"0":2},
                        "b":{"0":2}}})");
    OID bucketId3 = bucketDoc3["_id"].OID();
    auto recordId3 = record_id_helpers::keyForOID(bucketId3);
    {
        mongo::write_ops::UpdateModification u(replaceDoc);
        mongo::write_ops::UpdateOpEntry update(BSON("_id" << bucketId1), std::move(u));
        mongo::write_ops::UpdateCommandRequest updateOp(_nsNoMeta.makeTimeseriesBucketsNamespace(),
                                                        {update});
        mongo::write_ops::WriteCommandRequestBase base;
        base.setBypassDocumentValidation(true);
        base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});
        updateOp.setWriteCommandRequestBase(base);

        mongo::write_ops::InsertCommandRequest insertOp1(_nsNoMeta.makeTimeseriesBucketsNamespace(),
                                                         {bucketDoc2});
        insertOp1.setWriteCommandRequestBase(base);
        mongo::write_ops::InsertCommandRequest insertOp2(_nsNoMeta.makeTimeseriesBucketsNamespace(),
                                                         {bucketDoc3});
        insertOp2.setWriteCommandRequestBase(base);

        ASSERT_DOES_NOT_THROW(
            performAtomicWrites(_opCtx,
                                *bucketsColl,
                                recordId1,
                                std::variant<mongo::write_ops::UpdateCommandRequest,
                                             mongo::write_ops::DeleteCommandRequest>{updateOp},
                                {insertOp1, insertOp2},
                                {},
                                /*fromMigrate=*/false,
                                /*stmtId=*/kUninitializedStmtId));
    }

    // Checks document1 is updated and document2 and document3 are added.
    {
        Snapshotted<BSONObj> doc;
        bool found = bucketsColl->findDoc(_opCtx, recordId1, &doc);
        ASSERT_TRUE(found);
        UnorderedFieldsBSONObjComparator comparator;
        ASSERT_EQ(0, comparator.compare(doc.value(), replaceDoc));

        found = bucketsColl->findDoc(_opCtx, recordId2, &doc);
        ASSERT_TRUE(found);
        ASSERT_EQ(0, comparator.compare(doc.value(), bucketDoc2));

        found = bucketsColl->findDoc(_opCtx, recordId3, &doc);
        ASSERT_TRUE(found);
        ASSERT_EQ(0, comparator.compare(doc.value(), bucketDoc3));
    }
}

TEST_F(TimeseriesWriteUtilTest, PerformAtomicWritesForUserDelete) {
    // Inserts a bucket document.
    const BSONObj uncompressedDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"66e1d884953633cfd2c479f2"},
            "control":{"version":1,"min":{"time":{"$date":"2024-09-11T17:51:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2024-09-11T17:53:18.428Z"},"a":3,"b":3},
                                   "count":3},
            "data":{"time":{"0":{"$date":"2024-09-11T17:51:30.000Z"},
                            "1":{"$date":"2024-09-11T17:52:12.000Z"},
                            "2":{"$date":"2024-09-11T17:53:18.428Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    CompressionResult compressionResult = compressBucket(uncompressedDoc,
                                                         _timeField,
                                                         _nsNoMeta,
                                                         /*validateDecompression*/ true);
    const BSONObj& bucketDoc = compressionResult.compressedBucket.value();
    auto minTime = bucketDoc.getObjectField(kBucketControlFieldName)
                       .getObjectField(kBucketControlMinFieldName)
                       .getField("time")
                       .Date();
    OID bucketId = bucketDoc["_id"].OID();
    auto recordId = record_id_helpers::keyForOID(bucketId);

    AutoGetCollection bucketsColl(
        _opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{_opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx, *bucketsColl, InsertStatement{bucketDoc}, nullptr));
        wunit.commit();
    }

    // Deletes two measurements from the bucket. This includes deleting the earliest measurement. We
    // should check that the minTime of the bucket does not get changed in spite of this.
    {
        ASSERT_DOES_NOT_THROW(performAtomicWritesForDelete(
            _opCtx,
            *bucketsColl,
            recordId,
            {::mongo::fromjson(R"({"time":{"$date":"2024-09-11T17:53:18.428Z"},"a":3,"b":3})")},
            /*fromMigrate=*/false,
            /*stmtId=*/kUninitializedStmtId,
            minTime));
    }

    // Checks only one measurement is left in the bucket. Ensure that the time of the last remaining
    // measurement has not changed, and that our bucket's control.min.time has not changed.
    {
        const BSONObj uncompressedReplaceDoc = ::mongo::fromjson(
            R"({"_id":{"$oid":"66e1d884953633cfd2c479f2"},
            "control":{"version":1,"min":{"time":{"$date":"2024-09-11T17:51:00.000Z"},"a":3,"b":3},
                                   "max":{"time":{"$date":"2024-09-11T17:53:18.428Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2024-09-11T17:53:18.428Z"}},
                    "a":{"0":3},
                    "b":{"0":3}}})");
        // As part of this test we are checking that deleting the earliest measurements in a bucket
        // does not impact the bucket's control.min time-field. To verify that this was done here,
        // let's check that the remaining measurement's rounded time is not equal to the current
        // control.min time. That way, we'll know that if we were incorrectly updating the
        // control.min time to be the min time of the remaining measurement, it would not be the
        // same time as it was when it was based on the true earliest (now-deleted) measurement.
        // TODO (SERVER-94872): Revisit this behavior, we may no longer need this check.
        auto remainingMeasurementMinTime =
            roundTimestampToGranularity(uncompressedReplaceDoc.getObjectField(kBucketDataFieldName)
                                            .getObjectField("time")
                                            .getField("0")
                                            .Date(),
                                        _getTimeseriesOptions(_nsNoMeta));
        auto controlMinTime = uncompressedReplaceDoc.getObjectField(kBucketControlFieldName)
                                  .getObjectField(kBucketControlMinFieldName)
                                  .getField("time")
                                  .Date();
        ASSERT_NE(remainingMeasurementMinTime, controlMinTime);
        CompressionResult compressionResult = compressBucket(uncompressedReplaceDoc,
                                                             _timeField,
                                                             _nsNoMeta,
                                                             /*validateDecompression*/ true);
        const BSONObj& replaceDoc = compressionResult.compressedBucket.value();

        Snapshotted<BSONObj> doc;
        bool found = bucketsColl->findDoc(_opCtx, recordId, &doc);

        ASSERT_TRUE(found);
        UnorderedFieldsBSONObjComparator comparator;
        ASSERT_EQ(0, comparator.compare(doc.value(), replaceDoc));
    }

    // Deletes the last measurement from the bucket.
    {
        ASSERT_DOES_NOT_THROW(performAtomicWritesForDelete(_opCtx,
                                                           *bucketsColl,
                                                           recordId,
                                                           {},
                                                           /*fromMigrate=*/false,
                                                           /*stmtId=*/kUninitializedStmtId,
                                                           minTime));
    }

    // Checks the document is removed.
    {
        Snapshotted<BSONObj> doc;
        bool found = bucketsColl->findDoc(_opCtx, recordId, &doc);
        ASSERT_FALSE(found);
    }
}

TEST_F(TimeseriesWriteUtilTest, PerformAtomicWritesForUserUpdate) {
    // Inserts a bucket document.
    const BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":2,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3},
                                   "count":3},
            "data":{"time":{"$binary":"CQBwO6c5gQEAAIANAAAAAAAAAAA=","$type":"07"},
                    "a":{"$binary":"AQAAAAAAAADwP5AtAAAACAAAAAA=","$type":"07"},
                    "b":{"$binary":"AQAAAAAAAADwP5AtAAAACAAAAAA=","$type":"07"}}})");
    OID bucketId = bucketDoc["_id"].OID();
    auto recordId = record_id_helpers::keyForOID(bucketId);
    auto minTime =
        bucketDoc.getObjectField("control").getObjectField("min").getField("time").Date();

    AutoGetCollection bucketsColl(
        _opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{_opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx, *bucketsColl, InsertStatement{bucketDoc}, nullptr));
        wunit.commit();
    }

    // Updates two measurements from the bucket.
    {
        std::vector<BSONObj> unchangedMeasurements{
            ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":2,"b":2})")};
        std::set<bucket_catalog::BucketId> bucketIds{};
        bucket_catalog::BucketCatalog sideBucketCatalog{
            1, getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes};
        ASSERT_DOES_NOT_THROW(performAtomicWritesForUpdate(
            _opCtx,
            *bucketsColl,
            recordId,
            unchangedMeasurements,
            {::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":10,"b":10})"),
             ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":30,"b":30})")},
            sideBucketCatalog,
            /*fromMigrate=*/false,
            /*stmtId=*/kUninitializedStmtId,
            &bucketIds,
            minTime));
        ASSERT_EQ(bucketIds.size(), 1);
    }

    // Checks only one measurement is left in the original bucket and a new document was inserted.
    {
        const BSONObj replaceDoc = ::mongo::fromjson(
            R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":2,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":2,"b":2},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":2,"b":2},
                                   "count":1},
            "data":{"time":{"$binary":"CQBwO6c5gQEAAAA=","$type":"07"},
                    "a":{"$binary":"EAACAAAAAA==","$type":"07"},
                    "b":{"$binary":"EAACAAAAAA==","$type":"07"}}})");
        Snapshotted<BSONObj> doc;
        bool found = bucketsColl->findDoc(_opCtx, recordId, &doc);

        ASSERT_TRUE(found);
        UnorderedFieldsBSONObjComparator comparator;
        ASSERT_EQ(0, comparator.compare(doc.value(), replaceDoc));

        ASSERT_EQ(2, bucketsColl->numRecords(_opCtx));
    }
}

TEST_F(TimeseriesWriteUtilTest, TrackInsertedBuckets) {
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
    OID bucketId = bucketDoc["_id"].OID();
    auto recordId = record_id_helpers::keyForOID(bucketId);
    auto minTime =
        bucketDoc.getObjectField("control").getObjectField("min").getField("time").Date();

    AutoGetCollection bucketsColl(
        _opCtx, _nsNoMeta.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{_opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            _opCtx, *bucketsColl, InsertStatement{bucketDoc}, nullptr));
        wunit.commit();
    }

    std::set<bucket_catalog::BucketId> bucketIds{};
    bucket_catalog::BucketCatalog sideBucketCatalog{
        1, getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes};

    // Updates one measurement. One new bucket is created.
    {
        std::vector<BSONObj> unchangedMeasurements{
            ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":2,"b":2})"),
            ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3})")};

        ASSERT_DOES_NOT_THROW(performAtomicWritesForUpdate(
            _opCtx,
            *bucketsColl,
            recordId,
            unchangedMeasurements,
            {::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":10,"b":10})")},
            sideBucketCatalog,
            /*fromMigrate=*/false,
            /*stmtId=*/kUninitializedStmtId,
            &bucketIds,
            minTime));
        ASSERT_EQ(bucketIds.size(), 1);
    }

    // Updates another measurement. No new bucket should be created.
    {
        std::vector<BSONObj> unchangedMeasurements{
            ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3})")};

        ASSERT_DOES_NOT_THROW(performAtomicWritesForUpdate(
            _opCtx,
            *bucketsColl,
            recordId,
            unchangedMeasurements,
            {::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":20,"b":20})")},
            sideBucketCatalog,
            /*fromMigrate=*/false,
            /*stmtId=*/kUninitializedStmtId,
            &bucketIds,
            minTime));
        ASSERT_EQ(bucketIds.size(), 1);
    }

    // Updates the last measurement with different schema. One more bucket is created.
    {
        std::vector<BSONObj> unchangedMeasurements{};

        ASSERT_DOES_NOT_THROW(performAtomicWritesForUpdate(
            _opCtx,
            *bucketsColl,
            recordId,
            unchangedMeasurements,
            {::mongo::fromjson(
                R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":"30","b":"30"})")},
            sideBucketCatalog,
            /*fromMigrate=*/false,
            /*stmtId=*/kUninitializedStmtId,
            &bucketIds,
            minTime));
        ASSERT_EQ(bucketIds.size(), 2);
    }
}

}  // namespace
}  // namespace mongo::timeseries
