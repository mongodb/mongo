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

/**
 * This file contains tests for sbe::TsBlockToCellBlockStage and sbe::BlockToRowStage.
 */

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/block_to_row.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/stages/ts_bucket_to_cell_block.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/unittest/assert.h"

namespace mongo::sbe {
class BlockStagesTest : public PlanStageTestFixture {
protected:
    BSONObj compressBucket(const BSONObj& bucket) {
        return *timeseries::compressBucket(bucket, /*timeFieldName*/ "time"_sd, /*nss*/ {}, false)
                    .compressedBucket;
    }

    std::tuple<std::unique_ptr<PlanStage>, value::SlotVector /*outSlots*/> makeBlockToRow(
        std::unique_ptr<PlanStage> input, value::SlotVector blockSlots) {
        auto outSlots = generateMultipleSlotIds(blockSlots.size());
        auto blockToRowStage =
            makeS<BlockToRowStage>(std::move(input), std::move(blockSlots), outSlots, 1 /*nodeId*/);
        return {std::move(blockToRowStage), std::move(outSlots)};
    }

    std::tuple<std::unique_ptr<PlanStage>,
               value::SlotVector /*blockSlots*/,
               boost::optional<value::SlotId> /*metaSlot*/>
    makeTsBucketToCellBlock(std::unique_ptr<PlanStage> input,
                            value::SlotId inSlot,
                            const std::vector<std::string>& cellPaths,
                            const TimeseriesOptions& tsOptions) {
        // Builds a TsBucketToCellBlockStage on top of the input stage.
        auto blockSlots = generateMultipleSlotIds(cellPaths.size());
        const auto metaSlot = tsOptions.getMetaField()
            ? boost::make_optional<value::SlotId>(generateSlotId())
            : boost::none;
        auto tsBucketStage = makeS<TsBucketToCellBlockStage>(std::move(input),
                                                             inSlot,
                                                             cellPaths,
                                                             blockSlots,
                                                             metaSlot,
                                                             metaSlot.has_value() /*hasMetaField*/,
                                                             tsOptions.getTimeField().toString(),
                                                             1 /*nodeId*/);

        return {std::move(tsBucketStage), std::move(blockSlots), metaSlot};
    }

    std::tuple<std::unique_ptr<PlanStage>, value::SlotVector, boost::optional<value::SlotId>>
    generateTsBucketToCellBlockOnVirtualScan(const BSONArray& inputDocs,
                                             const TimeseriesOptions& tsOptions,
                                             const std::vector<std::string>& cellPaths) {
        // Builds a scan for the (imaginary) buckets collection.
        auto [scanSlot, scanStage] = generateVirtualScan(inputDocs);

        // Builds a TsBucketToCellBlockStage on top of the scan.
        return makeTsBucketToCellBlock(std::move(scanStage), scanSlot, cellPaths, tsOptions);
    }

    std::tuple<std::unique_ptr<PlanStage>, value::SlotVector, boost::optional<value::SlotId>>
    generateUnpackBucketsOnVirtualScan(const BSONArray& inputDocs,
                                       const TimeseriesOptions& tsOptions,
                                       const std::vector<std::string>& cellPaths) {
        // Builds a scan for the (imaginary) buckets collection.
        auto [scanSlot, scanStage] = generateVirtualScan(inputDocs);

        // Builds a TsBucketToCellBlockStage on top of the scan.
        auto [tsBucketStage, blockSlots, metaSlot] =
            makeTsBucketToCellBlock(std::move(scanStage), scanSlot, cellPaths, tsOptions);

        // Builds a BlockToRowStage on top of the TsBucketToCellBlockStage.
        auto [bucketToRow, outSlots] =
            makeBlockToRow(std::move(tsBucketStage), std::move(blockSlots));

        return {std::move(bucketToRow), std::move(outSlots), metaSlot};
    }

    void verifyUnpackBucket(std::unique_ptr<PlanStage> blockToRow,
                            const std::vector<std::string>& cellPaths,
                            const value::SlotVector& outSlots,
                            const boost::optional<value::SlotId>& metaSlot,
                            const std::vector<BSONObj>& expectedData) {
        // Prepares the execution tree.
        auto ctx = makeCompileCtx();
        prepareTree(ctx.get(), blockToRow.get());

        // Sets up accessors for results from the BlockToRow stage.
        auto metaAccessor = metaSlot ? blockToRow->getAccessor(*ctx, *metaSlot) : nullptr;
        std::vector<value::SlotAccessor*> outAccessors(outSlots.size(), nullptr);
        for (size_t i = 0; i < outSlots.size(); ++i) {
            outAccessors[i] = blockToRow->getAccessor(*ctx, outSlots[i]);
            ASSERT_NE(nullptr, outAccessors[i]);
        }

        size_t i = 0;
        for (auto st = blockToRow->getNext(); st == PlanState::ADVANCED;
             st = blockToRow->getNext(), ++i) {
            // Verifies meta field.
            if (metaAccessor) {
                auto [metaTag, metaVal] = metaAccessor->getViewOfValue();
                auto [expectedTag, expectedVal] = bson::convertFrom<true>(expectedData[i]["tag"]);
                assertValuesEqual(expectedTag, expectedVal, metaTag, metaVal);
            }

            // Verifies rows.
            for (size_t j = 0; j < cellPaths.size(); ++j) {
                auto [actualTag, actualVal] = outAccessors[j]->getViewOfValue();
                auto [expectedTag, expectedVal] =
                    bson::convertFrom<true>(expectedData[i][cellPaths[j]]);

                assertValuesEqual(expectedTag, expectedVal, actualTag, actualVal);
            }
        }
        ASSERT_EQ(expectedData.size(), i);
    }

    // Builds TS bucket unpacking stages on top of an imaginary buckets collection 'inputDocs' and
    // verifies the results against 'expectedData' which is a vector of BSONObj whose field names
    // must conform to 'cellPaths'.
    //
    // The generated plan shape will look like:
    //  [1] block_to_row paths[blocksOut[0], ..., blocksOut[N]]
    //        vals[valsOut[0], ..., valsOut[N]]
    //  [1] ts_bucket_to_cellblock bucketSlot
    //        paths[blocksOut[0] = cellPaths[0], ..., blocksOut[N] = cellPaths[N]]
    //        meta = metaSlot?
    //  [0] virtual_scan bucketSlot [from 'inputDocs']
    //
    // The 'inputDocs' must be of timeseries bucket format, either compressed or uncompressed.
    void runUnpackBucketTest(BSONArray inputDocs,
                             const std::vector<std::string>& cellPaths,
                             const TimeseriesOptions& tsOptions,
                             const std::vector<BSONObj>& expectedData) {
        auto [blockToRow, outSlots, metaSlot] =
            generateUnpackBucketsOnVirtualScan(inputDocs, tsOptions, cellPaths);
        verifyUnpackBucket(std::move(blockToRow), cellPaths, outSlots, metaSlot, expectedData);
    }
};

// Stages under tests do not require 'control.min' and 'control.max' fields to be present though
// they are mandatory fields. This data is not valid timeseries data.
const BSONObj bucketWithMeta1 = fromjson(R"(
{
    "_id" : ObjectId("649f0704230f18da067519c4"),
    "control" : {"version" : 1},
    "meta" : "A",
    "data" : {
        "_id" : {"0" : 0}, "f" : {"0" : 0}, "time" : {"0" : {$date: "2023-06-30T16:47:09.512Z"}}}
    }
})");

// Stages under tests do not require 'control.min' and 'control.max' fields to be present though
// they are mandatory fields. This data is not valid timeseries data.
const BSONObj bucketWithMeta2 = fromjson(R"(
{
    "_id" : ObjectId("649f0704c3d83a4c3fe91689"),
    "control" : {"version" : 1},
    "meta" : "B",
    "data" : {
        "time" : {"0" : {$date: "2023-06-30T16:47:38.692Z"}, "1" : {$date: "2023-06-30T16:47:47.918Z"}},
        "_id" : {"0" : 3, "1" : 4},
        "f" : {"0" : 100, "1" : 101}
    }
})");

// Verifies that the TsBucketToCellBlockStage exposes the meta field correctly and produces
// 'CellBlock's.
TEST_F(BlockStagesTest, TsBucketToCellBlockStageTest) {
    // Builds a TsBucketToCellBlock stage on top of an imaginary buckets collection.
    std::vector<std::string> cellPaths{{"time"}, {"_id"}, {"f"}};
    auto tsOptions =
        TimeseriesOptions::parse(IDLParserContext{"BlockStagesTest::TsBucketToCellBlockSanityTest"},
                                 fromjson(R"({timeField: "time", metaField: "tag"})"));
    auto [tsBucketStage, blockSlots, metaSlot] = generateTsBucketToCellBlockOnVirtualScan(
        BSON_ARRAY(bucketWithMeta1 << bucketWithMeta2), tsOptions, cellPaths);

    // Prepares the execution tree.
    auto ctx = makeCompileCtx();
    prepareTree(ctx.get(), tsBucketStage.get());

    // Sets up accessors for results from the TsBucketToCellBlock stage.
    auto metaAccessor = tsBucketStage->getAccessor(*ctx, *metaSlot);
    std::vector<value::SlotAccessor*> blockAccessors(blockSlots.size(), nullptr);
    for (size_t i = 0; i < blockSlots.size(); ++i) {
        blockAccessors[i] = tsBucketStage->getAccessor(*ctx, blockSlots[i]);
    }

    std::vector expectedData{bucketWithMeta1, bucketWithMeta2};

    size_t i = 0;
    for (auto st = tsBucketStage->getNext(); st == PlanState::ADVANCED;
         st = tsBucketStage->getNext(), ++i) {
        // Verifies meta field.
        auto [metaTag, metaVal] = metaAccessor->getViewOfValue();
        auto [expectedTag, expectedVal] = bson::convertFrom<true>(expectedData[i]["meta"]);
        assertValuesEqual(expectedTag, expectedVal, metaTag, metaVal);

        // Verifies that cell blocks are produced for requested `cellPaths`.
        // We don't verify the actual values of the cell blocks here, as that is done in the
        // 'UnpackBucket*' tests below.
        for (size_t j = 0; j < cellPaths.size(); ++j) {
            // Verifies that each value is of cell block type.
            auto [tag, val] = blockAccessors[j]->getViewOfValue();
            ASSERT_EQ(value::TypeTags::cellBlock, tag);
        }
    }
    ASSERT_EQ(expectedData.size(), i);
}

const auto bucketsWithSameSchemaMeasurements = BSON_ARRAY(bucketWithMeta1 << bucketWithMeta2);
const auto cellPathsForBucketsWithSameSchemaMeasurements =
    std::vector<std::string>{{"time"}, {"_id"}, {"f"}};
const auto tsOptionsForBucketsWithSameSchemaMeasurements = TimeseriesOptions::parse(
    IDLParserContext{"BlockStagesTest::UnpackTwoBucketsWithSameSchemaMeasurements"},
    fromjson(R"({timeField: "time", metaField: "tag"})"));
const auto expectedDataForBucketsWithSameSchemaMeasurements = std::vector{
    fromjson(R"({"_id" : 0, "tag" : "A", "time" : {$date: "2023-06-30T16:47:09.512Z"}, "f" : 0})"),
    fromjson(
        R"({"_id" : 3, "tag" : "B", "time" : {$date: "2023-06-30T16:47:38.692Z"}, "f" : 100})"),
    fromjson(
        R"({"_id" : 4, "tag" : "B", "time" : {$date: "2023-06-30T16:47:47.918Z"}, "f" : 101})"),
};

TEST_F(BlockStagesTest, UnpackTwoBucketsWithSameSchemaMeasurements) {
    runUnpackBucketTest(bucketsWithSameSchemaMeasurements,
                        cellPathsForBucketsWithSameSchemaMeasurements,
                        tsOptionsForBucketsWithSameSchemaMeasurements,
                        expectedDataForBucketsWithSameSchemaMeasurements);
}

const auto compressedBucketsWithSameSchemaMeasurements =
    BSON_ARRAY(bucketWithMeta1 << bucketWithMeta2);
TEST_F(BlockStagesTest, Unpack_Compressed_TwoBucketsWithSameSchemaMeasurements) {
    runUnpackBucketTest(compressedBucketsWithSameSchemaMeasurements,
                        cellPathsForBucketsWithSameSchemaMeasurements,
                        tsOptionsForBucketsWithSameSchemaMeasurements,
                        expectedDataForBucketsWithSameSchemaMeasurements);
}

// Stages under tests do not require 'control.min' and 'control.max' fields to be present though
// they are mandatory fields. This data is not valid timeseries data.
const BSONObj bucketWithNoMeta = fromjson(R"(
{
	"_id" : ObjectId("64a5cb841ade1be79f4cc8c7"),
	"control" : {"version" : 1},
	"data" : {
		"foo" : {"0" : "A", "1" : "A", "2" : "B"},
		"time" : {
			"0" : {$date: "2023-07-05T19:59:28.339Z"},
			"1" : {$date: "2023-07-05T19:59:38.396Z"},
			"2" : {$date: "2023-07-05T19:59:50.772Z"}
		}
	}
})");
const auto cellPathsForBucketWithNoMeta = std::vector<std::string>{{"foo"}, {"time"}};
// No 'meta' field for 'tsOptions' parameter.
const auto tsOptionsForBucketWithNoMeta =
    TimeseriesOptions::parse(IDLParserContext{"BlockStagesTest::UnpackBucketWithNoMeta"},
                             fromjson(R"({timeField: "time"})"));
const auto expectedDataForBucketWithNoMeta = std::vector{
    fromjson(R"({"time" : {$date: "2023-07-05T19:59:28.339Z"}, "foo" : "A"})"),
    fromjson(R"({"time" : {$date: "2023-07-05T19:59:38.396Z"}, "foo" : "A"})"),
    fromjson(R"({"time" : {$date: "2023-07-05T19:59:50.772Z"}, "foo" : "B"})"),
};

TEST_F(BlockStagesTest, UnpackBucketWithNoMeta) {
    runUnpackBucketTest(BSON_ARRAY(bucketWithNoMeta),
                        cellPathsForBucketWithNoMeta,
                        tsOptionsForBucketWithNoMeta,
                        expectedDataForBucketWithNoMeta);
}

TEST_F(BlockStagesTest, Unpack_Compressed_BucketWithNoMeta) {
    runUnpackBucketTest(BSON_ARRAY(compressBucket(bucketWithNoMeta)),
                        cellPathsForBucketWithNoMeta,
                        tsOptionsForBucketWithNoMeta,
                        expectedDataForBucketWithNoMeta);
}

// Stages under tests do not require 'control.min' and 'control.max' fields to be present though
// they are mandatory fields. This data is not valid timeseries data.
const BSONObj bucketWithOneMissingField = fromjson(R"(
{
	"_id" : ObjectId("64a33d9cdf56a62781061048"),
	"control" : {"version" : 1},
	"meta" : "A",
	"data" : {
		"_id" : {"1" : 1, "2" : 2},
		"time" : {
			"0" : {$date: "2023-06-30T21:29:00.568Z"},
			"1" : {$date: "2023-06-30T21:29:09.968Z"},
			"2" : {$date: "2023-06-30T21:29:15.088Z"}
		}
	}
})");
const auto cellPathsForBucketWithOneMissingField = std::vector<std::string>{{"_id"}};
const auto tsOptionsForBucketWithOneMissingField =
    TimeseriesOptions::parse(IDLParserContext{"BlockStagesTest::UnpackBucketWithOneMissingField"},
                             fromjson(R"({timeField: "time"})"));
const auto expectedDataForBucketWithOneMissingField = std::vector{
    fromjson(R"({})"),
    fromjson(R"({"_id" : 1})"),
    fromjson(R"({"_id" : 2})"),
};

TEST_F(BlockStagesTest, UnpackBucketWithOneMissingField) {
    runUnpackBucketTest(BSON_ARRAY(bucketWithOneMissingField),
                        cellPathsForBucketWithOneMissingField,
                        tsOptionsForBucketWithOneMissingField,
                        expectedDataForBucketWithOneMissingField);
}

TEST_F(BlockStagesTest, Unpack_Compressed_BucketWithOneMissingField) {
    runUnpackBucketTest(BSON_ARRAY(compressBucket(bucketWithOneMissingField)),
                        cellPathsForBucketWithOneMissingField,
                        tsOptionsForBucketWithOneMissingField,
                        expectedDataForBucketWithOneMissingField);
}

// Stages under tests do not require 'control.min' and 'control.max' fields to be present though
// they are mandatory fields. This data is not valid timeseries data.
const BSONObj bucketWithMultipleMissingFields = fromjson(R"(
{
    "_id" : ObjectId("64a33d9cdf56a62781061048"),
    "control" : {"version" : 1},
    "meta" : "A",
    "data" : {
        "_id" : {"1" : 1, "2" : 2},
        "f" : {"0" : 0},
        "g" : {"0" : 100, "2" : 102},
        "time" : {
            "0" : {$date: "2023-06-30T21:29:00.568Z"},
            "1" : {$date: "2023-06-30T21:29:09.968Z"},
            "2" : {$date: "2023-06-30T21:29:15.088Z"}
        }
    }
})");
const auto cellPathsForBucketWithMultipleMissingFields =
    std::vector<std::string>{{"time"}, {"_id"}, {"f"}, {"g"}};
const auto tsOptionsForBucketWithMultipleMissingFields = TimeseriesOptions::parse(
    IDLParserContext{"BlockStagesTest::UnpackBucketWithMultipleMissingFields"},
    fromjson(R"({timeField: "time", metaField: "tag"})"));
const auto expectedDataForBucketWithMultipleMissingFields = std::vector{
    // The 1st document is missing the '_id' field.
    fromjson(R"({"tag" : "A", "time" : {$date: "2023-06-30T21:29:00.568Z"}, "f" : 0, "g" : 100})"),
    // The 2nd documents is missing the 'f' and 'g' fields.
    fromjson(R"({"tag" : "A", "time" : {$date: "2023-06-30T21:29:09.968Z"}, "_id" : 1})"),
    // The 3rd documents is missing the 'f' fields.
    fromjson(
        R"({"tag" : "A", "time" : {$date: "2023-06-30T21:29:15.088Z"}, "_id" : 2, "g" : 102})"),
};

TEST_F(BlockStagesTest, UnpackBucketWithMultipleMissingFields) {
    runUnpackBucketTest(BSON_ARRAY(bucketWithMultipleMissingFields),
                        cellPathsForBucketWithMultipleMissingFields,
                        tsOptionsForBucketWithMultipleMissingFields,
                        expectedDataForBucketWithMultipleMissingFields);
}

TEST_F(BlockStagesTest, Unpack_Compressed_BucketWithMultipleMissingFields) {
    runUnpackBucketTest(BSON_ARRAY(compressBucket(bucketWithMultipleMissingFields)),
                        cellPathsForBucketWithMultipleMissingFields,
                        tsOptionsForBucketWithMultipleMissingFields,
                        expectedDataForBucketWithMultipleMissingFields);
}

// Stages under tests do not require 'control.min' and 'control.max' fields to be present though
// they are mandatory fields. This data is not valid timeseries data.
const BSONObj bucketLeadingToEmptyBlock = fromjson(R"(
{
	"_id" : ObjectId("64a33d9cdf56a62781061048"),
	"control" : {"version" : 1},
	"meta" : "A",
	"data" : {
		"_id" : {"0" : 0},
		"time" : {"0" : {$date: "2023-06-30T21:29:00.568Z"}}
	}
})");
const auto cellPathsForBucketLeadingToEmptyBlock =
    std::vector<std::string>{{"time"}, {"_id"}, {"absent_field"}};
const auto tsOptionsForBucketLeadingToEmptyBlock =
    TimeseriesOptions::parse(IDLParserContext{"BlockStagesTest::UnpackBucketLeadingToEmptyBlock"},
                             fromjson(R"({timeField: "time", metaField: "tag"})"));
const auto expectedDataForBucketLeadingToEmptyBlock = std::vector{
    fromjson(R"({"_id" : 0, "tag" : "A", "time" : {$date: "2023-06-30T21:29:00.568Z"}})"),
};

TEST_F(BlockStagesTest, UnpackBucketLeadingToEmptyBlock) {
    runUnpackBucketTest(BSON_ARRAY(bucketLeadingToEmptyBlock),
                        cellPathsForBucketLeadingToEmptyBlock,
                        tsOptionsForBucketLeadingToEmptyBlock,
                        expectedDataForBucketLeadingToEmptyBlock);
}

TEST_F(BlockStagesTest, Unpack_Compressed_BucketLeadingToEmptyBlock) {
    runUnpackBucketTest(BSON_ARRAY(compressBucket(bucketLeadingToEmptyBlock)),
                        cellPathsForBucketLeadingToEmptyBlock,
                        tsOptionsForBucketLeadingToEmptyBlock,
                        expectedDataForBucketLeadingToEmptyBlock);
}

// Stages under tests do not require 'control.min' and 'control.max' fields to be present though
// they are mandatory fields. This data is not valid timeseries data.
const BSONObj bucketWithArrayField = fromjson(R"(
{
	"_id" : ObjectId("64a6250cc12c637f4b973abf"),
	"control" : {"version" : 1},
	"meta" : "A",
	"data" : {
		"time" : {
			"0" : {$date: "2023-07-06T02:21:11.454Z"},
			"1" : {$date: "2023-07-06T02:21:23.655Z"},
			"2" : {$date: "2023-07-06T02:21:43.388Z"}
		},
		"_id" : {"0" : 0, "1" : 2, "2" : -1},
		"f" : {"0" : [1, 2, 3], "1" : [4, 5], "2" : [100]}
	}
})");
const auto cellPathsForBucketWithArrayField = std::vector<std::string>{{"time"}, {"_id"}, {"f"}};
const auto tsOptionsForBucketWithArrayField =
    TimeseriesOptions::parse(IDLParserContext{"BlockStagesTest::UnpackBucketWithArrayField"},
                             fromjson(R"({timeField: "time", metaField: "tag"})"));
const auto expectedDataForBucketWithArrayField = std::vector{
    fromjson(
        R"({"time" : {$date: "2023-07-06T02:21:11.454Z"}, "tag" : "A", "_id" : 0, "f" : [1, 2, 3]})"),
    fromjson(
        R"({"time" : {$date: "2023-07-06T02:21:23.655Z"}, "tag" : "A", "_id" : 2, "f" : [4, 5 ]})"),
    fromjson(
        R"({"time" : {$date: "2023-07-06T02:21:43.388Z"}, "tag" : "A", "_id" : -1, "f" : [100]})"),
};

TEST_F(BlockStagesTest, UnpackBucketWithArrayField) {
    runUnpackBucketTest(BSON_ARRAY(bucketWithArrayField),
                        cellPathsForBucketWithArrayField,
                        tsOptionsForBucketWithArrayField,
                        expectedDataForBucketWithArrayField);
}

TEST_F(BlockStagesTest, Unpack_Compressed_BucketWithArrayField) {
    runUnpackBucketTest(BSON_ARRAY(compressBucket(bucketWithArrayField)),
                        cellPathsForBucketWithArrayField,
                        tsOptionsForBucketWithArrayField,
                        expectedDataForBucketWithArrayField);
}

// Stages under tests do not require 'control.min' and 'control.max' fields to be present though
// they are mandatory fields. This data is not valid timeseries data.
const BSONObj bucketWithObjectField = fromjson(R"(
{
	"_id" : ObjectId("64a6289082372d5621d421f1"),
	"control" : {"version" : 1},
	"meta" : "A",
	"data" : {
		"time" : {
			"0" : {$date: "2023-07-06T02:36:53.999Z"},
			"1" : {$date: "2023-07-06T02:37:14.226Z"},
			"2" : {$date: "2023-07-06T02:37:34.589Z"}
		},
		"_id" : {"0" : 0, "1" : 2, "2" : -23},
		"f" : {
			"0" : {"a" : 0, "b" : 0},
			"1" : {"a" : 100, "c" : -100},
			"2" : {"a" : 1000, "b" : 2}
		}
	}
})");
const auto cellPathsForBucketWithObjectField = std::vector<std::string>{{"time"}, {"_id"}, {"f"}};
const auto tsOptionsForBucketWithObjectField =
    TimeseriesOptions::parse(IDLParserContext{"BlockStagesTest::UnpackBucketWithObjectField"},
                             fromjson(R"({timeField: "time", metaField: "tag"})"));
const auto expectedDataForBucketWithObjectField = std::vector{
    fromjson(R"(
{"time" : {$date: "2023-07-06T02:36:53.999Z"}, "tag" : "A", "_id" : 0, "f" : {"a" : 0, "b" : 0}}
        )"),
    fromjson(R"(
{"time" : {$date: "2023-07-06T02:37:14.226Z"}, "tag" : "A", "_id" : 2, "f" : {"a" : 100, "c" : -100}}
        )"),
    fromjson(R"(
{"time" : {$date: "2023-07-06T02:37:34.589Z"}, "tag" : "A", "_id" : -23, "f" : {"a" : 1000, "b" : 2}}
        )"),
};

TEST_F(BlockStagesTest, UnpackBucketWithObjectField) {
    runUnpackBucketTest(BSON_ARRAY(bucketWithObjectField),
                        cellPathsForBucketWithObjectField,
                        tsOptionsForBucketWithObjectField,
                        expectedDataForBucketWithObjectField);
}

TEST_F(BlockStagesTest, Unpack_Compressed_BucketWithObjectField) {
    runUnpackBucketTest(BSON_ARRAY(compressBucket(bucketWithObjectField)),
                        cellPathsForBucketWithObjectField,
                        tsOptionsForBucketWithObjectField,
                        expectedDataForBucketWithObjectField);
}
}  // namespace mongo::sbe
