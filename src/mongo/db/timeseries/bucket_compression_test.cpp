// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/bucket_compression.h"

#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

#include <boost/optional/optional.hpp>

using namespace std::literals::string_view_literals;

namespace mongo {
namespace {

const BSONObj sampleBucket = mongo::fromjson(R"({
        "_id" : {"$oid": "630ea4802093f9983fc394dc"},
        "control" : {
                "version" : 1,
                "min" : {
                        "_id" : {"$oid": "630fabf7c388456f8aea4f2d"},
                        "t" : {"$date": "2022-08-31T00:00:00Z"},
                        "a" : 0
                },
                "max" : {
                        "_id" : {"$oid": "630fabf7c388456f8aea4f35"},
                        "t" : {"$date": "2022-08-31T00:00:04Z"},
                        "a" : 4
                }
        },
        "data" : {
                "_id" : {
                        "0" : {"$oid": "630fabf7c388456f8aea4f2d"},
                        "1" : {"$oid": "630fabf7c388456f8aea4f2f"},
                        "2" : {"$oid": "630fabf7c388456f8aea4f31"},
                        "3" : {"$oid": "630fabf7c388456f8aea4f33"},
                        "4" : {"$oid": "630fabf7c388456f8aea4f35"}
                },
                "a" : {
                        "0" : 0,
                        "1" : 1,
                        "2" : 2,
                        "3" : 3,
                        "4" : 4
                },
                "t" : {
                        "0" : {"$date": "2022-08-31T00:00:00Z"},
                        "1" : {"$date": "2022-08-31T00:00:01Z"},
                        "2" : {"$date": "2022-08-31T00:00:02Z"},
                        "3" : {"$date": "2022-08-31T00:00:03Z"},
                        "4" : {"$date": "2022-08-31T00:00:04Z"}
                }
        }
})");

const BSONObj bucketWithDuplicateIndexFieldNames = mongo::fromjson(R"({
        "_id" : {"$oid": "630ea4802093f9983fc394dc"},
        "control" : {
                "version" : 1,
                "min" : {
                        "_id" : {"$oid": "630fabf7c388456f8aea4f2d"},
                        "t" : {"$date": "2022-08-31T00:00:00Z"},
                        "a" : 0
                },
                "max" : {
                        "_id" : {"$oid": "630fabf7c388456f8aea4f33"},
                        "t" : {"$date": "2022-08-31T00:00:03Z"},
                        "a" : 3
                }
        },
        "data" : {
                "_id" : {
                        "0" : {"$oid": "630fabf7c388456f8aea4f2d"},
                        "1" : {"$oid": "630fabf7c388456f8aea4f2f"},
                        "1" : {"$oid": "630fabf7c388456f8aea4f31"},
                        "2" : {"$oid": "630fabf7c388456f8aea4f33"}
                },
                "a" : {
                        "0" : 0,
                        "1" : 1,
                        "1" : 2,
                        "2" : 3
                },
                "t" : {
                        "0" : {"$date": "2022-08-31T00:00:00Z"},
                        "1" : {"$date": "2022-08-31T00:00:01Z"},
                        "1" : {"$date": "2022-08-31T00:00:02Z"},
                        "2" : {"$date": "2022-08-31T00:00:03Z"}
                }
        }
})");

void assertNoDuplicateIndexFieldNames(const BSONObj& column) {
    size_t curIdx = 0;
    for (const auto elemIt : column) {
        EXPECT_EQ(std::to_string(curIdx++), elemIt.fieldName());
    }
}

TEST(TimeseriesBucketCompression, BasicRoundtrip) {
    auto compressed = timeseries::compressBucket(
        sampleBucket, "t"sv, NamespaceString::createNamespaceString_forTest("test.foo"), false);
    ASSERT_TRUE(compressed.compressedBucket.has_value());
    auto decompressed = timeseries::decompressBucket(compressed.compressedBucket.value());
    ASSERT_TRUE(decompressed.has_value());

    // Compression will re-order data fields, moving the timeField to the front.
    UnorderedFieldsBSONObjComparator comparator;
    EXPECT_EQ(0, comparator.compare(decompressed.value(), sampleBucket));
}

TEST(TimeseriesBucketCompression, RoundtripWithDuplicateIndexFieldNames) {
    const std::string_view timeFieldName("t");
    auto compressed =
        timeseries::compressBucket(bucketWithDuplicateIndexFieldNames,
                                   timeFieldName,
                                   NamespaceString::createNamespaceString_forTest("test.foo"),
                                   false);
    ASSERT_TRUE(compressed.compressedBucket.has_value());
    auto decompressed = timeseries::decompressBucket(compressed.compressedBucket.value());
    ASSERT_TRUE(decompressed.has_value());

    // Compression will re-order data fields, moving the timeField to the front.
    UnorderedFieldsBSONObjComparator comparator;

    // Decompression rewrites index field names, so the objects will not match.
    EXPECT_NE(0, comparator.compare(decompressed.value(), bucketWithDuplicateIndexFieldNames));

    // Check that we have 4 measurements in the decompressed bucket.
    EXPECT_EQ(4,
              decompressed->getObjectField(timeseries::kBucketDataFieldName)
                  .getObjectField(timeFieldName)
                  .nFields());

    for (const auto columnIt : decompressed->getObjectField(timeseries::kBucketDataFieldName)) {
        assertNoDuplicateIndexFieldNames(columnIt.Obj());
    }
}

TEST(TimeseriesBucketCompression, IgnoresExistingControlCountOnUncompressedBucket) {
    // An uncompressed bucket has no legal 'control.count', but a direct bucket write that bypasses
    // the strict bucket validator can stuff a value in there. Compression must discard it and
    // derive the count from the measurements, rather than inheriting or duplicating it.
    BSONObjBuilder bucketBuilder;
    for (const auto& elem : sampleBucket) {
        if (elem.fieldNameStringData() != timeseries::kBucketControlFieldName) {
            bucketBuilder.append(elem);
            continue;
        }
        BSONObjBuilder controlBuilder(
            bucketBuilder.subobjStart(timeseries::kBucketControlFieldName));
        controlBuilder.appendElements(elem.Obj());
        controlBuilder.append(timeseries::kBucketControlCountFieldName, 999);
    }

    auto compressed =
        timeseries::compressBucket(bucketBuilder.obj(),
                                   "t"sv,
                                   NamespaceString::createNamespaceString_forTest("test.foo"),
                                   true);
    ASSERT_TRUE(compressed.compressedBucket.has_value());

    // Exactly one 'count' field, holding the number of measurements actually in the bucket.
    const BSONObj control =
        compressed.compressedBucket->getObjectField(timeseries::kBucketControlFieldName);
    size_t numCountFields = 0;
    for (const auto& controlField : control) {
        if (controlField.fieldNameStringData() == timeseries::kBucketControlCountFieldName) {
            ++numCountFields;
        }
    }
    EXPECT_EQ(1, numCountFields);
    EXPECT_EQ(5, control[timeseries::kBucketControlCountFieldName].Int());
}

TEST(TimeseriesBucketCompression, CannotDecompressUncompressedBucket) {
    auto decompressed = timeseries::decompressBucket(sampleBucket);
    EXPECT_FALSE(decompressed.has_value());
}

TEST(TimeseriesBucketCompression, CompressAlreadyCompressedBucket) {
    // Compressing an already compressed bucket is a noop, should return the same compressed bucket
    // untouched.
    auto compressed = timeseries::compressBucket(
        sampleBucket, "t"sv, NamespaceString::createNamespaceString_forTest("test.foo"), false);
    ASSERT_TRUE(compressed.compressedBucket.has_value());
    auto res =
        timeseries::compressBucket(*compressed.compressedBucket,
                                   "t"sv,
                                   NamespaceString::createNamespaceString_forTest("test.foo"),
                                   false);
    ASSERT_TRUE(res.compressedBucket.has_value());
    EXPECT_EQ(compressed.compressedBucket->objsize(), res.compressedBucket->objsize());
    EXPECT_EQ(memcmp(compressed.compressedBucket->objdata(),
                     res.compressedBucket->objdata(),
                     compressed.compressedBucket->objsize()),
              0);
}

}  // namespace
}  // namespace mongo
