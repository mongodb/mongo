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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/ts_block.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::sbe {

class SbeValueTest : public SbeStageBuilderTestFixture {};

// This is just a made up example, and is not actually a valid bucket. There's no min/max and
// no time field.
const BSONObj kSampleBucket = fromjson(R"(
{
    "_id" : ObjectId("649f0704230f18da067519c4"),
    "control" : {"version" : 1},
    "meta" : "A",
    "data" : {
        "_id" : {"0" : 0, "1": 1, "2": 2, "3": 3}
    }
})");

int getBucketVersion(const BSONObj& bucket) {
    return bucket[timeseries::kBucketControlFieldName].embeddedObject().getIntField(
        timeseries::kBucketControlVersionFieldName);
}

std::unique_ptr<value::TsBlock> makeTsBlockFromBucket(const BSONObj& bucket, StringData fieldName) {
    auto bucketElem = bucket["data"][fieldName];
    const auto nFields = [&]() -> size_t {
        if (bucketElem.type() == BSONType::Object) {
            return bucketElem.embeddedObject().nFields();
        } else {
            invariant(bucketElem.type() == BSONType::BinData);
            BSONColumn col(bucketElem);
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


TEST_F(SbeValueTest, CloneCreatesIndependentCopy) {
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
		"_id" : {"1": 1, "0" : 0, "2" : 2},
		"time" : {
			"1" : {$date: "2023-06-30T21:29:09.968Z"},
            "0" : {$date: "2023-06-30T21:29:00.568Z"},
			"2" : {$date: "2023-06-30T21:29:15.088Z"}
		}
	}
})");

TEST_F(SbeValueTest, TsBlockMinMaxV1Schema) {
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

TEST_F(SbeValueTest, TsBlockMinMaxV2Schema) {
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

TEST_F(SbeValueTest, TsBlockMinMaxV3Schema) {
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
}  // namespace mongo::sbe
