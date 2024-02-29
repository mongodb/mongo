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
#include <boost/move/utility_core.hpp>
#include <cstdint>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries//timeseries_constants.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::timeseries {
namespace details {
inline bool operator==(const Measurement& lhs, const Measurement& rhs) {
    bool timeFieldEqual = (lhs.timeField.woCompare(rhs.timeField) == 0);
    if (!timeFieldEqual || (lhs.dataFields.size() != rhs.dataFields.size())) {
        return false;
    }

    StringMap<BSONElement> rhsFields;
    for (auto& field : rhs.dataFields) {
        rhsFields.insert({field.fieldNameStringData().toString(), field});
    }

    for (size_t i = 0; i < lhs.dataFields.size(); ++i) {
        auto& lhsField = lhs.dataFields[i];
        auto it = rhsFields.find(lhsField.fieldNameStringData().toString());
        if (it == rhsFields.end()) {
            return false;
        }

        if (it->second.woCompare(lhsField) != 0) {
            return false;
        }
    }
    return true;
}
}  // namespace details

namespace {

const std::string testDbName = "db_timeseries_write_util_test";
const TimeseriesOptions kTimeseriesOptions("time");

class TimeseriesWriteUtilTest : public CatalogTestFixture {
protected:
    using CatalogTestFixture::setUp;

    std::shared_ptr<bucket_catalog::WriteBatch> generateBatch(
        const UUID& uuid,
        bucket_catalog::BucketMetadata bucketMetadata = {{}, nullptr, boost::none}) {
        OID oid = OID::createFromString("629e1e680958e279dc29a517"_sd);
        bucket_catalog::BucketId bucketId(uuid, oid);
        std::uint8_t stripe = 0;
        auto opId = 0;
        auto collectionStats = std::make_shared<bucket_catalog::ExecutionStats>();
        bucket_catalog::ExecutionStatsController stats(collectionStats, _globalStats);
        return std::make_shared<bucket_catalog::WriteBatch>(
            _trackingContext,
            bucket_catalog::BucketHandle{bucketId, stripe},
            bucket_catalog::BucketKey{uuid, std::move(bucketMetadata)},
            opId,
            stats,
            kTimeseriesOptions.getTimeField());
    }

private:
    TrackingContext _trackingContext;
    bucket_catalog::ExecutionStats _globalStats;
};

TEST_F(TimeseriesWriteUtilTest, MakeNewBucketFromWriteBatch) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "MakeNewBucketFromWriteBatch");

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
        timeseries::makeNewDocumentForWrite(ns, batch, /*metadata=*/{}).uncompressedBucket;

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
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "MakeNewBucketFromWriteBatchWithMeta");

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
    auto newDoc = timeseries::makeNewDocumentForWrite(ns, batch, metadata).uncompressedBucket;

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTimeseriesAlwaysUseCompressedBuckets", true);
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "MakeNewCompressedBucketFromWriteBatch");

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
    auto bucketDoc = timeseries::makeNewDocumentForWrite(ns, batch, /*metadata=*/{});

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTimeseriesAlwaysUseCompressedBuckets", true);
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "MakeNewCompressedBucketFromWriteBatchWithMeta");

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
    auto bucketDoc = timeseries::makeNewDocumentForWrite(ns, batch, metadata);

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
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "MakeNewBucketFromMeasurements");
    UUID uuid = UUID::gen();
    OID oid = OID::createFromString("629e1e680958e279dc29a517"_sd);
    TimeseriesOptions options("time");
    options.setGranularity(BucketGranularityEnum::Seconds);
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":1,"b":1})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":2,"b":2})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:33:30.000Z"},"a":3,"b":3})")};

    // Makes the new document for write.
    auto newDoc = timeseries::makeNewDocumentForWrite(
                      ns, uuid, oid, measurements, /*metadata=*/{}, options, /*comparator=*/nullptr)
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
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "MakeNewBucketFromMeasurementsWithMeta");
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
    auto newDoc = timeseries::makeNewDocumentForWrite(
                      ns, uuid, oid, measurements, metadata, options, /*comparator=*/nullptr)
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
 * Test that makeTimeseriesCompressedDiffUpdateOp returns the expected diff object when
 * inserting measurements into a compressed bucket, and that out-of-order
 * measurements cause a bucket to upgraded to a v3 bucket.
 */
TEST_F(TimeseriesWriteUtilTest, MakeTimeseriesCompressedDiffUpdateOp) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTimeseriesAlwaysUseCompressedBuckets", true);
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "MakeTimeseriesCompressedDiffUpdateOp");

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

    const auto preImageCompressionResult =
        timeseries::compressBucket(uncompressedPreImage,
                                   kTimeseriesOptions.getTimeField(),
                                   ns,
                                   /*validateCompression=*/true);
    ASSERT_TRUE(preImageCompressionResult.compressedBucket);

    setUncompressedBucketDoc(*batch, uncompressedPreImage);
    batch->compressedBucketDoc = *preImageCompressionResult.compressedBucket;

    batch->numPreviouslyCommittedMeasurements = 3;
    BSONObj bucketDataDoc =
        batch->compressedBucketDoc->getObjectField(kBucketDataFieldName).getOwned();
    batch->intermediateBuilders.initBuilders(bucketDataDoc,
                                             batch->numPreviouslyCommittedMeasurements);

    /*const BSONObj expectedDiff = fromjson(
        R"({
        "scontrol":{"u":{"count":5,"version":3},
                    "smin":{"u":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":0,"b":0}},
                    "smax":{"u":{"time":{"$date":"2022-06-06T15:34:34.000Z"},"a":4,"b":4}}},
        "sdata":{
            "b":{"time":{"o":10,"d":{"$binary":"gAt9AAD8fGBtAA==","$type":"00"}},
                 "a":{"o":6,"d":{"$binary":"gCsAEAAUABAAAA==","$type":"00"}},
                 "b":{"o":6,"d":{"$binary":"gCsAEAAUABAAAA==","$type":"00"}}}}
        })");*/

    auto request = makeTimeseriesCompressedDiffUpdateOp(
        operationContext(), batch, ns.makeTimeseriesBucketsNamespace());
    auto& updates = request.getUpdates();

    ASSERT_EQ(updates.size(), 1);
    // The update command request should return the document diff of the batch applied on the pre
    // image.
    // TODO SERVER-79416: Re-enable this test. The expected offsets above are not correct with naive
    // diff implementation.
    // ASSERT(updates[0].getU().getDiff().binaryEqual(expectedDiff));
}

/**
 * Test that makeTimeseriesCompressedDiffUpdateOp returns the expected diff object when
 * inserting measurements with meta fields into a compressed bucket, and that out-of-order
 * measurements cause a bucket to upgraded to a v3 bucket.
 */
TEST_F(TimeseriesWriteUtilTest, MakeTimeseriesCompressedDiffUpdateOpWithMeta) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTimeseriesAlwaysUseCompressedBuckets", true);
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "MakeTimeseriesCompressedDiffUpdateOpWithMeta");

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
        generateBatch(UUID::gen(), {uncompressedPreImage.getField("meta"), nullptr, boost::none});
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"meta":{"tag":1},"a":0,"b":0})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:34.000Z"},"meta":{"tag":1},"a":4,"b":4})"),
    };

    batch->min = fromjson(R"({"u": {"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":0,"b":0}})");
    batch->max = fromjson(R"({"u": {"time":{"$date":"2022-06-06T15:34:34.000Z"},"a":4,"b":4}})");
    batch->measurements = {measurements.begin(), measurements.end()};
    auto metadata = fromjson(R"({"meta":{"tag":1}})");

    const auto preImageCompressionResult =
        timeseries::compressBucket(uncompressedPreImage,
                                   kTimeseriesOptions.getTimeField(),
                                   ns,
                                   /*validateCompression=*/true);
    ASSERT_TRUE(preImageCompressionResult.compressedBucket);

    setUncompressedBucketDoc(*batch, uncompressedPreImage);
    batch->compressedBucketDoc = *preImageCompressionResult.compressedBucket;

    batch->numPreviouslyCommittedMeasurements = 3;
    BSONObj bucketDataDoc =
        batch->compressedBucketDoc->getObjectField(kBucketDataFieldName).getOwned();
    batch->intermediateBuilders.initBuilders(bucketDataDoc,
                                             batch->numPreviouslyCommittedMeasurements);

    /*const BSONObj expectedDiff = fromjson(
        R"({
        "scontrol":{"u":{"count":5, "version":3},
                    "smin":{"u":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":0,"b":0}},
                    "smax":{"u":{"time":{"$date":"2022-06-06T15:34:34.000Z"},"a":4,"b":4}}},
        "sdata":{
            "b":{"time":{"o":10,"d":{"$binary":"gAt9AAD8fGBtAA==","$type":"00"}},
                 "a":{"o":6,"d":{"$binary":"gCsAEAAUABAAAA==","$type":"00"}},
                 "b":{"o":6,"d":{"$binary":"gCsAEAAUABAAAA==","$type":"00"}}}}
        })");*/

    auto request = makeTimeseriesCompressedDiffUpdateOp(
        operationContext(), batch, ns.makeTimeseriesBucketsNamespace());
    auto& updates = request.getUpdates();

    ASSERT_EQ(updates.size(), 1);

    // The update command request should return the document diff of the batch applied on the pre
    // image.
    // TODO SERVER-79416: Re-enable this test. The expected offsets above are not correct with naive
    // diff implementation.
    // ASSERT(updates[0].getU().getDiff().binaryEqual(expectedDiff));
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

        ASSERT_DOES_NOT_THROW(performAtomicWrites(
            opCtx,
            bucketsColl.getCollection(),
            recordId,
            std::variant<write_ops::UpdateCommandRequest, write_ops::DeleteCommandRequest>{op},
            {},
            {},
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

        ASSERT_DOES_NOT_THROW(performAtomicWrites(
            opCtx,
            bucketsColl.getCollection(),
            recordId,
            std::variant<write_ops::UpdateCommandRequest, write_ops::DeleteCommandRequest>{op},
            {},
            {},
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

TEST_F(TimeseriesWriteUtilTest, PerformAtomicDeleteAndInsert) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "PerformAtomicDeleteAndInsert");
    auto opCtx = operationContext();
    ASSERT_OK(createCollection(opCtx,
                               ns.dbName(),
                               BSON("create" << ns.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"))));

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

    AutoGetCollection bucketsColl(opCtx, ns.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            opCtx, *bucketsColl, InsertStatement{bucketDoc1}, nullptr));
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
        write_ops::DeleteOpEntry deleteEntry(BSON("_id" << bucketId1), false);
        write_ops::DeleteCommandRequest deleteOp(ns.makeTimeseriesBucketsNamespace(),
                                                 {deleteEntry});
        write_ops::WriteCommandRequestBase base;
        base.setBypassDocumentValidation(true);
        base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});
        deleteOp.setWriteCommandRequestBase(base);

        write_ops::InsertCommandRequest insertOp(ns.makeTimeseriesBucketsNamespace(), {bucketDoc2});
        insertOp.setWriteCommandRequestBase(base);

        ASSERT_DOES_NOT_THROW(performAtomicWrites(
            opCtx,
            bucketsColl.getCollection(),
            recordId1,
            std::variant<write_ops::UpdateCommandRequest, write_ops::DeleteCommandRequest>{
                deleteOp},
            {insertOp},
            {},
            /*fromMigrate=*/false,
            /*stmtId=*/kUninitializedStmtId));
    }

    // Checks document1 is removed and document2 is added.
    {
        Snapshotted<BSONObj> doc;
        bool found = bucketsColl->findDoc(opCtx, recordId1, &doc);
        ASSERT_FALSE(found);

        found = bucketsColl->findDoc(opCtx, recordId2, &doc);
        ASSERT_TRUE(found);
        UnorderedFieldsBSONObjComparator comparator;
        ASSERT_EQ(0, comparator.compare(doc.value(), bucketDoc2));
    }
}

TEST_F(TimeseriesWriteUtilTest, PerformAtomicUpdateAndInserts) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "PerformAtomicUpdateAndInserts");
    auto opCtx = operationContext();
    ASSERT_OK(createCollection(opCtx,
                               ns.dbName(),
                               BSON("create" << ns.coll() << "timeseries"
                                             << BSON("timeField"
                                                     << "time"))));

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

    AutoGetCollection bucketsColl(opCtx, ns.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            opCtx, *bucketsColl, InsertStatement{bucketDoc1}, nullptr));
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
        write_ops::UpdateModification u(replaceDoc);
        write_ops::UpdateOpEntry update(BSON("_id" << bucketId1), std::move(u));
        write_ops::UpdateCommandRequest updateOp(ns.makeTimeseriesBucketsNamespace(), {update});
        write_ops::WriteCommandRequestBase base;
        base.setBypassDocumentValidation(true);
        base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});
        updateOp.setWriteCommandRequestBase(base);

        write_ops::InsertCommandRequest insertOp1(ns.makeTimeseriesBucketsNamespace(),
                                                  {bucketDoc2});
        insertOp1.setWriteCommandRequestBase(base);
        write_ops::InsertCommandRequest insertOp2(ns.makeTimeseriesBucketsNamespace(),
                                                  {bucketDoc3});
        insertOp2.setWriteCommandRequestBase(base);

        ASSERT_DOES_NOT_THROW(performAtomicWrites(
            opCtx,
            bucketsColl.getCollection(),
            recordId1,
            std::variant<write_ops::UpdateCommandRequest, write_ops::DeleteCommandRequest>{
                updateOp},
            {insertOp1, insertOp2},
            {},
            /*fromMigrate=*/false,
            /*stmtId=*/kUninitializedStmtId));
    }

    // Checks document1 is updated and document2 and document3 are added.
    {
        Snapshotted<BSONObj> doc;
        bool found = bucketsColl->findDoc(opCtx, recordId1, &doc);
        ASSERT_TRUE(found);
        UnorderedFieldsBSONObjComparator comparator;
        ASSERT_EQ(0, comparator.compare(doc.value(), replaceDoc));

        found = bucketsColl->findDoc(opCtx, recordId2, &doc);
        ASSERT_TRUE(found);
        ASSERT_EQ(0, comparator.compare(doc.value(), bucketDoc2));

        found = bucketsColl->findDoc(opCtx, recordId3, &doc);
        ASSERT_TRUE(found);
        ASSERT_EQ(0, comparator.compare(doc.value(), bucketDoc3));
    }
}

TEST_F(TimeseriesWriteUtilTest, PerformAtomicWritesForUserDelete) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "PerformAtomicWritesForUserDelete");
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
    OID bucketId = bucketDoc["_id"].OID();
    auto recordId = record_id_helpers::keyForOID(bucketId);

    AutoGetCollection bucketsColl(opCtx, ns.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            opCtx, *bucketsColl, InsertStatement{bucketDoc}, nullptr));
        wunit.commit();
    }

    // Deletes two measurements from the bucket.
    {
        ASSERT_DOES_NOT_THROW(performAtomicWritesForDelete(
            opCtx,
            bucketsColl.getCollection(),
            recordId,
            {::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":2,"b":2})")},
            /*fromMigrate=*/false,
            /*stmtId=*/kUninitializedStmtId));
    }

    // Checks only one measurement is left in the bucket.
    {
        const BSONObj replaceDoc = ::mongo::fromjson(
            R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":2,"b":2},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":2,"b":2}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":2},
                    "b":{"0":2}}})");
        Snapshotted<BSONObj> doc;
        bool found = bucketsColl->findDoc(opCtx, recordId, &doc);

        ASSERT_TRUE(found);
        UnorderedFieldsBSONObjComparator comparator;
        ASSERT_EQ(0, comparator.compare(doc.value(), replaceDoc));
    }

    // Deletes the last measurement from the bucket.
    {
        ASSERT_DOES_NOT_THROW(performAtomicWritesForDelete(opCtx,
                                                           bucketsColl.getCollection(),
                                                           recordId,
                                                           {},
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

TEST_F(TimeseriesWriteUtilTest, PerformAtomicWritesForUserUpdate) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "PerformAtomicWritesForUserUpdate");
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
    OID bucketId = bucketDoc["_id"].OID();
    auto recordId = record_id_helpers::keyForOID(bucketId);

    AutoGetCollection bucketsColl(opCtx, ns.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            opCtx, *bucketsColl, InsertStatement{bucketDoc}, nullptr));
        wunit.commit();
    }

    // Updates two measurements from the bucket.
    {
        std::vector<BSONObj> unchangedMeasurements{
            ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":2,"b":2})")};
        std::set<OID> bucketIds{};
        bucket_catalog::BucketCatalog sideBucketCatalog{
            1, getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes};
        ASSERT_DOES_NOT_THROW(performAtomicWritesForUpdate(
            opCtx,
            bucketsColl.getCollection(),
            recordId,
            unchangedMeasurements,
            {::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":10,"b":10})"),
             ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":30,"b":30})")},
            sideBucketCatalog,
            /*fromMigrate=*/false,
            /*stmtId=*/kUninitializedStmtId,
            &bucketIds));
        ASSERT_EQ(bucketIds.size(), 1);
    }

    // Checks only one measurement is left in the original bucket and a new document was inserted.
    {
        const BSONObj replaceDoc = ::mongo::fromjson(
            R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":2,"b":2},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":2,"b":2}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":2},
                    "b":{"0":2}}})");
        Snapshotted<BSONObj> doc;
        bool found = bucketsColl->findDoc(opCtx, recordId, &doc);

        ASSERT_TRUE(found);
        UnorderedFieldsBSONObjComparator comparator;
        ASSERT_EQ(0, comparator.compare(doc.value(), replaceDoc));

        ASSERT_EQ(2, bucketsColl->numRecords(opCtx));
    }
}

TEST_F(TimeseriesWriteUtilTest, TrackInsertedBuckets) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_util_test", "TrackInsertedBuckets");
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
    OID bucketId = bucketDoc["_id"].OID();
    auto recordId = record_id_helpers::keyForOID(bucketId);

    AutoGetCollection bucketsColl(opCtx, ns.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    {
        WriteUnitOfWork wunit{opCtx};
        ASSERT_OK(collection_internal::insertDocument(
            opCtx, *bucketsColl, InsertStatement{bucketDoc}, nullptr));
        wunit.commit();
    }

    std::set<OID> bucketIds{};
    bucket_catalog::BucketCatalog sideBucketCatalog{
        1, getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes};

    // Updates one measurement. One new bucket is created.
    {
        std::vector<BSONObj> unchangedMeasurements{
            ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":2,"b":2})"),
            ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3})")};

        ASSERT_DOES_NOT_THROW(performAtomicWritesForUpdate(
            opCtx,
            bucketsColl.getCollection(),
            recordId,
            unchangedMeasurements,
            {::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":10,"b":10})")},
            sideBucketCatalog,
            /*fromMigrate=*/false,
            /*stmtId=*/kUninitializedStmtId,
            &bucketIds));
        ASSERT_EQ(bucketIds.size(), 1);
    }

    // Updates another measurement. No new bucket should be created.
    {
        std::vector<BSONObj> unchangedMeasurements{
            ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3})")};

        ASSERT_DOES_NOT_THROW(performAtomicWritesForUpdate(
            opCtx,
            bucketsColl.getCollection(),
            recordId,
            unchangedMeasurements,
            {::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":20,"b":20})")},
            sideBucketCatalog,
            /*fromMigrate=*/false,
            /*stmtId=*/kUninitializedStmtId,
            &bucketIds));
        ASSERT_EQ(bucketIds.size(), 1);
    }

    // Updates the last measurement with different schema. One more bucket is created.
    {
        std::vector<BSONObj> unchangedMeasurements{};

        ASSERT_DOES_NOT_THROW(performAtomicWritesForUpdate(
            opCtx,
            bucketsColl.getCollection(),
            recordId,
            unchangedMeasurements,
            {::mongo::fromjson(
                R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":"30","b":"30"})")},
            sideBucketCatalog,
            /*fromMigrate=*/false,
            /*stmtId=*/kUninitializedStmtId,
            &bucketIds));
        ASSERT_EQ(bucketIds.size(), 2);
    }
}

TEST_F(TimeseriesWriteUtilTest, SortMeasurementsOnTimeField) {
    const BSONObj metaField = fromjson(R"({"meta":{"tag":1}})");

    // The meta field should be filtered by the sorting process.
    const std::vector<BSONObj> measurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:50.000Z"},"meta":{"tag":1},"a":1,"b":1})"),
        fromjson(R"({"time":{"$date":"2022-06-07T15:34:30.000Z"},"meta":{"tag":1},"a":2,"b":2})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"meta":{"tag":1},"a":3,"b":3})")};

    auto batch = generateBatch(UUID::gen(), {metaField.getField("meta"), nullptr, boost::none});
    batch->measurements = {measurements.begin(), measurements.end()};
    batch->min = fromjson(R"({"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1})");
    batch->max = fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3})");
    batch->timeField = kTimeseriesOptions.getTimeField();

    std::vector testMeasurements = details::sortMeasurementsOnTimeField(batch);

    const std::vector<BSONObj> sortedMeasurements = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:50.000Z"},"a":1,"b":1})"),
        fromjson(R"({"time":{"$date":"2022-06-07T15:34:30.000Z"},"a":2,"b":2})")};

    const std::vector<BSONObj> sortedTimeFields = {
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:30.000Z"}})"),
        fromjson(R"({"time":{"$date":"2022-06-06T15:34:50.000Z"}})"),
        fromjson(R"({"time":{"$date":"2022-06-07T15:34:30.000Z"}})")};

    ASSERT_EQ(testMeasurements.size(), sortedMeasurements.size());
    for (size_t i = 0; i < sortedMeasurements.size(); ++i) {
        details::Measurement m;
        m.timeField = sortedTimeFields[i].getField("time");
        m.dataFields.push_back(sortedMeasurements[i].getField("time"));
        m.dataFields.push_back(sortedMeasurements[i].getField("a"));
        m.dataFields.push_back(sortedMeasurements[i].getField("b"));
        ASSERT_EQ(m, testMeasurements[i]);
    }
}

}  // namespace
}  // namespace mongo::timeseries
