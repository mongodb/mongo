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

#include "mongo/db/timeseries/bucket_compression.h"

#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional/optional.hpp>

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
        ASSERT_EQ(std::to_string(curIdx++), elemIt.fieldName());
    }
}

TEST(TimeseriesBucketCompression, BasicRoundtrip) {
    auto compressed = timeseries::compressBucket(
        sampleBucket, "t"_sd, NamespaceString::createNamespaceString_forTest("test.foo"), false);
    ASSERT_TRUE(compressed.compressedBucket.has_value());
    auto decompressed = timeseries::decompressBucket(compressed.compressedBucket.value());
    ASSERT_TRUE(decompressed.has_value());

    // Compression will re-order data fields, moving the timeField to the front.
    UnorderedFieldsBSONObjComparator comparator;
    ASSERT_EQ(0, comparator.compare(decompressed.value(), sampleBucket));
}

TEST(TimeseriesBucketCompression, RoundtripWithDuplicateIndexFieldNames) {
    const StringData timeFieldName("t");
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
    ASSERT_NE(0, comparator.compare(decompressed.value(), bucketWithDuplicateIndexFieldNames));

    // Check that we have 4 measurements in the decompressed bucket.
    ASSERT_EQ(4,
              decompressed->getObjectField(timeseries::kBucketDataFieldName)
                  .getObjectField(timeFieldName)
                  .nFields());

    for (const auto columnIt : decompressed->getObjectField(timeseries::kBucketDataFieldName)) {
        assertNoDuplicateIndexFieldNames(columnIt.Obj());
    }
}

TEST(TimeseriesBucketCompression, CannotDecompressUncompressedBucket) {
    auto decompressed = timeseries::decompressBucket(sampleBucket);
    ASSERT_FALSE(decompressed.has_value());
}

TEST(TimeseriesBucketCompression, CompressAlreadyCompressedBucket) {
    // Compressing an already compressed bucket is a noop, should return the same compressed bucket
    // untouched.
    auto compressed = timeseries::compressBucket(
        sampleBucket, "t"_sd, NamespaceString::createNamespaceString_forTest("test.foo"), false);
    ASSERT_TRUE(compressed.compressedBucket.has_value());
    auto res =
        timeseries::compressBucket(*compressed.compressedBucket,
                                   "t"_sd,
                                   NamespaceString::createNamespaceString_forTest("test.foo"),
                                   false);
    ASSERT_TRUE(res.compressedBucket.has_value());
    ASSERT_EQ(compressed.compressedBucket->objsize(), res.compressedBucket->objsize());
    ASSERT_EQ(memcmp(compressed.compressedBucket->objdata(),
                     res.compressedBucket->objdata(),
                     compressed.compressedBucket->objsize()),
              0);
}

}  // namespace
}  // namespace mongo
