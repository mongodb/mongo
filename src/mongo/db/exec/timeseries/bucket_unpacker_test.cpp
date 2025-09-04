/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/exec/timeseries/bucket_unpacker.h"

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/column/bsoncolumnbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/query/timeseries/bucket_spec.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <iterator>
#include <set>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

using timeseries::BucketSpec;
using timeseries::BucketUnpacker;

constexpr auto kUserDefinedTimeName = "time"_sd;
constexpr auto kUserDefinedMetaName = "myMeta"_sd;

/**
 * A fixture to test the BucketUnpacker
 */
class BucketUnpackerTest : public mongo::unittest::Test {
public:
    // First exponent for 10^exp that would go over the BSON max limit when filled with Timestamp
    // fields. See BucketUnpackerV1::kTimestampObjSizeTable for more details
    static constexpr int kBSONSizeExceeded10PowerExponentTimeFields = 6;

    /**
     * Makes a fresh BucketUnpacker, resets it to unpack the given 'bucket', and then returns it
     * before actually doing any unpacking.
     */
    BucketUnpacker makeBucketUnpacker(std::set<std::string> fields,
                                      BucketSpec::Behavior behavior,
                                      BSONObj bucket,
                                      boost::optional<std::string> metaFieldName = boost::none) {
        auto spec = BucketSpec{
            std::string{kUserDefinedTimeName}, metaFieldName, std::move(fields), behavior};
        BucketUnpacker unpacker{std::move(spec)};
        unpacker.reset(std::move(bucket));
        return unpacker;
    }

    /**
     * Constructs a 'BucketUnpacker' based on the provided parameters and then resets it to unpack
     * the given 'bucket'. Asserts that 'reset()' throws the given 'errorCode'.
     */
    void assertUnpackerThrowsCode(std::set<std::string> fields,
                                  BucketSpec::Behavior behavior,
                                  BSONObj bucket,
                                  boost::optional<std::string> metaFieldName,
                                  int errorCode) {
        auto spec = BucketSpec{
            std::string{kUserDefinedTimeName}, metaFieldName, std::move(fields), behavior};
        BucketUnpacker unpacker{std::move(spec)};
        ASSERT_THROWS_CODE(unpacker.reset(std::move(bucket)), AssertionException, errorCode);
    }

    void assertGetNext(BucketUnpacker& unpacker, const Document& expected) {
        ASSERT_DOCUMENT_EQ(unpacker.getNext(), expected);
    }

    void assertGetNextBson(BucketUnpacker& unpacker, BSONObj expected) {
        ASSERT_BSONOBJ_EQ(unpacker.getNextBson(), expected);
    }

    std::pair<BSONObj, StringData> buildUncompressedBucketForMeasurementCount(int num) {
        BSONObjBuilder root;
        {
            BSONObjBuilder builder(root.subobjStart("control"_sd));
            builder.append("version"_sd, timeseries::kTimeseriesControlUncompressedVersion);
        }
        {
            BSONObjBuilder data(root.subobjStart("data"_sd));
            {
                DecimalCounter<uint32_t> fieldNameCounter;
                BSONObjBuilder builder(data.subobjStart("time"_sd));
                for (int i = 0; i < num; ++i, ++fieldNameCounter) {
                    builder.append(fieldNameCounter, Date_t::now());
                }
            }
        }
        BSONObj obj = root.obj();
        return {obj, "time"_sd};
    }

    // Modifies the 'control.count' field for a v2 compressed bucket. Zero delta removes the
    // 'control.count' field, positive increases the count and negative delta decreases the count.
    BSONObj modifyCompressedBucketElementCount(BSONObj compressedBucket, int delta) {
        BSONObjBuilder root;
        for (auto&& elem : compressedBucket) {
            if (elem.fieldNameStringData() != "control"_sd) {
                root.append(elem);
                continue;
            }

            BSONObjBuilder controlBuilder(root.subobjStart("control"_sd));
            for (auto&& controlElem : elem.Obj()) {
                if (controlElem.fieldNameStringData() != "count"_sd) {
                    controlBuilder.append(controlElem);
                    continue;
                }

                if (delta != 0) {
                    int count = controlElem.Number();
                    controlBuilder.append("count"_sd, count + delta);
                }
            }
        }
        return root.obj();
    }

    // Modifies the 'data.<fieldName>' field for a v2 compressed bucket. Rebuilds the compressed
    // column with the last element removed.
    BSONObj modifyCompressedBucketRemoveLastInField(BSONObj compressedBucket,
                                                    StringData fieldName) {
        BSONObjBuilder root;
        for (auto&& elem : compressedBucket) {
            if (elem.fieldNameStringData() != "data"_sd) {
                root.append(elem);
                continue;
            }

            BSONObjBuilder dataBuilder(root.subobjStart("data"_sd));
            for (auto&& dataElem : elem.Obj()) {
                if (dataElem.fieldNameStringData() != fieldName) {
                    dataBuilder.append(dataElem);
                    continue;
                }

                BSONColumn col(dataElem);
                int num = std::distance(col.begin(), col.end()) - 1;
                ASSERT(num >= 0);

                BSONColumnBuilder builder;
                auto it = col.begin();
                for (int i = 0; i < num; ++i) {
                    auto elem = *it;
                    builder.append(elem);
                    ++it;
                }

                dataBuilder.append(fieldName, builder.finalize());
            }
        }
        return root.obj();
    }
};

TEST_F(BucketUnpackerTest, UnpackBasicIncludeAllMeasurementFields) {
    std::set<std::string> fields{
        "_id", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}, "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':Date(1), '1':Date(2)}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketSpec::Behavior::kInclude,
                                       std::move(bucket),
                                       std::string{kUserDefinedMetaName});

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNext(unpacker,
                  Document{fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")});

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNext(
        unpacker,
        Document{fromjson("{time: Date(2), myMeta: {m1: 999, m2: 9999}, _id: 2, a :2, b: 1}")});
    ASSERT_FALSE(unpacker.hasNext());
}

TEST_F(BucketUnpackerTest, ExcludeASingleField) {
    std::set<std::string> fields{"b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':Date(1), '1':Date(2)}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketSpec::Behavior::kExclude,
                                           std::move(bucket),
                                           std::string{kUserDefinedMetaName});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(
            unpacker,
            Document{fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(
            unpacker,
            Document{fromjson("{time: Date(2), myMeta: {m1: 999, m2: 9999}, _id: 2, a: 2}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, EmptyIncludeGetsEmptyMeasurements) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':Date(1), '1':Date(2)}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketSpec::Behavior::kInclude,
                                           std::move(bucket),
                                           std::string{kUserDefinedMetaName});

        // We should produce empty documents, one per measurement in the bucket.
        for (auto idx = 0; idx < 2; ++idx) {
            ASSERT_TRUE(unpacker.hasNext());
            assertGetNext(unpacker, Document(fromjson("{}")));
        }
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, EmptyExcludeMaterializesAllFields) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':Date(1), '1':Date(2)}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketSpec::Behavior::kExclude,
                                           std::move(bucket),
                                           std::string{kUserDefinedMetaName});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(
            unpacker,
            Document{fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(
            unpacker,
            Document{fromjson("{time: Date(2), myMeta: {m1: 999, m2: 9999}, _id: 2, a :2, b: 1}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, SparseColumnsWhereOneColumnIsExhaustedBeforeTheOther) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':Date(1), '1':Date(2)}, "
        "a:{'0':1}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketSpec::Behavior::kExclude,
                                           std::move(bucket),
                                           std::string{kUserDefinedMetaName});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(
            unpacker,
            Document{fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(
            unpacker,
            Document{fromjson("{time: Date(2), myMeta: {m1: 999, m2: 9999}, _id: 2, b: 1}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, UnpackBasicIncludeWithDollarPrefix) {
    std::set<std::string> fields{
        "_id", "$a", "b", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':Date(1), '1':Date(2)}, "
        "$a:{'0':1, '1':2}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketSpec::Behavior::kInclude,
                                           std::move(bucket),
                                           std::string{kUserDefinedMetaName});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(
            unpacker,
            Document{fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1, $a: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson(
                          "{time: Date(2), myMeta: {m1: 999, m2: 9999}, _id: 2, $a: 2, b: 1}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, BucketsWithMetadataOnly) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':Date(1), '1':Date(2)}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketSpec::Behavior::kExclude,
                                           std::move(bucket),
                                           std::string{kUserDefinedMetaName});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1}")});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: Date(2), myMeta: {m1: 999, m2: 9999}, _id: 2}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, UnorderedRowKeysDoesntAffectMaterialization) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'1':1, '0':2, '2': "
        "3}, time: {'1':Date(1), '0': Date(2), "
        "'2': Date(3)}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketSpec::Behavior::kExclude,
                                           std::move(bucket),
                                           std::string{kUserDefinedMetaName});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: Date(2), myMeta: {m1: 999, m2: 9999}, _id: 2}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: Date(3), myMeta: {m1: 999, m2: 9999}, _id: 3}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    // bucket compressor does not handle unordered row keys
}

TEST_F(BucketUnpackerTest, MissingMetaFieldDoesntMaterializeMetadata) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, data: {_id: {'0':1, '1':2, '2': 3}, time: {'0':Date(1), '1': "
        "Date(2), '2': "
        "Date(3)}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketSpec::Behavior::kExclude,
                                           std::move(bucket),
                                           std::string{kUserDefinedMetaName});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(1), _id: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(2), _id: 2}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(3), _id: 3}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, MissingMetaFieldDoesntMaterializeMetadataUnorderedKeys) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, data: {_id: {'1':1, '0':2, '2': 3}, time: {'1':Date(1), '0': "
        "Date(2), '2': "
        "Date(3)}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketSpec::Behavior::kExclude,
                                           std::move(bucket),
                                           std::string{kUserDefinedMetaName});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(1), _id: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(2), _id: 2}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(3), _id: 3}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    // bucket compressor does not handle unordered row keys
}

TEST_F(BucketUnpackerTest, ExcludedMetaFieldDoesntMaterializeMetadataWhenBucketHasMeta) {
    std::set<std::string> fields{std::string{kUserDefinedMetaName}};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2, '2': "
        "3}, time: {'0':Date(1), '1': Date(2), "
        "'2': Date(3)}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketSpec::Behavior::kExclude,
                                           std::move(bucket),
                                           std::string{kUserDefinedMetaName});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(1), _id: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(2), _id: 2}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(3), _id: 3}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, UnpackerResetThrowsOnUndefinedMeta) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: undefined, data: {_id: {'0':1, '1':2, '2': 3}, time: "
        "{'0':Date(1), '1': Date(2), '2': Date(3)}}}");

    auto test = [&](BSONObj bucket) {
        assertUnpackerThrowsCode(fields,
                                 BucketSpec::Behavior::kExclude,
                                 std::move(bucket),
                                 std::string{kUserDefinedMetaName},
                                 5369600);
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, UnpackerResetThrowsOnUnexpectedMeta) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2, '2': "
        "3}, time: {'0':Date(1), '1': Date(2), "
        "'2': Date(3)}}}");

    auto test = [&](BSONObj bucket) {
        assertUnpackerThrowsCode(fields,
                                 BucketSpec::Behavior::kExclude,
                                 std::move(bucket),
                                 boost::none /* no metaField provided */,
                                 5369601);
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, NullMetaInBucketMaterializesAsNull) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: null, data: {_id: {'0':4, '1':5, '2':6}, time: "
        "{'0':Date(4), "
        "'1': Date(5), '2': Date(6)}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketSpec::Behavior::kExclude,
                                           std::move(bucket),
                                           std::string{kUserDefinedMetaName});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(4), myMeta: null, _id: 4}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(5), myMeta: null, _id: 5}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(6), myMeta: null, _id: 6}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, GetNextHandlesMissingMetaInBucket) {
    std::set<std::string> fields{};

    auto bucket = fromjson(R"(
{
    control: {version: 1},
    data: {
        _id: {'0':4, '1':5, '2':6},
        time: {'0':Date(4), '1': Date(5), '2': Date(6)}
    }
})");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketSpec::Behavior::kExclude,
                                           std::move(bucket),
                                           std::string{kUserDefinedMetaName});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(4), _id: 4}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(5), _id: 5}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: Date(6), _id: 6}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, EmptyDataRegionInBucketIsTolerated) {
    std::set<std::string> fields{};

    auto bucket = Document{
        {"_id", 1},
        {"control", Document{{"version", 1}}},
        {"meta", Document{{"m1", 999}, {"m2", 9999}}},
        {"data",
         Document{}}}.toBson();

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(
            fields, BucketSpec::Behavior::kExclude, bucket, std::string{kUserDefinedMetaName});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
}

TEST_F(BucketUnpackerTest, UnpackerResetThrowsOnEmptyBucket) {
    std::set<std::string> fields{};

    auto bucket = Document{};
    assertUnpackerThrowsCode(std::move(fields),
                             BucketSpec::Behavior::kExclude,
                             bucket.toBson(),
                             std::string{kUserDefinedMetaName},
                             5346510);
}

TEST_F(BucketUnpackerTest, EraseMetaFromFieldSetAndDetermineIncludeMeta) {
    // Tests a missing 'metaField' in the spec.
    std::set<std::string> empFields{};
    auto bucket = fromjson(R"(
{
    control: {version: 1},
    data: {
        _id: {'0':4, '1':5, '2':6},
        time: {'0':Date(4), '1': Date(5), '2': Date(6)}
    }
})");
    auto unpacker = makeBucketUnpacker(empFields,
                                       BucketSpec::Behavior::kInclude,
                                       std::move(bucket),
                                       std::string{kUserDefinedMetaName});

    // Tests a spec with 'metaField' in include list.
    std::set<std::string> fields{std::string{kUserDefinedMetaName}};

    auto specWithMetaInclude = BucketSpec{std::string{kUserDefinedTimeName},
                                          std::string{kUserDefinedMetaName},
                                          std::move(fields),
                                          BucketSpec::Behavior::kInclude};
    // This calls eraseMetaFromFieldSetAndDetermineIncludeMeta.
    unpacker.setBucketSpec(std::move(specWithMetaInclude));
    ASSERT_TRUE(unpacker.includeMetaField());
    ASSERT_EQ(unpacker.bucketSpec().fieldSet().count(std::string{kUserDefinedMetaName}), 0);

    std::set<std::string> fieldsNoMetaInclude{"foo"};
    auto specWithFooInclude = BucketSpec{std::string{kUserDefinedTimeName},
                                         std::string{kUserDefinedMetaName},
                                         std::move(fieldsNoMetaInclude),
                                         BucketSpec::Behavior::kInclude};

    std::set<std::string> fieldsNoMetaExclude{"foo"};
    auto specWithFooExclude = BucketSpec{std::string{kUserDefinedTimeName},
                                         std::string{kUserDefinedMetaName},
                                         std::move(fieldsNoMetaExclude),
                                         BucketSpec::Behavior::kExclude};

    unpacker.setBucketSpec(std::move(specWithFooExclude));
    ASSERT_TRUE(unpacker.includeMetaField());
    unpacker.setBucketSpec(std::move(specWithFooInclude));
    ASSERT_FALSE(unpacker.includeMetaField());

    // Tests a spec with 'metaField' not in exclude list.
    std::set<std::string> excludeFields{};
    auto specMetaExclude = BucketSpec{std::string{kUserDefinedTimeName},
                                      std::string{kUserDefinedMetaName},
                                      std::move(excludeFields),
                                      BucketSpec::Behavior::kExclude};

    auto specMetaInclude = specMetaExclude;
    specMetaInclude.setBehavior(BucketSpec::Behavior::kInclude);

    unpacker.setBucketSpec(std::move(specMetaExclude));
    ASSERT_TRUE(unpacker.includeMetaField());
    unpacker.setBucketSpec(std::move(specMetaInclude));
    ASSERT_FALSE(unpacker.includeMetaField());
}

TEST_F(BucketUnpackerTest, EraseUnneededComputedMetaProjFieldsWithInclusiveProject) {
    auto bucket = fromjson(R"(
{
    control: {version: 1},
    data: {
        _id: {'0':4, '1':5, '2':6},
        time: {'0':Date(4), '1': Date(5), '2': Date(6)}
    }
})");
    std::set<std::string> unpackerFields{std::string{kUserDefinedTimeName}};
    auto unpacker = makeBucketUnpacker(unpackerFields,
                                       BucketSpec::Behavior::kInclude,
                                       std::move(bucket),
                                       std::string{kUserDefinedMetaName});

    // Add fields to '_computedMetaProjFields'.
    unpacker.addComputedMetaProjFields({"hello"_sd, "bye"_sd});
    ASSERT_TRUE(unpacker.bucketSpec().computedMetaProjFields().contains("hello"));
    ASSERT_TRUE(unpacker.bucketSpec().computedMetaProjFields().contains("bye"));

    auto spec = unpacker.bucketSpec();
    std::set<std::string> includeFields{std::string{kUserDefinedTimeName}, "bye"};
    spec.setFieldSet(std::move(includeFields));
    spec.setBehavior(BucketSpec::Behavior::kInclude);

    // This calls eraseUnneededComputedMetaProjFields().
    unpacker.setBucketSpec(std::move(spec));
    // As "hello" was not in the includes, it should be removed.
    ASSERT_FALSE(unpacker.bucketSpec().computedMetaProjFields().contains("hello"));
    // As "bye" was in the includes, it should still be in '_computedMetaProjFields'.
    ASSERT_TRUE(unpacker.bucketSpec().computedMetaProjFields().contains("bye"));
}

TEST_F(BucketUnpackerTest, EraseUnneededComputedMetaProjFieldsWithExclusiveProject) {
    auto bucket = fromjson(R"(
{
    control: {version: 1},
    data: {
        _id: {'0':4, '1':5, '2':6},
        time: {'0':Date(4), '1': Date(5), '2': Date(6)}
    }
})");
    std::set<std::string> unpackerFields{std::string{kUserDefinedTimeName}};
    auto unpacker = makeBucketUnpacker(unpackerFields,
                                       BucketSpec::Behavior::kInclude,
                                       std::move(bucket),
                                       std::string{kUserDefinedMetaName});

    // Add fields to '_computedMetaProjFields'.
    unpacker.addComputedMetaProjFields({"hello"_sd, "bye"_sd});
    ASSERT_TRUE(unpacker.bucketSpec().computedMetaProjFields().contains("hello"));
    ASSERT_TRUE(unpacker.bucketSpec().computedMetaProjFields().contains("bye"));

    auto spec = unpacker.bucketSpec();
    std::set<std::string> excludeFields{std::string{kUserDefinedTimeName}, "bye"};
    spec.setFieldSet(std::move(excludeFields));
    spec.setBehavior(BucketSpec::Behavior::kExclude);

    // This calls eraseUnneededComputedMetaProjFields().
    unpacker.setBucketSpec(std::move(spec));
    // As "hello" was not excluded, it should still exist.
    ASSERT_TRUE(unpacker.bucketSpec().computedMetaProjFields().contains("hello"));
    // As "bye" was in the excludes, it should be removed from '_computedMetaProjFields'.
    ASSERT_FALSE(unpacker.bucketSpec().computedMetaProjFields().contains("bye"));
}

TEST_F(BucketUnpackerTest, DetermineIncludeTimeField) {
    auto bucket = fromjson(R"(
{
    control: {version: 1},
    data: {
        _id: {'0':4, '1':5, '2':6},
        time: {'0':Date(4), '1': Date(5), '2': Date(6)}
    }
})");
    std::set<std::string> unpackerFields{std::string{kUserDefinedTimeName}};
    auto unpacker = makeBucketUnpacker(unpackerFields,
                                       BucketSpec::Behavior::kInclude,
                                       std::move(bucket),
                                       std::string{kUserDefinedMetaName});

    std::set<std::string> includeFields{std::string{kUserDefinedTimeName}};
    auto includeSpec = BucketSpec{std::string{kUserDefinedTimeName},
                                  std::string{kUserDefinedMetaName},
                                  std::move(includeFields),
                                  BucketSpec::Behavior::kInclude};
    // This calls determineIncludeTimeField.
    unpacker.setBucketSpec(std::move(includeSpec));
    ASSERT_TRUE(unpacker.includeTimeField());

    std::set<std::string> excludeFields{std::string{kUserDefinedTimeName}};
    auto excludeSpec = BucketSpec{std::string{kUserDefinedTimeName},
                                  std::string{kUserDefinedMetaName},
                                  std::move(excludeFields),
                                  BucketSpec::Behavior::kExclude};
    unpacker.setBucketSpec(std::move(excludeSpec));
    ASSERT_FALSE(unpacker.includeTimeField());
}

TEST_F(BucketUnpackerTest, DetermineIncludeFieldIncludeMode) {
    std::string includedMeasurementField = "measurementField1";
    std::string excludedMeasurementField = "measurementField2";
    std::set<std::string> fields{std::string{kUserDefinedTimeName}, includedMeasurementField};

    auto bucket = Document{
        {"_id", 1},
        {"control", Document{{"version", 1}}},
        {"meta", Document{{"m1", 999}, {"m2", 9999}}},
        {"data",
         Document{}}}.toBson();

    auto spec = BucketSpec{std::string{kUserDefinedTimeName},
                           std::string{kUserDefinedMetaName},
                           std::move(fields),
                           BucketSpec::Behavior::kInclude};

    BucketUnpacker includeUnpacker;
    includeUnpacker.setBucketSpec(std::move(spec));
    // Need to call reset so that the private method calculateFieldsToIncludeExcludeDuringUnpack()
    // is called, and _unpackFieldsToIncludeExclude gets filled with fields.
    includeUnpacker.reset(std::move(bucket));
    // Now the spec knows which fields to include/exclude.

    ASSERT_TRUE(determineIncludeField(kUserDefinedTimeName,
                                      BucketSpec::Behavior::kInclude,
                                      includeUnpacker.fieldsToIncludeExcludeDuringUnpack()));
    ASSERT_TRUE(determineIncludeField(includedMeasurementField,
                                      BucketSpec::Behavior::kInclude,
                                      includeUnpacker.fieldsToIncludeExcludeDuringUnpack()));
    ASSERT_FALSE(determineIncludeField(excludedMeasurementField,
                                       BucketSpec::Behavior::kInclude,
                                       includeUnpacker.fieldsToIncludeExcludeDuringUnpack()));
}

TEST_F(BucketUnpackerTest, DetermineIncludeFieldExcludeMode) {
    std::string includedMeasurementField = "measurementField1";
    std::string excludedMeasurementField = "measurementField2";
    std::set<std::string> fields{std::string{kUserDefinedTimeName}, includedMeasurementField};

    auto bucket = Document{
        {"_id", 1},
        {"control", Document{{"version", 1}}},
        {"meta", Document{{"m1", 999}, {"m2", 9999}}},
        {"data",
         Document{}}}.toBson();

    auto spec = BucketSpec{std::string{kUserDefinedTimeName},
                           std::string{kUserDefinedMetaName},
                           std::move(fields),
                           BucketSpec::Behavior::kExclude};

    BucketUnpacker excludeUnpacker;
    excludeUnpacker.setBucketSpec(std::move(spec));
    excludeUnpacker.reset(std::move(bucket));

    ASSERT_FALSE(determineIncludeField(kUserDefinedTimeName,
                                       BucketSpec::Behavior::kExclude,
                                       excludeUnpacker.fieldsToIncludeExcludeDuringUnpack()));
    ASSERT_FALSE(determineIncludeField(includedMeasurementField,
                                       BucketSpec::Behavior::kExclude,
                                       excludeUnpacker.fieldsToIncludeExcludeDuringUnpack()));
    ASSERT_TRUE(determineIncludeField(excludedMeasurementField,
                                      BucketSpec::Behavior::kExclude,
                                      excludeUnpacker.fieldsToIncludeExcludeDuringUnpack()));
}

/**
 * Manually computes the timestamp object size for n timestamps.
 */
auto expectedTimestampObjSize(int32_t rowKeyOffset, int32_t n) {
    BSONObjBuilder bob;
    for (auto i = 0; i < n; ++i) {
        bob.appendDate(std::to_string(i + rowKeyOffset), Date_t::now());
    }
    return bob.done().objsize();
}

TEST_F(BucketUnpackerTest, ExtractSingleMeasurement) {
    std::set<std::string> fields{
        "_id", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}, "a", "b"};
    auto spec = BucketSpec{std::string{kUserDefinedTimeName},
                           std::string{kUserDefinedMetaName},
                           std::move(fields),
                           BucketSpec::Behavior::kInclude};
    auto unpacker = BucketUnpacker{std::move(spec)};

    auto d1 = dateFromISOString("2020-02-17T00:00:00.000Z").getValue();
    auto d2 = dateFromISOString("2020-02-17T01:00:00.000Z").getValue();
    auto d3 = dateFromISOString("2020-02-17T02:00:00.000Z").getValue();
    auto bucket = BSON("control" << BSON("version" << 1) << "meta"
                                 << BSON("m1" << 999 << "m2" << 9999) << "data"
                                 << BSON("_id" << BSON("0" << 1 << "1" << 2 << "2" << 3) << "time"
                                               << BSON("0" << d1 << "1" << d2 << "2" << d3) << "a"
                                               << BSON("0" << 1 << "1" << 2 << "2" << 3) << "b"
                                               << BSON("1" << 1 << "2" << 2)));

    unpacker.reset(std::move(bucket));

    auto next = unpacker.extractSingleMeasurement(0);
    auto expected = Document{
        {"myMeta", Document{{"m1", 999}, {"m2", 9999}}}, {"_id", 1}, {"time", d1}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(next, expected);

    next = unpacker.extractSingleMeasurement(2);
    expected = Document{{"myMeta", Document{{"m1", 999}, {"m2", 9999}}},
                        {"_id", 3},
                        {"time", d3},
                        {"a", 3},
                        {"b", 2}};
    ASSERT_DOCUMENT_EQ(next, expected);

    next = unpacker.extractSingleMeasurement(1);
    expected = Document{{"myMeta", Document{{"m1", 999}, {"m2", 9999}}},
                        {"_id", 2},
                        {"time", d2},
                        {"a", 2},
                        {"b", 1}};
    ASSERT_DOCUMENT_EQ(next, expected);

    // Can we extract the middle element again?
    next = unpacker.extractSingleMeasurement(1);
    ASSERT_DOCUMENT_EQ(next, expected);
}

TEST_F(BucketUnpackerTest, ExtractSingleMeasurementSparse) {
    std::set<std::string> fields{
        "_id", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}, "a", "b"};
    auto spec = BucketSpec{std::string{kUserDefinedTimeName},
                           std::string{kUserDefinedMetaName},
                           std::move(fields),
                           BucketSpec::Behavior::kInclude};
    auto unpacker = BucketUnpacker{std::move(spec)};

    auto d1 = dateFromISOString("2020-02-17T00:00:00.000Z").getValue();
    auto d2 = dateFromISOString("2020-02-17T01:00:00.000Z").getValue();
    auto bucket = BSON("control" << BSON("version" << 1) << "meta"
                                 << BSON("m1" << 999 << "m2" << 9999) << "data"
                                 << BSON("_id" << BSON("0" << 1 << "1" << 2) << "time"
                                               << BSON("0" << d1 << "1" << d2) << "a"
                                               << BSON("0" << 1) << "b" << BSON("1" << 1)));

    unpacker.reset(std::move(bucket));
    auto next = unpacker.extractSingleMeasurement(1);
    auto expected = Document{
        {"myMeta", Document{{"m1", 999}, {"m2", 9999}}}, {"_id", 2}, {"time", d2}, {"b", 1}};
    ASSERT_DOCUMENT_EQ(next, expected);

    // Can we extract the same element again?
    next = unpacker.extractSingleMeasurement(1);
    ASSERT_DOCUMENT_EQ(next, expected);

    next = unpacker.extractSingleMeasurement(0);
    expected = Document{
        {"myMeta", Document{{"m1", 999}, {"m2", 9999}}}, {"_id", 1}, {"time", d1}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(next, expected);

    // Can we extract the same element twice in a row?
    next = unpacker.extractSingleMeasurement(0);
    ASSERT_DOCUMENT_EQ(next, expected);

    next = unpacker.extractSingleMeasurement(0);
    ASSERT_DOCUMENT_EQ(next, expected);
}

TEST_F(BucketUnpackerTest, SimpleGetNextBson) {
    // We use array representation for '_id', 'time', and 'a' but can't use array representation for
    // 'b' since it doesn't exist for the first document.
    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: [1,2], "
        "time: [Date(1), Date(2)], "
        "a:[1,2], b:{'1':1}}}");

    // The compressed bucket is now a v2 bucket.
    auto compressedBucket =
        timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket;

    auto bson0 = fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}");
    auto bson1 = fromjson("{time: Date(2), myMeta: {m1: 999, m2: 9999}, _id: 2, a :2, b: 1}");

    for (auto& bucket : {bucket, *compressedBucket}) {
        std::set<std::string> fields{
            "_id", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}, "a", "b"};
        auto unpacker = makeBucketUnpacker(std::move(fields),
                                           BucketSpec::Behavior::kInclude,
                                           std::move(bucket),
                                           std::string{kUserDefinedMetaName});

        ASSERT_EQ(unpacker.numberOfMeasurements(), 2);
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNextBson(unpacker, bson0);
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNextBson(unpacker, bson1);
        ASSERT_FALSE(unpacker.hasNext());
    }
}

TEST_F(BucketUnpackerTest, SimpleGetNextBsonOnUncompressedBucket) {
    std::set<std::string> fields{
        "_id", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}, "a", "b"};

    // We use array representation for '_id', 'time', and 'a' but can't use array representation for
    // 'b' since it doesn't exist for the first document.
    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: [1,2], "
        "time: [Date(1), Date(2)], "
        "a:[1,2], b:{'1':1}}}");

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketSpec::Behavior::kInclude,
                                       std::move(bucket),
                                       std::string{kUserDefinedMetaName});

    auto bson0 = fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}");
    auto bson1 = fromjson("{time: Date(2), myMeta: {m1: 999, m2: 9999}, _id: 2, a :2, b: 1}");

    ASSERT_EQ(unpacker.numberOfMeasurements(), 2);

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNextBson(unpacker, bson0);

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNextBson(unpacker, bson1);

    ASSERT_FALSE(unpacker.hasNext());
}

DEATH_TEST_REGEX_F(BucketUnpackerTest, GetNextBsonWhenExhausted, "Tripwire assertion.*7026801") {
    std::set<std::string> fields{
        "_id", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}, "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: [1], "
        "time: [Date(1)], "
        "a:[1]}}");

    auto compressedBucket =
        timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket;

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketSpec::Behavior::kInclude,
                                       std::move(*compressedBucket),
                                       std::string{kUserDefinedMetaName});

    auto bson0 = fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}");

    ASSERT_EQ(unpacker.numberOfMeasurements(), 1);

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNextBson(unpacker, bson0);

    ASSERT_FALSE(unpacker.hasNext());
    ASSERT_THROWS_CODE(unpacker.getNextBson(), AssertionException, 7026801);
}


DEATH_TEST_REGEX_F(BucketUnpackerTest,
                   GetNextBsonWhenMinTimeAsMetadataSet,
                   "Tripwire assertion.*7026802") {
    std::set<std::string> fields{
        "_id", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}, "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: [1], "
        "time: [Date(1)], "
        "a:[1]}}");

    auto compressedBucket =
        timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket;

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketSpec::Behavior::kInclude,
                                       std::move(*compressedBucket),
                                       std::string{kUserDefinedMetaName});

    unpacker.setIncludeMinTimeAsMetadata();

    ASSERT_TRUE(unpacker.hasNext());
    ASSERT_THROWS_CODE(unpacker.getNextBson(), AssertionException, 7026802);
}

DEATH_TEST_REGEX_F(BucketUnpackerTest,
                   GetNextBsonWhenMaxTimeAsMetadataSet,
                   "Tripwire assertion.*7026802") {
    std::set<std::string> fields{
        "_id", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}, "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: [1], "
        "time: [Date(1)], "
        "a:[1]}}");

    auto compressedBucket =
        timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket;

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketSpec::Behavior::kInclude,
                                       std::move(*compressedBucket),
                                       std::string{kUserDefinedMetaName});

    unpacker.setIncludeMaxTimeAsMetadata();

    ASSERT_TRUE(unpacker.hasNext());
    ASSERT_THROWS_CODE(unpacker.getNextBson(), AssertionException, 7026802);
}

TEST_F(BucketUnpackerTest, ComputeMeasurementCountLowerBoundsAreCorrect) {
    // Test the case when the target size hits a table entry which represents the lower bound of an
    // interval.
    for (int i = 0; i <= kBSONSizeExceeded10PowerExponentTimeFields; ++i) {
        int bucketCount = std::pow(10, i);
        auto [bucket, timeField] = buildUncompressedBucketForMeasurementCount(bucketCount);
        ASSERT_EQ(bucketCount, BucketUnpacker::computeMeasurementCount(bucket, timeField));

        auto compressedBucket =
            timeseries::compressBucket(bucket, timeField, {}, false).compressedBucket;
        ASSERT_EQ(bucketCount,
                  BucketUnpacker::computeMeasurementCount(*compressedBucket, timeField));

        // Remove the count field.
        auto modifiedCompressedBucket = modifyCompressedBucketElementCount(*compressedBucket, 0);
        ASSERT_EQ(bucketCount,
                  BucketUnpacker::computeMeasurementCount(modifiedCompressedBucket, timeField));
    }
}

TEST_F(BucketUnpackerTest, ComputeMeasurementCountUpperBoundsAreCorrect) {
    // Test the case when the target size hits a table entry which represents the upper bound of an
    // interval.
    for (int i = 1; i <= kBSONSizeExceeded10PowerExponentTimeFields; ++i) {
        int bucketCount = std::pow(10, i) - 1;
        auto [bucket, timeField] = buildUncompressedBucketForMeasurementCount(bucketCount);
        ASSERT_EQ(bucketCount, BucketUnpacker::computeMeasurementCount(bucket, timeField));
    }
}

TEST_F(BucketUnpackerTest, ComputeMeasurementCountAllPointsInSmallerIntervals) {
    // Test all values for some of the smaller intervals up to 100 measurements.
    for (auto bucketCount = 0; bucketCount < 25; ++bucketCount) {
        auto [bucket, timeField] = buildUncompressedBucketForMeasurementCount(bucketCount);
        ASSERT_EQ(bucketCount, BucketUnpacker::computeMeasurementCount(bucket, timeField));
    }
}

TEST_F(BucketUnpackerTest, ComputeMeasurementCountInLargerIntervals) {
    auto testMeasurementCount = [&](int num) {
        auto [bucket, timeField] = buildUncompressedBucketForMeasurementCount(num);
        ASSERT_EQ(num, BucketUnpacker::computeMeasurementCount(bucket, timeField));
    };

    testMeasurementCount(2222);
    testMeasurementCount(11111);
    testMeasurementCount(449998);
}

TEST_F(BucketUnpackerTest, GetNextWithMetadataFields) {
    std::set<std::string> fields{
        "_id", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}, "a", "b"};

    // The value for 'b' cannot use array representation (like '_id') since it doesn't have a value
    // for the first document.
    auto bucket = fromjson(
        "{control: {'version': 1, 'min': {'time' : Date(1)}, 'max': {'time' : Date(2)}}, meta: "
        "{'m1': 999, 'm2': 9999}, hello:{'h1': 0, 'h2': 1}, data: {_id: [1,2], "
        "time: [Date(1), Date(2)], "
        "a:[1,2], b:{'1':1}}}");

    auto spec = BucketSpec{std::string{kUserDefinedTimeName},
                           std::string{kUserDefinedMetaName},
                           std::move(fields),
                           BucketSpec::Behavior::kInclude};

    BucketUnpacker unpacker{std::move(spec)};

    unpacker.addComputedMetaProjFields({"hello"_sd});
    unpacker.setIncludeMaxTimeAsMetadata();
    unpacker.setIncludeMinTimeAsMetadata();

    // Reset bucket to update _computedMetaProjections, _minTime, and _maxTime.
    unpacker.reset(std::move(bucket));

    auto doc0 = Document{fromjson(
        "{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1,  hello: {h1: 0, h2: 1}}")};
    ASSERT_TRUE(unpacker.hasNext());
    auto document = unpacker.getNext();
    ASSERT_DOCUMENT_EQ(doc0, document);

    Date_t minDate = Date_t::fromMillisSinceEpoch(1);
    Date_t maxDate = Date_t::fromMillisSinceEpoch(2);
    ASSERT_EQ(document.metadata().getTimeseriesBucketMinTime(), minDate);
    ASSERT_EQ(document.metadata().getTimeseriesBucketMaxTime(), maxDate);
}

TEST_F(BucketUnpackerTest, TamperedCompressedCountLess) {
    std::set<std::string> fields{
        "_id", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}, "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':Date(1), '1':Date(2)}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto compressedBucket =
        timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket;
    // Reduce the count by one to be 1.
    auto modifiedCompressedBucket = modifyCompressedBucketElementCount(*compressedBucket, -1);

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketSpec::Behavior::kInclude,
                                       std::move(modifiedCompressedBucket),
                                       std::string{kUserDefinedMetaName});

    auto doc0 = Document{fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")};
    auto doc1 =
        Document{fromjson("{time: Date(2), myMeta: {m1: 999, m2: 9999}, _id: 2, a :2, b: 1}")};

    // 1 is reported when asking for numberOfMeasurements()
    ASSERT_EQ(unpacker.numberOfMeasurements(), 1);
    ASSERT_DOCUMENT_EQ(unpacker.extractSingleMeasurement(0), doc0);

    // Iterating returns both documents
    ASSERT_TRUE(unpacker.hasNext());
    assertGetNext(unpacker, doc0);

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNext(unpacker, doc1);
    ASSERT_FALSE(unpacker.hasNext());
}

TEST_F(BucketUnpackerTest, TamperedCompressedCountMore) {
    std::set<std::string> fields{
        "_id", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}, "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':Date(1), '1':Date(2)}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto compressedBucket =
        timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket;
    // Increase the count by one to be 3.
    auto modifiedCompressedBucket = modifyCompressedBucketElementCount(*compressedBucket, 1);

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketSpec::Behavior::kInclude,
                                       std::move(modifiedCompressedBucket),
                                       std::string{kUserDefinedMetaName});

    auto doc0 = Document{fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")};
    auto doc1 =
        Document{fromjson("{time: Date(2), myMeta: {m1: 999, m2: 9999}, _id: 2, a :2, b: 1}")};

    ASSERT_EQ(unpacker.numberOfMeasurements(), 3);
    ASSERT_DOCUMENT_EQ(unpacker.extractSingleMeasurement(0), doc0);
    ASSERT_DOCUMENT_EQ(unpacker.extractSingleMeasurement(1), doc1);
    ASSERT_THROWS_CODE(unpacker.extractSingleMeasurement(2), AssertionException, 6067500);

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNext(unpacker, doc0);

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNext(unpacker, doc1);
    ASSERT_FALSE(unpacker.hasNext());
}

TEST_F(BucketUnpackerTest, TamperedCompressedCountMissing) {
    std::set<std::string> fields{
        "_id", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}, "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':Date(1), '1':Date(2)}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto compressedBucket =
        timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket;
    // Remove the count field
    auto modifiedCompressedBucket = modifyCompressedBucketElementCount(*compressedBucket, 0);

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketSpec::Behavior::kInclude,
                                       std::move(modifiedCompressedBucket),
                                       std::string{kUserDefinedMetaName});

    auto doc0 = Document{fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")};
    auto doc1 =
        Document{fromjson("{time: Date(2), myMeta: {m1: 999, m2: 9999}, _id: 2, a :2, b: 1}")};

    // Missing count field will make the unpacker measure the number of time fields for an accurate
    // count
    ASSERT_EQ(unpacker.numberOfMeasurements(), 2);
    ASSERT_DOCUMENT_EQ(unpacker.extractSingleMeasurement(0), doc0);
    ASSERT_DOCUMENT_EQ(unpacker.extractSingleMeasurement(1), doc1);

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNext(unpacker, doc0);

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNext(unpacker, doc1);
    ASSERT_FALSE(unpacker.hasNext());
}

TEST_F(BucketUnpackerTest, TamperedCompressedElementMismatchDataField) {
    std::set<std::string> fields{
        "_id", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}, "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':Date(1), '1':Date(2)}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto compressedBucket =
        timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket;
    // Remove an element in the "a" field.
    auto modifiedCompressedBucket =
        modifyCompressedBucketRemoveLastInField(*compressedBucket, "a"_sd);

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketSpec::Behavior::kInclude,
                                       std::move(modifiedCompressedBucket),
                                       std::string{kUserDefinedMetaName});

    auto doc0 = Document{fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")};

    ASSERT_EQ(unpacker.numberOfMeasurements(), 2);
    ASSERT_DOCUMENT_EQ(unpacker.extractSingleMeasurement(0), doc0);
    // We will now uassert when trying to get the tampered document
    ASSERT_THROWS_CODE(unpacker.extractSingleMeasurement(1), AssertionException, 6067600);

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNext(unpacker, doc0);

    ASSERT_TRUE(unpacker.hasNext());
    // We will now uassert when trying to get the tampered document
    ASSERT_THROWS_CODE(unpacker.getNext(), AssertionException, 6067601);
}

TEST_F(BucketUnpackerTest, TamperedCompressedElementMismatchTimeField) {
    std::set<std::string> fields{
        "_id", std::string{kUserDefinedMetaName}, std::string{kUserDefinedTimeName}, "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':Date(1), '1':Date(2)}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto compressedBucket =
        timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket;
    // Remove an element in the time field
    auto modifiedCompressedBucket =
        modifyCompressedBucketRemoveLastInField(*compressedBucket, "time"_sd);

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketSpec::Behavior::kInclude,
                                       std::move(modifiedCompressedBucket),
                                       std::string{kUserDefinedMetaName});

    auto doc0 = Document{fromjson("{time: Date(1), myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")};

    ASSERT_EQ(unpacker.numberOfMeasurements(), 2);
    ASSERT_DOCUMENT_EQ(unpacker.extractSingleMeasurement(0), doc0);
    // We will now uassert when trying to get the tampered document
    ASSERT_THROWS_CODE(unpacker.extractSingleMeasurement(1), AssertionException, 6067500);

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNext(unpacker, doc0);

    // When time is modified it will look like there is no more elements when iterating
    ASSERT_FALSE(unpacker.hasNext());
}

}  // namespace
}  // namespace mongo
