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

#include "mongo/db/exec/sbe/values/ts_block.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/sbe/sbe_block_test_helpers.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/bsoncolumn_materializer.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {

class TsSbeValueTest : public SbeStageBuilderTestFixture {};

// This is just a made up example, and is not actually a valid bucket. There's no min/max and
// no time field.
const BSONObj kSampleBucket = fromjson(R"(
{
    "_id" : ObjectId("649f0704230f18da067519c4"),
    "control" : {"version" : 1},
    "meta" : "A",
    "data" : {
        "time" : {
             "0" : {$date: "2023-06-30T21:29:00.568Z"},
             "1" : {$date: "2023-06-30T21:29:09.968Z"},
             "2" : {$date: "2023-06-30T21:29:15.088Z"},
             "3" : {$date: "2023-06-30T21:29:19.088Z"}
        },
        "_id" : {"0" : 0, "1": 1, "2": 2, "3": 3}
    }
})");

int getBucketVersion(const BSONObj& bucket) {
    return bucket[timeseries::kBucketControlFieldName].embeddedObject().getIntField(
        timeseries::kBucketControlVersionFieldName);
}

std::unique_ptr<value::TsBlock> makeTsBlockFromBucket(const BSONObj& bucket, StringData fieldName) {
    auto bucketElem = bucket["data"][fieldName];
    const auto nFields = [&bucket]() -> size_t {
        // Use a dense field.
        const BSONElement timeField = bucket["data"]["time"];
        if (timeField.type() == BSONType::object) {
            return timeField.embeddedObject().nFields();
        } else {
            invariant(timeField.type() == BSONType::binData);
            BSONColumn col(timeField);
            return col.size();
        }
    }();

    auto [columnTag, columnVal] = bson::convertToView(bucketElem);

    const auto nothing = value::TagValueView{};

    const auto [min, max] = [&]() {
        if (bucket["control"]["min"]) {
            return std::pair(bson::convertToView(bucket["control"]["min"][fieldName]),
                             bson::convertToView(bucket["control"]["max"][fieldName]));
        }
        return std::pair(nothing, nothing);
    }();

    return std::make_unique<value::TsBlock>(nFields,
                                            false, /* owned */
                                            columnTag,
                                            columnVal,
                                            getBucketVersion(bucket),
                                            // isTimefield: this check is only safe for the tests
                                            // here where the time field is called 'time'.
                                            fieldName == "time",
                                            min,
                                            max);
}

TEST_F(TsSbeValueTest, CloneCreatesIndependentCopy) {
    // A TsCellBlockForTopLevelField can be created in an "unowned" state.

    auto tsBlock = makeTsBlockFromBucket(kSampleBucket, "_id");
    auto bucketElem = kSampleBucket["data"]["_id"].embeddedObject();
    auto cellBlock = std::make_unique<value::TsCellBlockForTopLevelField>(tsBlock.get());

    auto& valBlock = cellBlock->getValueBlock();

    auto checkBlockIsValid = [](value::ValueBlock& valBlock) {
        {
            auto extractedVals = valBlock.extract();
            ASSERT(extractedVals.count() == 4);
            for (size_t i = 0; i < extractedVals.count(); ++i) {
                auto expectedT = value::TypeTags::NumberDouble;
                auto expectedV = value::bitcastFrom<double>(double(i));

                ASSERT_THAT(extractedVals[i], ValueEq(std::make_pair(expectedT, expectedV)))
                    << "Expected value extracted from cloned CellBlock to be same as original";
            }
        }
    };

    // Test that we can clone the block before extracting it, and we get the right results.
    auto cellBlockCloneBeforeExtract = cellBlock->clone();
    auto valBlockCloneBeforeExtract = valBlock.clone();
    checkBlockIsValid(cellBlockCloneBeforeExtract->getValueBlock());
    checkBlockIsValid(*valBlockCloneBeforeExtract);

    // Now extract the original block, and ensure we get the right results.
    {
        valBlock.extract();

        checkBlockIsValid(valBlock);
        checkBlockIsValid(cellBlock->getValueBlock());
    }

    // Check that we can clone the original cell block _after_ extracting, and still get valid
    // results.
    auto cellBlockCloneAfterExtract = cellBlock->clone();
    auto valBlockCloneAfterExtract = valBlock.clone();
    checkBlockIsValid(cellBlockCloneAfterExtract->getValueBlock());
    checkBlockIsValid(*valBlockCloneAfterExtract);

    // And if we destroy the originals, we should still be able to read the clones.
    cellBlock.reset();

    //
    // 'valBlock' is now invalid.
    //

    checkBlockIsValid(cellBlockCloneBeforeExtract->getValueBlock());
    checkBlockIsValid(*valBlockCloneBeforeExtract);

    checkBlockIsValid(cellBlockCloneAfterExtract->getValueBlock());
    checkBlockIsValid(*valBlockCloneAfterExtract);
}

// Buckets with the v1 schema are not guaranteed to be sorted by the time field.
const BSONObj kBucketWithMinMaxV1 = fromjson(R"(
{
        "_id" : ObjectId("64a33d9cdf56a62781061048"),
        "control" : {
        "version" : 1,
        "min": {
            "_id": 0,
            "time": {$date: "2023-06-30T21:29:00.000Z"}
        },
        "max": {
            "_id": 2,
            "time": {$date: "2023-06-30T21:29:15.088Z"}
        }
    },
        "meta" : "A",
        "data" : {
                "_id" : {"0" : 0, "1": 1, "2" : 2},
                "time" : {
                     "0" : {$date: "2023-06-30T21:29:00.568Z"},
                     "1" : {$date: "2023-06-30T21:29:09.968Z"},
                     "2" : {$date: "2023-06-30T21:29:15.088Z"}
                }
        }
})");

TEST_F(TsSbeValueTest, TsBlockMinMaxV1Schema) {
    {
        auto tsBlock = makeTsBlockFromBucket(kBucketWithMinMaxV1, "_id");
        auto cellBlockId = std::make_unique<value::TsCellBlockForTopLevelField>(tsBlock.get());

        auto& valBlock = cellBlockId->getValueBlock();

        const auto expectedMinId = bson::convertToView(kBucketWithMinMaxV1["data"]["_id"]["0"]);
        const auto expectedMaxId = bson::convertToView(kBucketWithMinMaxV1["data"]["_id"]["2"]);
        invariant(expectedMinId.tag != value::TypeTags::Nothing);
        invariant(expectedMaxId.tag != value::TypeTags::Nothing);

        {
            auto [minTag, minVal] = valBlock.tryMin();
            ASSERT_THAT(std::make_pair(minTag, minVal), ValueEq(expectedMinId))
                << "Expected block min to be the true min val in the block";
        }

        {
            auto [maxTag, maxVal] = valBlock.tryMax();
            ASSERT_THAT(std::make_pair(maxTag, maxVal), ValueEq(expectedMaxId))
                << "Expected block max to be the max val in the block";
        }

        {
            auto [lowerBoundTag, lowerBoundVal] = valBlock.tryLowerBound();
            ASSERT_THAT(std::make_pair(lowerBoundTag, lowerBoundVal), ValueEq(expectedMinId))
                << "Expected block lower bound to be the true min val in the block";
        }

        {
            auto [upperBoundTag, upperBoundVal] = valBlock.tryUpperBound();
            ASSERT_THAT(std::make_pair(upperBoundTag, upperBoundVal), ValueEq(expectedMaxId))
                << "Expected block upper bound to be the true max val in the block";
        }
    }

    {
        auto tsBlock = makeTsBlockFromBucket(kBucketWithMinMaxV1, "time");
        auto cellBlockTime = std::make_unique<value::TsCellBlockForTopLevelField>(tsBlock.get());

        auto& valBlock = cellBlockTime->getValueBlock();

        const auto expectedLowerBoundTime =
            bson::convertToView(kBucketWithMinMaxV1["control"]["min"]["time"]);
        const auto expectedMaxTime = bson::convertToView(kBucketWithMinMaxV1["data"]["time"]["2"]);

        {
            auto [minTag, minVal] = valBlock.tryMin();
            // The min time field value for v1 buckets cannot be determined in O(1), so tryMin()
            // should return Nothing.
            ASSERT_EQ(minTag, value::TypeTags::Nothing)
                << "Expected block min of the time field to be nothing";
        }

        {
            auto [maxTag, maxVal] = valBlock.tryMax();
            ASSERT_THAT(std::make_pair(maxTag, maxVal), ValueEq(expectedMaxTime))
                << "Expected block max for to be the max val in the block";
        }
        {
            auto [lowerBoundTag, lowerBoundVal] = valBlock.tryLowerBound();
            ASSERT_THAT(std::make_pair(lowerBoundTag, lowerBoundVal),
                        ValueEq(expectedLowerBoundTime))
                << "Expected block lower bound to be the control min val in the block";
        }

        {
            auto [upperBoundTag, upperBoundVal] = valBlock.tryUpperBound();
            ASSERT_THAT(std::make_pair(upperBoundTag, upperBoundVal), ValueEq(expectedMaxTime))
                << "Expected block upper bound to be the true max val in the block";
        }
    }
}

TEST_F(TsSbeValueTest, TsBlockMinMaxV2Schema) {
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithMinMaxV1, "time"_sd, {}, false).compressedBucket;
    ASSERT(compressedBucketOpt) << "Should have been able to create compressed v2 bucket";
    auto compressedBucket = *compressedBucketOpt;

    {
        auto tsBlock = makeTsBlockFromBucket(compressedBucket, "_id");
        auto cellBlockId = std::make_unique<value::TsCellBlockForTopLevelField>(tsBlock.get());

        auto& valBlock = cellBlockId->getValueBlock();

        const auto expectedMinId = bson::convertToView(kBucketWithMinMaxV1["data"]["_id"]["0"]);
        const auto expectedMaxId = bson::convertToView(kBucketWithMinMaxV1["data"]["_id"]["2"]);
        invariant(expectedMinId.tag != value::TypeTags::Nothing);
        invariant(expectedMaxId.tag != value::TypeTags::Nothing);

        {
            auto [minTag, minVal] = valBlock.tryMin();
            ASSERT_THAT(std::make_pair(minTag, minVal), ValueEq(expectedMinId))
                << "Expected block min to be the true min val in the block";

            auto [maxTag, maxVal] = valBlock.tryMax();
            ASSERT_THAT(std::make_pair(maxTag, maxVal), ValueEq(expectedMaxId))
                << "Expected block max to be the max val in the block";
        }

        {
            auto [lowerBoundTag, lowerBoundVal] = valBlock.tryLowerBound();
            ASSERT_THAT(std::make_pair(lowerBoundTag, lowerBoundVal), ValueEq(expectedMinId))
                << "Expected block lower bound to be the true min val in the block";
        }

        {
            auto [upperBoundTag, upperBoundVal] = valBlock.tryUpperBound();
            ASSERT_THAT(std::make_pair(upperBoundTag, upperBoundVal), ValueEq(expectedMaxId))
                << "Expected block upper bound to be the true max val in the block";
        }
    }

    {
        auto tsBlock = makeTsBlockFromBucket(compressedBucket, "time");
        auto cellBlockTime = std::make_unique<value::TsCellBlockForTopLevelField>(tsBlock.get());

        auto& valBlock = cellBlockTime->getValueBlock();

        const auto expectedMinTime = bson::convertToView(kBucketWithMinMaxV1["data"]["time"]["0"]);
        const auto expectedMaxTime = bson::convertToView(kBucketWithMinMaxV1["data"]["time"]["2"]);
        const auto expectedLowerBoundTime =
            bson::convertToView(kBucketWithMinMaxV1["control"]["min"]["time"]);

        {
            // The min time field value for v2 buckets can be determined in O(1), so tryMin() should
            // return the true min.
            {
                auto [minTag, minVal] = valBlock.tryMin();
                ASSERT_THAT(std::make_pair(minTag, minVal), ValueEq(expectedMinTime))
                    << "Expected block min of the time field to be the true min";
            }

            {
                auto [maxTag, maxVal] = valBlock.tryMax();
                ASSERT_THAT(std::make_pair(maxTag, maxVal), ValueEq(expectedMaxTime))
                    << "Expected block max of the time field to be the max val in the block";
            }

            {
                auto [lowerBoundTag, lowerBoundVal] = valBlock.tryLowerBound();
                ASSERT_THAT(std::make_pair(lowerBoundTag, lowerBoundVal),
                            ValueEq(expectedLowerBoundTime))
                    << "Expected block lower bound to be the control.min in the bucket";
            }

            {
                auto [upperBoundTag, upperBoundVal] = valBlock.tryUpperBound();
                ASSERT_THAT(std::make_pair(upperBoundTag, upperBoundVal), ValueEq(expectedMaxTime))
                    << "Expected block upper bound to be the true max val in the block";
            }
        }
    }
}

TEST_F(TsSbeValueTest, TsBlockMinMaxV3Schema) {
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithMinMaxV1, "time"_sd, {}, false).compressedBucket;
    ASSERT(compressedBucketOpt) << "Should have been able to create compressed v2 bucket";

    auto compressedBucket = *compressedBucketOpt;

    // Now bump the version field to 3. A v3 bucket is the same format as a v2 bucket, except
    // there's no guarantee the time field is sorted.
    Document d(compressedBucket);
    MutableDocument md(d);
    md.setNestedField("control.version", Value(3));

    compressedBucket = md.freeze().toBson();

    {
        auto tsBlock = makeTsBlockFromBucket(compressedBucket, "_id");
        auto cellBlockId = std::make_unique<value::TsCellBlockForTopLevelField>(tsBlock.get());

        auto& valBlock = cellBlockId->getValueBlock();

        const auto expectedMinId = bson::convertToView(kBucketWithMinMaxV1["data"]["_id"]["0"]);
        const auto expectedMaxId = bson::convertToView(kBucketWithMinMaxV1["data"]["_id"]["2"]);

        {
            auto [minTag, minVal] = valBlock.tryMin();
            ASSERT_THAT(std::make_pair(minTag, minVal), ValueEq(expectedMinId))
                << "Expected block min to be the true min val in the block";

            auto [maxTag, maxVal] = valBlock.tryMax();
            ASSERT_THAT(std::make_pair(maxTag, maxVal), ValueEq(expectedMaxId))
                << "Expected block max to be the max val in the block";
        }

        {
            auto [lowerBoundTag, lowerBoundVal] = valBlock.tryLowerBound();
            ASSERT_THAT(std::make_pair(lowerBoundTag, lowerBoundVal), ValueEq(expectedMinId))
                << "Expected block lower bound to be the true min val in the block";
        }

        {
            auto [upperBoundTag, upperBoundVal] = valBlock.tryUpperBound();
            ASSERT_THAT(std::make_pair(upperBoundTag, upperBoundVal), ValueEq(expectedMaxId))
                << "Expected block upper bound to be the true max val in the block";
        }
    }

    {
        auto tsBlock = makeTsBlockFromBucket(compressedBucket, "time");
        auto cellBlockTime = std::make_unique<value::TsCellBlockForTopLevelField>(tsBlock.get());

        auto& valBlock = cellBlockTime->getValueBlock();

        const auto expectedMaxTime = bson::convertToView(kBucketWithMinMaxV1["data"]["time"]["2"]);
        const auto expectedLowerBoundTime =
            bson::convertToView(kBucketWithMinMaxV1["control"]["min"]["time"]);

        {
            {
                // V3 buckets are not guaranteed to be sorted, so there is no way to compute the
                // minimum time value in O(1).
                auto [minTag, minVal] = valBlock.tryMin();
                ASSERT_EQ(minTag, value::TypeTags::Nothing)
                    << "Expected block min of the time field to be nothing";
            }

            {
                auto [maxTag, maxVal] = valBlock.tryMax();
                ASSERT_THAT(std::make_pair(maxTag, maxVal), ValueEq(expectedMaxTime))
                    << "Expected block max of the time field to be the max val in the block";
            }

            {
                auto [lowerBoundTag, lowerBoundVal] = valBlock.tryLowerBound();
                ASSERT_THAT(std::make_pair(lowerBoundTag, lowerBoundVal),
                            ValueEq(expectedLowerBoundTime))
                    << "Expected block lower bound to be the bucket control.min";
            }

            {
                auto [upperBoundTag, upperBoundVal] = valBlock.tryUpperBound();
                ASSERT_THAT(std::make_pair(upperBoundTag, upperBoundVal), ValueEq(expectedMaxTime))
                    << "Expected block upper bound to be the true max val in the block";
            }
        }
    }
}

// Bucket with pre-1970 dates and the control.max rounded up for time.
const BSONObj kBucketWithMinMaxPre1970 = fromjson(R"(
{
        "_id" : ObjectId("64a33d9cdf56a62781061048"),
        "control" : {
        "version" : 1,
        "min": {
            "_id": 0,
            "time": Date(-100)
        },
        "max": {
            "_id": 2,
            "time": Date(0)
        }
    },
        "meta" : "A",
        "data" : {
                "_id" : {"0" : 0, "1": 1, "2" : 2},
                "time" : {
                     "0" : Date(-100),
                     "1" : Date(-100),
                     "2" : Date(-100)
                }
        }
})");

TEST_F(TsSbeValueTest, TsBlockMaxTimePre1970) {
    auto tsBlock = makeTsBlockFromBucket(kBucketWithMinMaxPre1970, "time");
    auto cellBlockTime = std::make_unique<value::TsCellBlockForTopLevelField>(tsBlock.get());

    auto& valBlock = cellBlockTime->getValueBlock();
    const auto expectedLowerBoundTime =
        bson::convertToView(kBucketWithMinMaxPre1970["control"]["min"]["time"]);
    const auto expectedUpperBoundTime =
        bson::convertToView(kBucketWithMinMaxPre1970["control"]["max"]["time"]);

    {
        auto [minTag, minVal] = valBlock.tryMin();
        ASSERT_EQ(minTag, value::TypeTags::Nothing)
            << "Expected block min of the time field to be nothing";
    }

    {
        // Max cannot be used for dates before or equal to the unix epoch.
        auto [maxTag, maxVal] = valBlock.tryMax();
        ASSERT_EQ(maxTag, value::TypeTags::Nothing)
            << "Expected block max of the time field to be nothing";
    }

    {
        auto [lowerBoundTag, lowerBoundVal] = valBlock.tryLowerBound();
        ASSERT_THAT(std::make_pair(lowerBoundTag, lowerBoundVal), ValueEq(expectedLowerBoundTime))
            << "Expected block lower bound to be the bucket control.min";
    }

    {
        auto [upperBoundTag, upperBoundVal] = valBlock.tryUpperBound();
        ASSERT_THAT(std::make_pair(upperBoundTag, upperBoundVal), ValueEq(expectedUpperBoundTime))
            << "Expected block upper bound to be the bucket control.max";
    }
}

const BSONObj kBucketWithMinMaxAndArrays = fromjson(R"(
{
        "_id" : ObjectId("64a33d9cdf56a62781061048"),
        "control" : {
        "version" : 1,
        "min": {
            "_id": 0,
            "time": {$date: "2023-06-30T21:29:00.000Z"},
            "arr": [1,2],
            "sometimesMissing": 0
        },
        "max": {
            "_id": 2,
            "time": {$date: "2023-06-30T21:29:15.088Z"},
            "arr": [5, 5],
            "sometimesMissing": 9
        }
    },
        "meta" : "A",
        "data" : {
                "_id" : {"0" : 0, "1": 1, "2" : 2},
                "time" : {
                        "0" : {$date: "2023-06-30T21:29:00.568Z"},
                        "1" : {$date: "2023-06-30T21:29:09.968Z"},
                        "2" : {$date: "2023-06-30T21:29:15.088Z"}
                },
                "arr": {"0": [1, 2], "1": [2, 3], "2": [5, 5]},
                "sometimesMissing": {"0": 0, "2": 9}
        }
})");

TEST_F(TsSbeValueTest, TsBlockHasArray) {
    {
        auto tsBlock = makeTsBlockFromBucket(kBucketWithMinMaxAndArrays, "_id");
        boost::optional<bool> hasArrayRes = tsBlock->tryHasArray();
        ASSERT_TRUE(hasArrayRes);  // Check that it gives us a definitive yes/no.
        ASSERT_FALSE(*hasArrayRes);
    }

    {
        auto tsBlock = makeTsBlockFromBucket(kBucketWithMinMaxAndArrays, "arr");
        boost::optional<bool> hasArrayRes = tsBlock->tryHasArray();
        ASSERT_TRUE(hasArrayRes);  // Check that it gives us a definitive yes/no.
        ASSERT_TRUE(*hasArrayRes);
    }
}

TEST_F(TsSbeValueTest, TsBlockFillEmpty) {
    {
        auto tsBlock = makeTsBlockFromBucket(kBucketWithMinMaxAndArrays, "_id");
        // v1 bucket, non-time dense field: tryDense() returns false via the
        // heuristic, so fillEmpty() deblocks and delegates. The deblocked block
        // is itself dense and returns nullptr.
        ASSERT(tsBlock->fillEmpty(value::TypeTags::Null, 0) == nullptr);
    }

    {
        auto tsBlock = makeTsBlockFromBucket(kBucketWithMinMaxAndArrays, "sometimesMissing");
        auto fillRes = tsBlock->fillEmpty(value::TypeTags::Null, 0);
        ASSERT(fillRes);
        auto extracted = fillRes->extract();
        ASSERT_EQ(extracted.count(), 3);
        assertValuesEqual(extracted[0].tag,
                          extracted[0].value,
                          value::TypeTags::NumberDouble,
                          value::bitcastFrom<double>(0));
        assertValuesEqual(extracted[1].tag, extracted[1].value, value::TypeTags::Null, 0);
        assertValuesEqual(extracted[2].tag,
                          extracted[2].value,
                          value::TypeTags::NumberDouble,
                          value::bitcastFrom<double>(9));
    }

    {
        auto compressedBucketOpt =
            timeseries::compressBucket(kBucketWithMinMaxAndArrays, "time"_sd, {}, false)
                .compressedBucket;
        ASSERT(compressedBucketOpt);
        auto compressedBucket = *compressedBucketOpt;

        // _id is dense and is not the time field. fillEmpty() must return nullptr
        // WITHOUT deblocking the column.
        auto tsBlock = makeTsBlockFromBucket(compressedBucket, "_id");
        ASSERT(tsBlock->fillEmpty(value::TypeTags::Null, 0) == nullptr);
        ASSERT(tsBlock->decompressedBlock_forTest() == nullptr)
            << "fillEmpty() on a dense column must not trigger deblocking";
    }
}

const BSONObj kBucketWithMixedNumbers = fromjson(R"(
{
    "_id" : ObjectId("64a33d9cdf56a62781061048"),
    "control" : {
        "version" : 1,
        "min": {
            "_id": 0,
            "time": {$date: "2023-06-30T21:29:00.000Z"},
            "num": NumberLong(123)
        },
        "max": {
            "_id": 2,
            "time": {$date: "2023-06-30T21:29:15.088Z"},
            "num": NumberLong(789)
        }
    },
    "meta" : "A",
    "data" : {
        "_id" : {"0" : 0, "1": 1, "2" : 2},
        "time" : {
            "0" : {$date: "2023-06-30T21:29:00.568Z"},
            "1" : {$date: "2023-06-30T21:29:09.968Z"},
            "2" : {$date: "2023-06-30T21:29:15.088Z"}
        },
        num: {"0": NumberLong(123),
              "1": NumberInt(456),
              "2": NumberLong(789)}
    }
})");

TEST_F(TsSbeValueTest, FillType) {
    {
        // Tests on the "time" field.
        auto timeBlock = makeTsBlockFromBucket(kBucketWithMixedNumbers, "time");

        auto [fillTag, fillVal] = makeDecimal("1234.5678");
        value::ValueGuard fillGuard{fillTag, fillVal};

        {
            uint32_t nullUndefinedTypeMask = static_cast<uint32_t>(
                getBSONTypeMask(BSONType::null) | getBSONTypeMask(BSONType::undefined));

            auto out = timeBlock->fillType(nullUndefinedTypeMask, fillTag, fillVal);

            // The type mask won't match the control min/max tags, so no work needs to be done.
            ASSERT_EQ(out, nullptr);
        }

        {
            uint32_t dateTypeMask = static_cast<uint32_t>(getBSONTypeMask(BSONType::date));

            auto out = timeBlock->fillType(dateTypeMask, fillTag, fillVal);
            ASSERT_NE(out, nullptr);
            auto outVal = value::bitcastFrom<value::ValueBlock*>(out.get());
            assertBlockEq(value::TypeTags::valueBlock,
                          outVal,
                          TypedValues{{fillTag, fillVal}, {fillTag, fillVal}, {fillTag, fillVal}});
        }
    }

    {
        // Test on the "num" field.
        auto numBlock = makeTsBlockFromBucket(kBucketWithMixedNumbers, "num");

        auto extracted = numBlock->extract();

        auto [fillTag, fillVal] = makeDecimal("1234.5678");
        value::ValueGuard fillGuard{fillTag, fillVal};

        {
            uint32_t arrayStringTypeMask = static_cast<uint32_t>(getBSONTypeMask(BSONType::array) |
                                                                 getBSONTypeMask(BSONType::string));

            auto out = numBlock->fillType(arrayStringTypeMask, fillTag, fillVal);

            // The type mask won't match the control min/max tags, so no work needs to be done.
            ASSERT_EQ(out, nullptr);
        }

        {
            // The min and max won't match this tag since they are NumberLongs but there is a value
            // in the block that should match this tag.
            uint32_t int32TypeMask = static_cast<uint32_t>(getBSONTypeMask(BSONType::numberInt));

            auto out = numBlock->fillType(int32TypeMask, fillTag, fillVal);
            ASSERT_NE(out, nullptr);
            auto outVal = value::bitcastFrom<value::ValueBlock*>(out.get());
            assertBlockEq(value::TypeTags::valueBlock,
                          outVal,
                          TypedValues{extracted[0], {fillTag, fillVal}, extracted[2]});
        }
    }
}

const BSONObj kBucketWithArrs = fromjson(R"(
{
    "_id" : ObjectId("64a33d9cdf56a62781061048"),
    "control" : {
        "version" : 1,
        "min": {
            "_id": 0,
            "time": {$date: "2023-06-30T21:29:00.000Z"},
            "num": NumberLong(123),
            "arr": [2, 3, 3]
        },
        "max": {
            "_id": 2,
            "time": {$date: "2023-06-30T21:29:15.088Z"},
            "num": NumberLong(789),
            "arr": [0, 1, 0]
        }
    },
    "meta" : "A",
    "data" : {
        "_id" : {"0" : 0, "1": 1, "2" : 2},
        "time" : {
            "0" : {$date: "2023-06-30T21:29:00.568Z"},
            "1" : {$date: "2023-06-30T21:29:09.968Z"},
            "2" : {$date: "2023-06-30T21:29:15.088Z"}
        },
        num: {"0": NumberLong(123),
              "1": NumberLong(456),
              "2": NumberLong(789)},
        arr: {"0": [1, 2, 3],
              "1": [0, 1, 2],
              "2": [2, 3, 0]}
    }
})");

TEST_F(TsSbeValueTest, TsBlockMiscTest) {
    {
        // Tests on the "time" field.
        auto timeBlock = makeTsBlockFromBucket(kBucketWithArrs, "time");

        ASSERT_EQ(timeBlock->tryDense(), boost::optional<bool>(true));
        ASSERT(timeBlock->hasNoObjsOrArrays());
    }

    {
        // Test on the "num" field.
        auto numBlock = makeTsBlockFromBucket(kBucketWithArrs, "num");

        ASSERT_EQ(numBlock->tryDense(), boost::optional<bool>(false));
        ASSERT(numBlock->hasNoObjsOrArrays());
    }

    {
        // Test on the "arr" field.
        auto arrBlock = makeTsBlockFromBucket(kBucketWithArrs, "arr");

        ASSERT_EQ(arrBlock->tryDense(), boost::optional<bool>(false));
        ASSERT(!arrBlock->hasNoObjsOrArrays());
        // We do not use control min/max when they are arrays.
        ASSERT_EQ(arrBlock->tryLowerBound().tag, value::TypeTags::Nothing);
        ASSERT_EQ(arrBlock->tryMax().tag, value::TypeTags::Nothing);
    }
}

const BSONObj kBucketWithBigScalars = fromjson(R"(
{
    "_id" : ObjectId("64a33d9cdf56a62781061048"),
    "control" : {
        "version" : 1,
        "min": {
            "_id": 0,
            "time": {$date: "2023-06-30T21:29:00.000Z"},
            "bigString": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "num": 123
        },
        "max": {
            "_id": 2,
            "time": {$date: "2023-06-30T21:29:15.088Z"},
            "bigString": "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
            "num": 999
        }
    },
    "meta" : "A",
    "data" : {
        "_id" : {"0" : 0, "1": 1, "2" : 2},
        "time" : {
            "0" : {$date: "2023-06-30T21:29:00.568Z"},
            "1" : {$date: "2023-06-30T21:29:09.968Z"},
            "2" : {$date: "2023-06-30T21:29:15.088Z"}
        },
        bigString: {"0": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                    "1": "bb",
                    "2": "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"},
        num: {"0": 456,
              "1": 123,
              "2": 999}
    }
})");


TEST_F(TsSbeValueTest, VerifyDecompressedBlockType) {
    {
        // Extracting from an uncompressed bucket always does the copy.
        auto tsBlock = makeTsBlockFromBucket(kBucketWithBigScalars, "bigString");
        [[maybe_unused]] auto unusedDeblockedVals = tsBlock->extract();

        auto decompressedInternalBlock = tsBlock->decompressedBlock_forTest();
        ASSERT(decompressedInternalBlock);
        ASSERT(dynamic_cast<value::HeterogeneousBlock*>(decompressedInternalBlock));
    }

    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithBigScalars, "time"_sd, {}, false).compressedBucket;
    ASSERT(compressedBucketOpt) << "Should have been able to create compressed v2 bucket";
    auto compressedBucket = *compressedBucketOpt;

    {
        // Extracting from a column with deep values from a compressed bucket avoids the copy.
        auto tsBlock = makeTsBlockFromBucket(compressedBucket, "bigString");
        [[maybe_unused]] auto unusedDeblockedVals = tsBlock->extract();

        auto decompressedInternalBlock = tsBlock->decompressedBlock_forTest();
        ASSERT(decompressedInternalBlock);
        ASSERT(dynamic_cast<value::BSONElementStorageValueBlock*>(decompressedInternalBlock));
    }

    {
        // Extracting from a column with shallow values from a compressed bucket gives a
        // HomogeneousBlock.
        auto tsBlock = makeTsBlockFromBucket(compressedBucket, "num");
        [[maybe_unused]] auto unusedDeblockedVals = tsBlock->extract();

        auto decompressedInternalBlock = tsBlock->decompressedBlock_forTest();
        ASSERT(decompressedInternalBlock);
        std::cout << "ian: " << typeid(*decompressedInternalBlock).name() << std::endl;
        ASSERT(dynamic_cast<value::Int32Block*>(decompressedInternalBlock));
    }
}

TEST_F(TsSbeValueTest, TsBlockTryDenseFastPath) {
    // --- v2 (compressed) bucket cases ---
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithMinMaxAndArrays, "time"_sd, {}, false)
            .compressedBucket;
    ASSERT(compressedBucketOpt) << "Should have been able to create compressed v2 bucket";
    auto compressedBucket = *compressedBucketOpt;

    {
        // The time field is always dense.
        auto tsBlock = makeTsBlockFromBucket(compressedBucket, "time");
        ASSERT_EQ(tsBlock->tryDense(), boost::optional<bool>(true));
    }

    {
        // _id is dense but is not the time field; tryDense() must detect this via
        // bsoncolumn::dense(), not the _isTimeField heuristic.
        auto tsBlock = makeTsBlockFromBucket(compressedBucket, "_id");
        ASSERT_EQ(tsBlock->tryDense(), boost::optional<bool>(true));
    }

    {
        // "sometimesMissing" has positions 0 and 2 populated but position 1 missing,
        // so it is genuinely sparse. Call twice to verify the cache also holds
        // the "false" branch.
        auto tsBlock = makeTsBlockFromBucket(compressedBucket, "sometimesMissing");
        ASSERT_EQ(tsBlock->tryDense(), boost::optional<bool>(false));
        ASSERT_EQ(tsBlock->tryDense(), boost::optional<bool>(false));
    }

    // --- v1 (uncompressed BSONObject) bucket cases: heuristic fallback ---
    {
        // Time field in v1 — still reported dense via _isTimeField.
        auto tsBlock = makeTsBlockFromBucket(kBucketWithMinMaxAndArrays, "time");
        ASSERT_EQ(tsBlock->tryDense(), boost::optional<bool>(true));
    }

    {
        // Non-time dense field in v1 — heuristic says not dense (preserved behavior).
        // Call twice to verify the v1 cache branch also holds its value.
        auto tsBlock = makeTsBlockFromBucket(kBucketWithMinMaxAndArrays, "_id");
        ASSERT_EQ(tsBlock->tryDense(), boost::optional<bool>(false));
        ASSERT_EQ(tsBlock->tryDense(), boost::optional<bool>(false));
    }

    // --- caching sanity check ---
    {
        auto tsBlock = makeTsBlockFromBucket(compressedBucket, "_id");
        auto first = tsBlock->tryDense();
        auto second = tsBlock->tryDense();
        ASSERT_EQ(first, second);
        ASSERT_EQ(first, boost::optional<bool>(true));
    }
}

TEST_F(TsSbeValueTest, TsBlockArgMinMaxBSONColumnFastPath) {
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithMinMaxAndArrays, "time"_sd, {}, false)
            .compressedBucket;
    ASSERT(compressedBucketOpt);
    auto compressedBucket = *compressedBucketOpt;

    // Non-time dense numeric field: argMin/argMax must run via
    // bsoncolumn::min/max and must NOT trigger full decompression.
    auto tsBlock = makeTsBlockFromBucket(compressedBucket, "_id");

    auto minIdx = tsBlock->argMin();
    auto maxIdx = tsBlock->argMax();
    ASSERT_TRUE(minIdx.has_value());
    ASSERT_TRUE(maxIdx.has_value());
    ASSERT_FALSE(tsBlock->decompressed())
        << "argMin/argMax should not trigger ensureDeblocked on a BSONColumn-backed block";
    ASSERT_LT(*minIdx, tsBlock->count());
    ASSERT_LT(*maxIdx, tsBlock->count());

    // Verify the indices actually point to the extreme values. at() triggers
    // ensureDeblocked(), which is the current behavior before any fast-path
    // optimization of at() itself.
    const auto expectedMin =
        bson::convertToView(kBucketWithMinMaxAndArrays["control"]["min"]["_id"]);
    const auto expectedMax =
        bson::convertToView(kBucketWithMinMaxAndArrays["control"]["max"]["_id"]);
    auto [minTag, minVal] = tsBlock->at(*minIdx);
    auto [maxTag, maxVal] = tsBlock->at(*maxIdx);
    ASSERT_THAT(std::make_pair(minTag, minVal), ValueEq(expectedMin))
        << "argMin index must point to the minimum value in the block";
    ASSERT_THAT(std::make_pair(maxTag, maxVal), ValueEq(expectedMax))
        << "argMax index must point to the maximum value in the block";
}

TEST_F(TsSbeValueTest, TsBlockArgMinMaxSparseColumn) {
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithMinMaxAndArrays, "time"_sd, {}, false)
            .compressedBucket;
    ASSERT(compressedBucketOpt);
    auto compressedBucket = *compressedBucketOpt;

    auto tsBlock = makeTsBlockFromBucket(compressedBucket, "sometimesMissing");
    auto minIdx = tsBlock->argMin();
    ASSERT_TRUE(minIdx.has_value());
    ASSERT_LT(*minIdx, tsBlock->count());
    ASSERT_FALSE(tsBlock->decompressed())
        << "Sparse columns also go through the bsoncolumn::min fast path";
}

TEST_F(TsSbeValueTest, TsBlockArgMinTimeSortedShortcut) {
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithMinMaxAndArrays, "time"_sd, {}, false)
            .compressedBucket;
    ASSERT(compressedBucketOpt);
    auto compressedBucket = *compressedBucketOpt;

    auto tsBlock = makeTsBlockFromBucket(compressedBucket, "time");
    ASSERT_EQ(tsBlock->argMin(), boost::optional<size_t>(0u));
    ASSERT_EQ(tsBlock->argMax(), boost::optional<size_t>(tsBlock->count() - 1));
    ASSERT_FALSE(tsBlock->decompressed()) << "Time-sorted argMin/argMax must not deblock";
}

// Exercises the middle dispatch branch: when _decompressedBlock is already set,
// argMin/argMax must delegate to it rather than re-running the BSONColumn fast path.
TEST_F(TsSbeValueTest, TsBlockArgMinDelegatesToDecompressedBlock) {
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithMinMaxAndArrays, "time"_sd, {}, false)
            .compressedBucket;
    ASSERT(compressedBucketOpt);
    auto compressedBucket = *compressedBucketOpt;

    // _id has values {0, 1, 2}: min at index 0, max at index 2.
    auto tsBlock = makeTsBlockFromBucket(compressedBucket, "_id");

    // Force deblocking so that _decompressedBlock is populated.
    [[maybe_unused]] auto unused = tsBlock->extract();
    ASSERT_TRUE(tsBlock->decompressed()) << "extract() must populate _decompressedBlock";

    // Now argMin/argMax must go through the _decompressedBlock delegation branch.
    auto minIdx = tsBlock->argMin();
    auto maxIdx = tsBlock->argMax();
    ASSERT_TRUE(minIdx.has_value());
    ASSERT_TRUE(maxIdx.has_value());

    // The block is still decompressed after the call (the delegation branch must
    // not reset _decompressedBlock).
    ASSERT_TRUE(tsBlock->decompressed())
        << "argMin/argMax via delegation must leave _decompressedBlock intact";

    // Verify the indices point to the correct extreme values.
    const auto expectedMin =
        bson::convertToView(kBucketWithMinMaxAndArrays["control"]["min"]["_id"]);
    const auto expectedMax =
        bson::convertToView(kBucketWithMinMaxAndArrays["control"]["max"]["_id"]);
    auto [minTag, minVal] = tsBlock->at(*minIdx);
    auto [maxTag, maxVal] = tsBlock->at(*maxIdx);
    ASSERT_THAT(std::make_pair(minTag, minVal), ValueEq(expectedMin))
        << "argMin (via decompressed delegation) must return the true minimum index";
    ASSERT_THAT(std::make_pair(maxTag, maxVal), ValueEq(expectedMax))
        << "argMax (via decompressed delegation) must return the true maximum index";
}

TEST_F(TsSbeValueTest, TsBlockAtBoundaryFastPathDense) {
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithMinMaxAndArrays, "time"_sd, {}, false)
            .compressedBucket;
    ASSERT(compressedBucketOpt);
    auto compressedBucket = *compressedBucketOpt;

    // Dense non-time field.
    auto tsBlock = makeTsBlockFromBucket(compressedBucket, "_id");

    auto first = tsBlock->at(0);
    auto last = tsBlock->at(tsBlock->count() - 1);
    ASSERT_FALSE(tsBlock->decompressed())
        << "at(0)/at(count-1) on a dense column should use bsoncolumn::first/last";

    // The _id field in kBucketWithMinMaxAndArrays holds {0, 1, 2}, so
    // at(0) must yield 0 and at(count-1) must yield 2. Asserting on the
    // actual values (not just non-Nothing) catches regressions where the
    // boundary fast path returns the wrong element.
    const auto expectedFirst =
        bson::convertToView(kBucketWithMinMaxAndArrays["control"]["min"]["_id"]);
    const auto expectedLast =
        bson::convertToView(kBucketWithMinMaxAndArrays["control"]["max"]["_id"]);
    ASSERT_THAT(std::make_pair(first.tag, first.value), ValueEq(expectedFirst));
    ASSERT_THAT(std::make_pair(last.tag, last.value), ValueEq(expectedLast));

    // Second call hits cache; still no decompression.
    auto firstAgain = tsBlock->at(0);
    ASSERT_FALSE(tsBlock->decompressed());
    ASSERT_THAT(std::make_pair(firstAgain.tag, firstAgain.value), ValueEq(expectedFirst));
}

TEST_F(TsSbeValueTest, TsBlockAtSparseFallsThroughToDeblock) {
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithMinMaxAndArrays, "time"_sd, {}, false)
            .compressedBucket;
    ASSERT(compressedBucketOpt);
    auto compressedBucket = *compressedBucketOpt;

    // Sparse field: boundary fast path is not safe, so at() must fall through.
    auto tsBlock = makeTsBlockFromBucket(compressedBucket, "sometimesMissing");
    (void)tsBlock->at(0);
    ASSERT_TRUE(tsBlock->decompressed())
        << "at() on a sparse BSONColumn must fall through to ensureDeblocked()";
}

TEST_F(TsSbeValueTest, TsBlockAtSparseCorrectValues) {
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithMinMaxAndArrays, "time"_sd, {}, false)
            .compressedBucket;
    ASSERT(compressedBucketOpt);
    auto compressedBucket = *compressedBucketOpt;

    // "sometimesMissing" is sparse: position 0 -> 0, position 1 -> missing, position 2 -> 9.
    // at() must fall through to ensureDeblocked() and return the correct value at each index.
    auto tsBlock = makeTsBlockFromBucket(compressedBucket, "sometimesMissing");
    ASSERT_EQ(tsBlock->count(), 3u);

    auto first = tsBlock->at(0);
    ASSERT_TRUE(tsBlock->decompressed()) << "at(0) on sparse column must deblock";
    ASSERT_EQ(first.tag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(first.value), 0);

    auto missing = tsBlock->at(1);
    ASSERT_EQ(missing.tag, value::TypeTags::Nothing)
        << "at(1) on a missing position must return Nothing";

    auto last = tsBlock->at(tsBlock->count() - 1);
    ASSERT_EQ(last.tag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(last.value), 9);
}

TEST_F(TsSbeValueTest, TsBlockAtInteriorIndexDeblocks) {
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithMinMaxAndArrays, "time"_sd, {}, false)
            .compressedBucket;
    ASSERT(compressedBucketOpt);
    auto compressedBucket = *compressedBucketOpt;

    auto tsBlock = makeTsBlockFromBucket(compressedBucket, "_id");
    // Interior, non-cached index: must fall through.
    size_t interior = tsBlock->count() / 2;
    ASSERT_GT(interior, 0u);
    ASSERT_LT(interior, tsBlock->count() - 1);
    (void)tsBlock->at(interior);
    ASSERT_TRUE(tsBlock->decompressed()) << "at(interior) must fall through to ensureDeblocked()";
}

TEST_F(TsSbeValueTest, TsBlockArgMinThenAtHitsCache) {
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithMinMaxAndArrays, "time"_sd, {}, false)
            .compressedBucket;
    ASSERT(compressedBucketOpt);
    auto compressedBucket = *compressedBucketOpt;

    auto tsBlock = makeTsBlockFromBucket(compressedBucket, "_id");
    auto idx = tsBlock->argMin();
    ASSERT_TRUE(idx.has_value());
    auto elem = tsBlock->at(*idx);
    ASSERT_FALSE(tsBlock->decompressed())
        << "argMin populates the cache; at(argMin) must not deblock";
    ASSERT_NE(elem.tag, value::TypeTags::Nothing);
}

// Bucket with a string column whose values are all >7 chars, so the SBEColumnMaterializer
// emits 'bsonString' (heap-allocated via the BSONElementStorage) rather than 'StringSmall'
// (inline). The min and max are at interior indices (1 and 2) so that at(argMin/argMax) cannot
// short-circuit through the boundary fast paths in TsBlock::at() and must instead hit the
// _atCache populated by argMin/argMax.
const BSONObj kBucketWithDeepStrings = fromjson(R"(
{
    "_id": ObjectId("64a33d9cdf56a62781061049"),
    "control": {
        "version": 1,
        "min": {
            "_id": 0,
            "time": {$date: "2023-06-30T21:29:00.000Z"},
            "name": "alpha-001"
        },
        "max": {
            "_id": 3,
            "time": {$date: "2023-06-30T21:29:15.000Z"},
            "name": "delta-004"
        }
    },
    "meta": "A",
    "data": {
        "_id": {"0": 0, "1": 1, "2": 2, "3": 3},
        "time": {
            "0": {$date: "2023-06-30T21:29:00.000Z"},
            "1": {$date: "2023-06-30T21:29:05.000Z"},
            "2": {$date: "2023-06-30T21:29:10.000Z"},
            "3": {$date: "2023-06-30T21:29:15.000Z"}
        },
        "name": {
            "0": "bravo-002",
            "1": "alpha-001",
            "2": "delta-004",
            "3": "charlie-03"
        }
    }
})");

TEST_F(TsSbeValueTest, TsBlockArgMinMaxAtCacheStoresDeepString) {
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithDeepStrings, "time"_sd, {}, false).compressedBucket;
    ASSERT(compressedBucketOpt);

    auto tsBlock = makeTsBlockFromBucket(*compressedBucketOpt, "name");

    auto minIdx = tsBlock->argMin();
    auto maxIdx = tsBlock->argMax();
    ASSERT_EQ(minIdx, boost::optional<size_t>(1u)) << "argMin must point to the interior min slot";
    ASSERT_EQ(maxIdx, boost::optional<size_t>(2u)) << "argMax must point to the interior max slot";
    ASSERT_FALSE(tsBlock->decompressed()) << "argMin/argMax must not deblock";

    auto check = [&](size_t idx, StringData expected) {
        auto [tag, val] = tsBlock->at(idx);
        ASSERT_FALSE(tsBlock->decompressed())
            << "at(idx) on an interior index must hit _atCache, not deblock or use boundary path";
        ASSERT_EQ(tag, value::TypeTags::bsonString)
            << "Strings >7 chars must materialize via the allocator as bsonString";
        ASSERT_EQ(value::getStringView(tag, val), expected)
            << "Cached string contents must be intact (allocator kept it alive)";
    };
    check(*minIdx, "alpha-001"_sd);
    check(*maxIdx, "delta-004"_sd);
}

TEST_F(TsSbeValueTest, TsBlockCloneStartsWithEmptyCache) {
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithMinMaxAndArrays, "time"_sd, {}, false)
            .compressedBucket;
    ASSERT(compressedBucketOpt);
    auto compressedBucket = *compressedBucketOpt;

    // Use a sparse (non-dense) column so argMin goes through the BSONColumn fast
    // path and populates _atCache without triggering ensureDeblocked().
    auto original = makeTsBlockFromBucket(compressedBucket, "sometimesMissing");
    auto idx = original->argMin();
    ASSERT_TRUE(idx.has_value());
    ASSERT_FALSE(original->decompressed())
        << "argMin on a sparse column must not deblock the original";

    // Clone. The clone must start with an empty _atCache.
    auto cloned = original->cloneStrongTyped();

    // at(*idx) on the clone must deblock: if the cache had been copied from the
    // original, the clone would return the cached value without decompressing.
    (void)cloned->at(*idx);
    ASSERT_TRUE(cloned->decompressed())
        << "clone must deblock on at(argMin): empty cache proves cache was not shared";
}
}  // namespace mongo::sbe
