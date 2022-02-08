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

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/bson/util/bsoncolumnbuilder.h"
#include "mongo/db/exec/bucket_unpacker.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

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
                                      BucketUnpacker::Behavior behavior,
                                      BSONObj bucket,
                                      boost::optional<std::string> metaFieldName = boost::none) {
        auto spec = BucketSpec{kUserDefinedTimeName.toString(), metaFieldName, std::move(fields)};

        BucketUnpacker unpacker{std::move(spec), behavior};
        unpacker.reset(std::move(bucket));
        return unpacker;
    }

    /**
     * Constructs a 'BucketUnpacker' based on the provided parameters and then resets it to unpack
     * the given 'bucket'. Asserts that 'reset()' throws the given 'errorCode'.
     */
    void assertUnpackerThrowsCode(std::set<std::string> fields,
                                  BucketUnpacker::Behavior behavior,
                                  BSONObj bucket,
                                  boost::optional<std::string> metaFieldName,
                                  int errorCode) {
        auto spec = BucketSpec{kUserDefinedTimeName.toString(), metaFieldName, std::move(fields)};
        BucketUnpacker unpacker{std::move(spec), behavior};
        ASSERT_THROWS_CODE(unpacker.reset(std::move(bucket)), AssertionException, errorCode);
    }

    void assertGetNext(BucketUnpacker& unpacker, const Document& expected) {
        ASSERT_DOCUMENT_EQ(unpacker.getNext(), expected);
    }

    std::pair<BSONObj, StringData> buildUncompressedBucketForMeasurementCount(int num) {
        BSONObjBuilder root;
        {
            BSONObjBuilder builder(root.subobjStart("control"_sd));
            builder.append("version"_sd, 1);
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

                BSONColumnBuilder builder(fieldName);
                auto it = col.begin();
                for (int i = 0; i < num; ++i) {
                    auto elem = *it;
                    if (!elem.eoo())
                        builder.append(elem);
                    else
                        builder.skip();
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
        "_id", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString(), "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketUnpacker::Behavior::kInclude,
                                       std::move(bucket),
                                       kUserDefinedMetaName.toString());

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNext(unpacker,
                  Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")});

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNext(unpacker,
                  Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, a :2, b: 1}")});
    ASSERT_FALSE(unpacker.hasNext());
}

TEST_F(BucketUnpackerTest, ExcludeASingleField) {
    std::set<std::string> fields{"b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, a: 2}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, EmptyIncludeGetsEmptyMeasurements) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kInclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());

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
        "time: {'0':1, '1':2}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(
            unpacker,
            Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, a :2, b: 1}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, SparseColumnsWhereOneColumnIsExhaustedBeforeTheOther) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "a:{'0':1}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, b: 1}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, UnpackBasicIncludeWithDollarPrefix) {
    std::set<std::string> fields{
        "_id", "$a", "b", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString()};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "$a:{'0':1, '1':2}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kInclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, $a: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(
            unpacker,
            Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, $a: 2, b: 1}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, BucketsWithMetadataOnly) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1}")});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, UnorderedRowKeysDoesntAffectMaterialization) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'1':1, '0':2, '2': "
        "3}, time: {'1':1, '0': 2, "
        "'2': 3}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 3, myMeta: {m1: 999, m2: 9999}, _id: 3}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    // bucket compressor does not handle unordered row keys
}

TEST_F(BucketUnpackerTest, MissingMetaFieldDoesntMaterializeMetadata) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, data: {_id: {'0':1, '1':2, '2': 3}, time: {'0':1, '1': 2, '2': "
        "3}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 1, _id: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 2, _id: 2}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 3, _id: 3}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, MissingMetaFieldDoesntMaterializeMetadataUnorderedKeys) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, data: {_id: {'1':1, '0':2, '2': 3}, time: {'1':1, '0': 2, '2': "
        "3}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 1, _id: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 2, _id: 2}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 3, _id: 3}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    // bucket compressor does not handle unordered row keys
}

TEST_F(BucketUnpackerTest, ExcludedMetaFieldDoesntMaterializeMetadataWhenBucketHasMeta) {
    std::set<std::string> fields{kUserDefinedMetaName.toString()};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2, '2': "
        "3}, time: {'0':1, '1': 2, "
        "'2': 3}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 1, _id: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 2, _id: 2}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 3, _id: 3}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, UnpackerResetThrowsOnUndefinedMeta) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: undefined, data: {_id: {'0':1, '1':2, '2': 3}, time: "
        "{'0':1, '1': 2, '2': 3}}}");

    auto test = [&](BSONObj bucket) {
        assertUnpackerThrowsCode(fields,
                                 BucketUnpacker::Behavior::kExclude,
                                 std::move(bucket),
                                 kUserDefinedMetaName.toString(),
                                 5369600);
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, UnpackerResetThrowsOnUnexpectedMeta) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2, '2': "
        "3}, time: {'0':1, '1': 2, "
        "'2': 3}}}");

    auto test = [&](BSONObj bucket) {
        assertUnpackerThrowsCode(fields,
                                 BucketUnpacker::Behavior::kExclude,
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
        "{control: {'version': 1}, meta: null, data: {_id: {'0':4, '1':5, '2':6}, time: {'0':4, "
        "'1': 5, '2': 6}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 4, myMeta: null, _id: 4}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 5, myMeta: null, _id: 5}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 6, myMeta: null, _id: 6}")});
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
        time: {'0':4, '1': 5, '2': 6}
    }
})");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 4, _id: 4}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 5, _id: 5}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 6, _id: 6}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(*timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket);
}

TEST_F(BucketUnpackerTest, EmptyDataRegionInBucketIsTolerated) {
    std::set<std::string> fields{};

    auto bucket = Document{{"_id", 1},
                           {"control", Document{{"version", 1}}},
                           {"meta", Document{{"m1", 999}, {"m2", 9999}}},
                           {"data", Document{}}}
                      .toBson();

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(
            fields, BucketUnpacker::Behavior::kExclude, bucket, kUserDefinedMetaName.toString());
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
}

TEST_F(BucketUnpackerTest, UnpackerResetThrowsOnEmptyBucket) {
    std::set<std::string> fields{};

    auto bucket = Document{};
    assertUnpackerThrowsCode(std::move(fields),
                             BucketUnpacker::Behavior::kExclude,
                             bucket.toBson(),
                             kUserDefinedMetaName.toString(),
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
        time: {'0':4, '1': 5, '2': 6}
    }
})");
    auto unpacker = makeBucketUnpacker(empFields,
                                       BucketUnpacker::Behavior::kInclude,
                                       std::move(bucket),
                                       kUserDefinedMetaName.toString());

    // Tests a spec with 'metaField' in include list.
    std::set<std::string> fields{kUserDefinedMetaName.toString()};

    auto specWithMetaInclude = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(fields)};
    // This calls eraseMetaFromFieldSetAndDetermineIncludeMeta.
    unpacker.setBucketSpecAndBehavior(std::move(specWithMetaInclude),
                                      BucketUnpacker::Behavior::kInclude);
    ASSERT_TRUE(unpacker.includeMetaField());
    ASSERT_EQ(unpacker.bucketSpec().fieldSet().count(kUserDefinedMetaName.toString()), 0);

    std::set<std::string> fieldsNoMetaInclude{"foo"};
    auto specWithFooInclude = BucketSpec{kUserDefinedTimeName.toString(),
                                         kUserDefinedMetaName.toString(),
                                         std::move(fieldsNoMetaInclude)};

    std::set<std::string> fieldsNoMetaExclude{"foo"};
    auto specWithFooExclude = BucketSpec{kUserDefinedTimeName.toString(),
                                         kUserDefinedMetaName.toString(),
                                         std::move(fieldsNoMetaExclude)};

    unpacker.setBucketSpecAndBehavior(std::move(specWithFooExclude),
                                      BucketUnpacker::Behavior::kExclude);
    ASSERT_TRUE(unpacker.includeMetaField());
    unpacker.setBucketSpecAndBehavior(std::move(specWithFooInclude),
                                      BucketUnpacker::Behavior::kInclude);
    ASSERT_FALSE(unpacker.includeMetaField());

    // Tests a spec with 'metaField' not in exclude list.
    std::set<std::string> excludeFields{};
    auto specMetaExclude = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(excludeFields)};
    auto specMetaInclude = specMetaExclude;
    unpacker.setBucketSpecAndBehavior(std::move(specMetaExclude),
                                      BucketUnpacker::Behavior::kExclude);
    ASSERT_TRUE(unpacker.includeMetaField());
    unpacker.setBucketSpecAndBehavior(std::move(specMetaInclude),
                                      BucketUnpacker::Behavior::kInclude);
    ASSERT_FALSE(unpacker.includeMetaField());
}

TEST_F(BucketUnpackerTest, DetermineIncludeTimeField) {
    auto bucket = fromjson(R"(
{
    control: {version: 1},
    data: {
        _id: {'0':4, '1':5, '2':6},
        time: {'0':4, '1': 5, '2': 6}
    }
})");
    std::set<std::string> unpackerFields{kUserDefinedTimeName.toString()};
    auto unpacker = makeBucketUnpacker(unpackerFields,
                                       BucketUnpacker::Behavior::kInclude,
                                       std::move(bucket),
                                       kUserDefinedMetaName.toString());

    std::set<std::string> includeFields{kUserDefinedTimeName.toString()};
    auto includeSpec = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(includeFields)};
    // This calls determineIncludeTimeField.
    unpacker.setBucketSpecAndBehavior(std::move(includeSpec), BucketUnpacker::Behavior::kInclude);
    ASSERT_TRUE(unpacker.includeTimeField());

    std::set<std::string> excludeFields{kUserDefinedTimeName.toString()};
    auto excludeSpec = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(excludeFields)};
    unpacker.setBucketSpecAndBehavior(std::move(excludeSpec), BucketUnpacker::Behavior::kExclude);
    ASSERT_FALSE(unpacker.includeTimeField());
}

TEST_F(BucketUnpackerTest, DetermineIncludeFieldIncludeMode) {
    std::string includedMeasurementField = "measurementField1";
    std::string excludedMeasurementField = "measurementField2";
    std::set<std::string> fields{kUserDefinedTimeName.toString(), includedMeasurementField};

    auto bucket = Document{{"_id", 1},
                           {"control", Document{{"version", 1}}},
                           {"meta", Document{{"m1", 999}, {"m2", 9999}}},
                           {"data", Document{}}}
                      .toBson();

    auto spec = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(fields)};

    BucketUnpacker includeUnpacker;
    includeUnpacker.setBucketSpecAndBehavior(std::move(spec), BucketUnpacker::Behavior::kInclude);
    // Need to call reset so that the private method calculateFieldsToIncludeExcludeDuringUnpack()
    // is called, and _unpackFieldsToIncludeExclude gets filled with fields.
    includeUnpacker.reset(std::move(bucket));
    // Now the spec knows which fields to include/exclude.

    ASSERT_TRUE(determineIncludeField(kUserDefinedTimeName,
                                      BucketUnpacker::Behavior::kInclude,
                                      includeUnpacker.fieldsToIncludeExcludeDuringUnpack()));
    ASSERT_TRUE(determineIncludeField(includedMeasurementField,
                                      BucketUnpacker::Behavior::kInclude,
                                      includeUnpacker.fieldsToIncludeExcludeDuringUnpack()));
    ASSERT_FALSE(determineIncludeField(excludedMeasurementField,
                                       BucketUnpacker::Behavior::kInclude,
                                       includeUnpacker.fieldsToIncludeExcludeDuringUnpack()));
}

TEST_F(BucketUnpackerTest, DetermineIncludeFieldExcludeMode) {
    std::string includedMeasurementField = "measurementField1";
    std::string excludedMeasurementField = "measurementField2";
    std::set<std::string> fields{kUserDefinedTimeName.toString(), includedMeasurementField};

    auto bucket = Document{{"_id", 1},
                           {"control", Document{{"version", 1}}},
                           {"meta", Document{{"m1", 999}, {"m2", 9999}}},
                           {"data", Document{}}}
                      .toBson();

    auto spec = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(fields)};

    BucketUnpacker excludeUnpacker;
    excludeUnpacker.setBucketSpecAndBehavior(std::move(spec), BucketUnpacker::Behavior::kExclude);
    excludeUnpacker.reset(std::move(bucket));

    ASSERT_FALSE(determineIncludeField(kUserDefinedTimeName,
                                       BucketUnpacker::Behavior::kExclude,
                                       excludeUnpacker.fieldsToIncludeExcludeDuringUnpack()));
    ASSERT_FALSE(determineIncludeField(includedMeasurementField,
                                       BucketUnpacker::Behavior::kExclude,
                                       excludeUnpacker.fieldsToIncludeExcludeDuringUnpack()));
    ASSERT_TRUE(determineIncludeField(excludedMeasurementField,
                                      BucketUnpacker::Behavior::kExclude,
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
        "_id", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString(), "a", "b"};
    auto spec = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(fields)};
    auto unpacker = BucketUnpacker{std::move(spec), BucketUnpacker::Behavior::kInclude};

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
        "_id", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString(), "a", "b"};
    auto spec = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(fields)};
    auto unpacker = BucketUnpacker{std::move(spec), BucketUnpacker::Behavior::kInclude};

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

TEST_F(BucketUnpackerTest, ComputeMeasurementCountLowerBoundsAreCorrect) {
    // Test the case when the target size hits a table entry which represents the lower bound of an
    // interval.
    for (int i = 0; i <= kBSONSizeExceeded10PowerExponentTimeFields; ++i) {
        int bucketCount = std::pow(10, i);
        auto [bucket, timeField] = buildUncompressedBucketForMeasurementCount(bucketCount);
        ASSERT_EQ(bucketCount, BucketUnpacker::computeMeasurementCount(bucket, timeField));
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

TEST_F(BucketUnpackerTest, TamperedCompressedCountLess) {
    std::set<std::string> fields{
        "_id", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString(), "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto compressedBucket =
        timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket;
    // Reduce the count by one to be 1.
    auto modifiedCompressedBucket = modifyCompressedBucketElementCount(*compressedBucket, -1);

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketUnpacker::Behavior::kInclude,
                                       std::move(modifiedCompressedBucket),
                                       kUserDefinedMetaName.toString());

    auto doc0 = Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")};
    auto doc1 = Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, a :2, b: 1}")};

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
        "_id", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString(), "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto compressedBucket =
        timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket;
    // Increase the count by one to be 3.
    auto modifiedCompressedBucket = modifyCompressedBucketElementCount(*compressedBucket, 1);

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketUnpacker::Behavior::kInclude,
                                       std::move(modifiedCompressedBucket),
                                       kUserDefinedMetaName.toString());

    auto doc0 = Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")};
    auto doc1 = Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, a :2, b: 1}")};

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
        "_id", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString(), "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto compressedBucket =
        timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket;
    // Remove the count field
    auto modifiedCompressedBucket = modifyCompressedBucketElementCount(*compressedBucket, 0);

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketUnpacker::Behavior::kInclude,
                                       std::move(modifiedCompressedBucket),
                                       kUserDefinedMetaName.toString());

    auto doc0 = Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")};
    auto doc1 = Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, a :2, b: 1}")};

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
        "_id", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString(), "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto compressedBucket =
        timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket;
    // Remove an element in the "a" field.
    auto modifiedCompressedBucket =
        modifyCompressedBucketRemoveLastInField(*compressedBucket, "a"_sd);

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketUnpacker::Behavior::kInclude,
                                       std::move(modifiedCompressedBucket),
                                       kUserDefinedMetaName.toString());

    auto doc0 = Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")};

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
        "_id", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString(), "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto compressedBucket =
        timeseries::compressBucket(bucket, "time"_sd, {}, false).compressedBucket;
    // Remove an element in the time field
    auto modifiedCompressedBucket =
        modifyCompressedBucketRemoveLastInField(*compressedBucket, "time"_sd);

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketUnpacker::Behavior::kInclude,
                                       std::move(modifiedCompressedBucket),
                                       kUserDefinedMetaName.toString());

    auto doc0 = Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")};

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
