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

    auto [columnTag, columnVal] = bson::convertFrom<true /* View */>(bucketElem);

    const auto nothing =
        std::pair<value::TypeTags, value::Value>(value::TypeTags::Nothing, value::Value{0u});

    const auto [min, max] = [&]() {
        if (bucket["control"]["min"]) {
            return std::pair(bson::convertFrom<true>(bucket["control"]["min"][fieldName]),
                             bson::convertFrom<true>(bucket["control"]["max"][fieldName]));
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

        const auto expectedMinId = bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["_id"]["0"]);
        const auto expectedMaxId = bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["_id"]["2"]);
        invariant(expectedMinId.first != value::TypeTags::Nothing);
        invariant(expectedMaxId.first != value::TypeTags::Nothing);

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
            bson::convertFrom<true>(kBucketWithMinMaxV1["control"]["min"]["time"]);
        const auto expectedMaxTime =
            bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["time"]["2"]);

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

        const auto expectedMinId = bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["_id"]["0"]);
        const auto expectedMaxId = bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["_id"]["2"]);
        invariant(expectedMinId.first != value::TypeTags::Nothing);
        invariant(expectedMaxId.first != value::TypeTags::Nothing);

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

        const auto expectedMinTime =
            bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["time"]["0"]);
        const auto expectedMaxTime =
            bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["time"]["2"]);
        const auto expectedLowerBoundTime =
            bson::convertFrom<true>(kBucketWithMinMaxV1["control"]["min"]["time"]);

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

        const auto expectedMinId = bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["_id"]["0"]);
        const auto expectedMaxId = bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["_id"]["2"]);

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

        const auto expectedMaxTime =
            bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["time"]["2"]);
        const auto expectedLowerBoundTime =
            bson::convertFrom<true>(kBucketWithMinMaxV1["control"]["min"]["time"]);

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
        bson::convertFrom<true>(kBucketWithMinMaxPre1970["control"]["min"]["time"]);
    const auto expectedUpperBoundTime =
        bson::convertFrom<true>(kBucketWithMinMaxPre1970["control"]["max"]["time"]);

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
        // Already dense fields should return nullptr when fillEmpty'd.
        ASSERT(tsBlock->fillEmpty(value::TypeTags::Null, 0) == nullptr);
    }

    {
        auto tsBlock = makeTsBlockFromBucket(kBucketWithMinMaxAndArrays, "sometimesMissing");
        auto fillRes = tsBlock->fillEmpty(value::TypeTags::Null, 0);
        ASSERT(fillRes);
        auto extracted = fillRes->extract();
        ASSERT_EQ(extracted.count(), 3);
        assertValuesEqual(extracted[0].first,
                          extracted[0].second,
                          value::TypeTags::NumberDouble,
                          value::bitcastFrom<double>(0));
        assertValuesEqual(extracted[1].first, extracted[1].second, value::TypeTags::Null, 0);
        assertValuesEqual(extracted[2].first,
                          extracted[2].second,
                          value::TypeTags::NumberDouble,
                          value::bitcastFrom<double>(9));
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
        ASSERT_EQ(arrBlock->tryLowerBound().first, value::TypeTags::Nothing);
        ASSERT_EQ(arrBlock->tryMax().first, value::TypeTags::Nothing);
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
}  // namespace mongo::sbe
