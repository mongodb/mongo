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
#include "mongo/db/exec/sbe/sbe_unittest.h"
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
        "_id" : {"0" : 0}
    }
})");

TEST_F(SbeValueTest, CloneCreatesIndependentCopy) {
    // A TsCellBlockForTopLevelField can be created in an "unowned" state.
    auto cellBlock = std::make_unique<value::TsCellBlockForTopLevelField>(
        1,     /* count */
        false, /* owned */
        value::TypeTags::bsonObject,
        value::bitcastFrom<const char*>(kSampleBucket["data"]["_id"].embeddedObject().objdata()),
        false, /* isTimefield */
        std::pair<value::TypeTags, value::Value>(value::TypeTags::Nothing, value::Value{0u}),
        std::pair<value::TypeTags, value::Value>(value::TypeTags::Nothing, value::Value{0u}));

    auto& valBlock = cellBlock->getValueBlock();

    const auto expectedTagVal = bson::convertFrom<true>(kSampleBucket["data"]["_id"]["0"]);


    // And we can read its values.
    {
        auto extractedVals = valBlock.extract();
        ASSERT_EQ(extractedVals.count, 1) << "Expected only one value";
        ASSERT_THAT(std::make_pair(extractedVals.tags[0], extractedVals.vals[0]),
                    ValueEq(expectedTagVal))
            << "Expected value extracted from block to be same as original";
    }

    // And we can clone the CellBlock.
    auto cellBlockClone = cellBlock->clone();
    auto valBlockClone = valBlock.clone();

    // And if we destroy the originals, we should still be able to read the clones.
    cellBlock.reset();
    // 'valBlock' is now invalid.

    // If we get the values from the cloned CellBlock, we should see the same data.
    {
        auto& valsFromClonedCellBlock = cellBlockClone->getValueBlock();
        auto extractedVals = valsFromClonedCellBlock.extract();
        ASSERT_THAT(std::make_pair(extractedVals.tags[0], extractedVals.vals[0]),
                    ValueEq(expectedTagVal))
            << "Expected value extracted from cloned CellBlock to be same as original";
    }

    // If we extract the values from the cloned ValueBlock, we should see the same data.
    {
        auto extractedVals = valBlockClone->extract();
        ASSERT_THAT(std::make_pair(extractedVals.tags[0], extractedVals.vals[0]),
                    ValueEq(expectedTagVal))
            << "Expected value from cloned ValueBlock to be same as original";
    }
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
        auto cellBlockId = std::make_unique<value::TsCellBlockForTopLevelField>(
            3,     /* count */
            false, /* owned */
            value::TypeTags::bsonObject,
            value::bitcastFrom<const char*>(
                kBucketWithMinMaxV1["data"]["_id"].embeddedObject().objdata()),
            false, /* isTimefield */
            bson::convertFrom<true>(kBucketWithMinMaxV1["control"]["min"]["_id"]),
            bson::convertFrom<true>(kBucketWithMinMaxV1["control"]["max"]["_id"]));

        auto& valBlock = cellBlockId->getValueBlock();

        const auto expectedMinId = bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["_id"]["0"]);
        const auto expectedMaxId = bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["_id"]["2"]);

        {
            auto [minTag, minVal] = valBlock.tryMin();
            ASSERT_NE(minTag, value::TypeTags::Nothing) << "Expected block min to be non-nothing";
            ASSERT_THAT(std::make_pair(minTag, minVal), ValueEq(expectedMinId))
                << "Expected block min to be the true min val in the block";

            auto [maxTag, maxVal] = valBlock.tryMax();
            ASSERT_NE(maxTag, value::TypeTags::Nothing) << "Expected block max to be non-nothing";
            ASSERT_THAT(std::make_pair(maxTag, maxVal), ValueEq(expectedMaxId))
                << "Expected block max to be the max val in the block";
        }
    }

    {
        auto cellBlockTime = std::make_unique<value::TsCellBlockForTopLevelField>(
            3,     /* count */
            false, /* owned */
            value::TypeTags::bsonObject,
            value::bitcastFrom<const char*>(
                kBucketWithMinMaxV1["data"]["time"].embeddedObject().objdata()),
            true, /* isTimefield */
            bson::convertFrom<true>(kBucketWithMinMaxV1["control"]["min"]["time"]),
            bson::convertFrom<true>(kBucketWithMinMaxV1["control"]["max"]["time"]));

        auto& valBlock = cellBlockTime->getValueBlock();

        const auto expectedMaxTime =
            bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["time"]["2"]);

        {
            auto [minTag, minVal] = valBlock.tryMin();
            // The min time field value for v1 buckets cannot be determined in O(1), so tryMin()
            // should return Nothing.
            ASSERT_EQ(minTag, value::TypeTags::Nothing)
                << "Expected block min of the time field to be nothing";

            auto [maxTag, maxVal] = valBlock.tryMax();
            ASSERT_NE(maxTag, value::TypeTags::Nothing) << "Expected block max to be non-nothing";
            ASSERT_THAT(std::make_pair(maxTag, maxVal), ValueEq(expectedMaxTime))
                << "Expected block max for to be the max val in the block";
        }
    }
}

TEST_F(SbeValueTest, TsBlockMinMaxV2Schema) {
    auto compressedBucketOpt =
        timeseries::compressBucket(kBucketWithMinMaxV1, "time"_sd, {}, false).compressedBucket;
    ASSERT(compressedBucketOpt) << "Should have been able to create compressed v2 bucket";
    auto compressedBucket = *compressedBucketOpt;

    {
        auto [columnTag, columnVal] =
            bson::convertFrom<true /* View */>(compressedBucket["data"]["_id"]);
        auto cellBlockId = std::make_unique<value::TsCellBlockForTopLevelField>(
            3,     /* count */
            false, /* owned */
            columnTag,
            columnVal,
            false, /* isTimefield */
            bson::convertFrom<true>(compressedBucket["control"]["min"]["_id"]),
            bson::convertFrom<true>(compressedBucket["control"]["max"]["_id"]));

        auto& valBlock = cellBlockId->getValueBlock();

        const auto expectedMinId = bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["_id"]["0"]);
        const auto expectedMaxId = bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["_id"]["2"]);

        {
            auto [minTag, minVal] = valBlock.tryMin();
            ASSERT_NE(minTag, value::TypeTags::Nothing) << "Expected block min to be non-nothing";
            ASSERT_THAT(std::make_pair(minTag, minVal), ValueEq(expectedMinId))
                << "Expected block min to be the true min val in the block";

            auto [maxTag, maxVal] = valBlock.tryMax();
            ASSERT_NE(maxTag, value::TypeTags::Nothing) << "Expected block max to be non-nothing";
            ASSERT_THAT(std::make_pair(maxTag, maxVal), ValueEq(expectedMaxId))
                << "Expected block max to be the max val in the block";
        }
    }

    {
        auto [columnTag, columnVal] =
            bson::convertFrom<true /* View */>(compressedBucket["data"]["time"]);
        auto cellBlockTime = std::make_unique<value::TsCellBlockForTopLevelField>(
            3,     /* count */
            false, /* owned */
            columnTag,
            columnVal,
            true, /* isTimefield */
            bson::convertFrom<true>(compressedBucket["control"]["min"]["time"]),
            bson::convertFrom<true>(compressedBucket["control"]["max"]["time"]));

        auto& valBlock = cellBlockTime->getValueBlock();

        const auto expectedMinTime =
            bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["time"]["0"]);
        const auto expectedMaxTime =
            bson::convertFrom<true>(kBucketWithMinMaxV1["data"]["time"]["2"]);

        {
            // The min time field value for v2 buckets can be determined in O(1), so tryMin() should
            // return the true min.
            auto [minTag, minVal] = valBlock.tryMin();
            ASSERT_NE(minTag, value::TypeTags::Nothing)
                << "Expected block min of the time field to be non-nothing";
            ASSERT_THAT(std::make_pair(minTag, minVal), ValueEq(expectedMinTime))
                << "Expected block min of the time field to be the true min";

            auto [maxTag, maxVal] = valBlock.tryMax();
            ASSERT_NE(maxTag, value::TypeTags::Nothing) << "Expected block max to be non-nothing";
            ASSERT_THAT(std::make_pair(maxTag, maxVal), ValueEq(expectedMaxTime))
                << "Expected block max of the time field to be the max val in the block";
        }
    }
}

}  // namespace mongo::sbe
