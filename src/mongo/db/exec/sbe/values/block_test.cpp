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

#include "mongo/bson/json.h"
#include "mongo/bson/util/bsoncolumnbuilder.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/bson_block.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/scalar_mono_cell_block.h"
#include "mongo/db/exec/sbe/values/ts_block.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/itoa.h"

namespace mongo::sbe {

using TypeTags = value::TypeTags;
using Value = value::Value;
using ColumnOpType = value::ColumnOpType;

class SbeValueTest : public SbeStageBuilderTestFixture {};

// Tests that copyValue() behaves correctly when given a TypeTags::valueBlock. Uses MonoBlock as
// the concrete block type.
TEST_F(SbeValueTest, SbeValueBlockTypeIsCopyable) {
    value::MonoBlock block(1, TypeTags::NumberInt32, value::bitcastFrom<int32_t>(123));

    auto [cpyTag, cpyValue] =
        value::copyValue(TypeTags::valueBlock, value::bitcastFrom<value::MonoBlock*>(&block));
    value::ValueGuard cpyGuard(cpyTag, cpyValue);
    ASSERT_EQ(cpyTag, TypeTags::valueBlock);
    auto cpy = value::getValueBlock(cpyValue);

    auto extracted = cpy->extract();
    ASSERT_EQ(extracted.count(), 1);
}

// Tests that copyValue() behaves correctly when given a TypeTags::valueBlock. Uses MonoBlock as
// the concrete block type.
TEST_F(SbeValueTest, SbeCellBlockTypeIsCopyable) {
    value::ScalarMonoCellBlock block(1, TypeTags::NumberInt32, value::bitcastFrom<int32_t>(123));

    auto [cpyTag, cpyValue] = value::copyValue(
        TypeTags::cellBlock, value::bitcastFrom<value::ScalarMonoCellBlock*>(&block));
    value::ValueGuard cpyGuard(cpyTag, cpyValue);
    ASSERT_EQ(cpyTag, TypeTags::cellBlock);
    auto cpy = value::getCellBlock(cpyValue);

    auto& vals = cpy->getValueBlock();
    auto extracted = vals.extract();
    ASSERT_EQ(extracted.count(), 1);
}

namespace {
// Other types and helpers for testing.
struct PathTestCase {
    value::CellBlock::Path path;
    BSONObj filterValues;
    std::vector<int32_t> filterPosInfo;

    BSONObj projectValues;
};

// Converts a block to a BSON object of the form {"result": <array of values>}.
// Nothing values (which are valid in a block) are represented with bson NULL.
BSONObj blockToBsonArr(value::ValueBlock& block) {
    auto extracted = block.extract();
    BSONArrayBuilder arr;
    for (size_t i = 0; i < extracted.count(); ++i) {
        auto tag = extracted.tags()[i];
        auto val = extracted.vals()[i];

        BSONObjBuilder tmp;

        if (tag == TypeTags::Nothing) {
            // Use null as a fill value.
            tmp.appendNull("foo");
        } else {
            bson::appendValueToBsonObj(tmp, "foo", tag, val);
        }

        arr.append(tmp.asTempObj().firstElement());
    }
    return BSON("result" << arr.arr());
}

// Converts a vector of char storing char(1)/char(0) into an ascii string of '1' and '0'.
std::string posInfoToString(const std::vector<int32_t>& posInfo) {
    std::string out;
    for (auto c : posInfo) {
        out += std::to_string(c);
        out += " ";
    }
    return out;
}
}  // namespace

class BsonBlockDecodingTest : public mongo::unittest::Test {
public:
    void run() {
        auto base = static_cast<mongo::unittest::Test*>(this);

        // First run the tests using the direct BSON implementation for extracting paths.
        base->run();

        // Then run the tests with the time series implementation.
        useTsImpl = true;
        base->run();
    }

    std::pair<std::vector<std::unique_ptr<value::TsBlock>>,
              std::vector<std::unique_ptr<value::CellBlock>>>
    extractCellBlocks(const std::vector<value::CellBlock::PathRequest>& paths,
                      const std::vector<BSONObj>& bsons) {

        if (useTsImpl) {
            // Shred the bsons here, produce a time series "bucket"-like thing, and pass it to the
            // TS decoding implementation.
            StringMap<std::unique_ptr<BSONColumnBuilder<>>> shredMap;

            size_t bsonIdx = 0;
            for (auto&& bson : bsons) {
                // Keep track of which fields we visited for this bson, so we can pad the
                // unvisited fields with 'gaps'.
                StringDataSet fieldsVisited;
                for (auto elt : bson) {
                    auto [it, inserted] = shredMap.insert(
                        std::pair(elt.fieldName(), std::make_unique<BSONColumnBuilder<>>()));

                    if (inserted) {
                        // Backfill with missing values.
                        for (size_t i = 0; i < bsonIdx; ++i) {
                            it->second->append(BSONElement());
                        }
                    }

                    it->second->append(elt);
                    fieldsVisited.insert(elt.fieldNameStringData());
                }

                // Fill in missings for fields not present in this document.
                for (auto& [k, v] : shredMap) {
                    if (!fieldsVisited.count(k)) {
                        v->append(BSONElement());
                    }
                }

                ++bsonIdx;
            }

            BSONObjBuilder dataFieldBuilder;
            for (auto& [fieldName, builder] : shredMap) {
                dataFieldBuilder.append(fieldName, builder->finalize());
            }

            // Store the bucket into a member variable so that the memory remains valid for the
            // rest of the test.
            _bucketStorage =
                BSON(timeseries::kBucketControlFieldName
                     << BSON(timeseries::kBucketControlCountFieldName << (long long)bsons.size())
                     << timeseries::kBucketDataFieldName << dataFieldBuilder.obj());
            // Now call into the time series extractor.

            value::TsBucketPathExtractor extractor(paths, "time");
            auto [n, storageBlocks, cellBlocks] = extractor.extractCellBlocks(_bucketStorage);
            return {std::move(storageBlocks), std::move(cellBlocks)};
        } else {
            return std::pair(std::vector<std::unique_ptr<value::TsBlock>>(),
                             value::extractCellBlocksFromBsons(paths, bsons));
        }
    }

    void testPaths(const std::vector<PathTestCase>& testCases, const std::vector<BSONObj>& bsons);

private:
    BSONObj _bucketStorage;

    bool useTsImpl = false;
};

void BsonBlockDecodingTest::testPaths(const std::vector<PathTestCase>& testCases,
                                      const std::vector<BSONObj>& bsons) {
    std::vector<value::CellBlock::PathRequest> pathReqs;
    for (auto& tc : testCases) {
        pathReqs.push_back(
            value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter, tc.path));
        pathReqs.push_back(
            value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kProject, tc.path));
    }

    auto [tsBlocks, cellBlocks] = extractCellBlocks(pathReqs, bsons);
    ASSERT_EQ(cellBlocks.size(), pathReqs.size());

    size_t idx = 0;
    for (auto& tc : testCases) {
        // const auto filterIdx = idx;

        const auto filterIdx = idx * 2;
        const auto projectIdx = idx * 2 + 1;

        auto& valsOut = cellBlocks[filterIdx]->getValueBlock();
        auto numObj = blockToBsonArr(valsOut);

        ASSERT_TRUE(SimpleBSONObjComparator::kInstance.evaluate(numObj == tc.filterValues))
            << "Incorrect values for filter path " << pathReqs[filterIdx].toString() << " got "
            << numObj << " expected " << tc.filterValues;

        ASSERT_EQ(cellBlocks[filterIdx]->filterPositionInfo(), tc.filterPosInfo)
            << "Incorrect position info for filter path " << pathReqs[filterIdx].toString()
            << posInfoToString(cellBlocks[filterIdx]->filterPositionInfo())
            << " == " << posInfoToString(tc.filterPosInfo);


        auto projectValues = blockToBsonArr(cellBlocks[projectIdx]->getValueBlock());
        ASSERT_TRUE(SimpleBSONObjComparator::kInstance.evaluate(projectValues == tc.projectValues))
            << "Incorrect values for project path " << pathReqs[projectIdx].toString() << " got "
            << projectValues << " expected " << tc.projectValues;

        ++idx;
    }
}


using Get = value::CellBlock::Get;
using Traverse = value::CellBlock::Traverse;
using Id = value::CellBlock::Id;

TEST_F(BsonBlockDecodingTest, BSONDocumentBlockSimple) {
    std::vector<BSONObj> bsons{
        fromjson("{a:1, b:1}"),
        fromjson("{a:2, b:2}"),
        fromjson("{a:[3,4], b:3}"),
        fromjson("{x: 123}"),
        fromjson("{a:6}"),
    };

    std::vector<PathTestCase> tests{
        PathTestCase{.path = {Get{"a"}, Id{}},
                     .filterValues = fromjson("{result: [1,2,[3,4], null, 6]}"),
                     .filterPosInfo = {1, 1, 1, 1, 1},
                     .projectValues = fromjson("{result: [1,2,[3,4], null, 6]}")},

        PathTestCase{.path = {Get{"b"}, Id{}},
                     .filterValues = fromjson("{result: [1,2,3,null,null]}"),
                     .filterPosInfo = {1, 1, 1, 1, 1},
                     .projectValues = fromjson("{result: [1,2,3,null,null]}")}};
    testPaths(tests, bsons);
}

TEST_F(BsonBlockDecodingTest, BSONDocumentBlockMissings) {
    // A bunch of documents missing 'a' at the beginning and end.
    std::vector<BSONObj> bsons{
        fromjson("{OtherField: 1}"),
        fromjson("{OtherField: 1}"),
        fromjson("{a:1}"),
        fromjson("{a:2}"),
        fromjson("{a:[[3],4]}"),
        fromjson("{OtherField: 1}"),
        fromjson("{a:6}"),
        fromjson("{OtherField: 1}"),
    };

    std::vector<PathTestCase> tests{PathTestCase{
        .path = {Get{"a"}, Id{}},
        .filterValues = fromjson("{result: [null, null, 1,2,[[3],4], null, 6, null]}"),
        .filterPosInfo = {1, 1, 1, 1, 1, 1, 1, 1},
        .projectValues = fromjson("{result: [null, null, 1,2,[[3],4], null, 6, null]}")}};
    testPaths(tests, bsons);
}


TEST_F(BsonBlockDecodingTest, BSONDocumentBlockGetTraverse) {
    std::vector<BSONObj> bsons{
        fromjson("{a:1, b:1}"),
        fromjson("{a:2, b:2}"),
        fromjson("{a:[3,4,[999]], b:2}"),
        fromjson("{a:5, b:2}"),
    };

    std::vector<PathTestCase> tests{
        PathTestCase{.path = {Get{"a"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: [1,2,3,4,[999], 5]}"),
                     .filterPosInfo = {1, 1, 3, 1},
                     .projectValues = fromjson("{result: [1,2,[3,4,[999]], 5]}")}};
    testPaths(tests, bsons);
}

TEST_F(BsonBlockDecodingTest, BSONDocumentBlockSubfield) {
    std::vector<BSONObj> bsons{
        fromjson("{a: {b: 1}}"),
        fromjson("{a: {b: [999, 999]}}"),
        fromjson("{a: [{b: [2, 3]}, {b: [4, 5]}]}"),
        fromjson("{OtherField: 1}"),
        fromjson("{a: [{b: [[999]]}]}"),
    };

    const auto getFieldAResult = fromjson(
        "{result: [{b:1}, {b: [999,999]}, [{b: [2,3]}, {b: [4,5]}], "
        "null, [{b: [[999]]}]]}");

    std::vector<PathTestCase> tests{
        // Get(A)/Id case.
        PathTestCase{.path = {Get{"a"}, Id{}},
                     .filterValues = getFieldAResult,
                     .filterPosInfo = {1, 1, 1, 1, 1},
                     .projectValues = getFieldAResult},
        // Get(A)/Traverse/Id case.
        PathTestCase{.path = {Get{"a"}, Traverse{}, Id{}},
                     .filterValues =
                         fromjson("{result: [{b: 1}, {b: [999, 999]}, {b: [2,3]}, {b: [4,5]}, "
                                  "null, {b: [[999]]}]}"),
                     .filterPosInfo = {1, 1, 2, 1, 1},
                     .projectValues = getFieldAResult},
        // Get(A)/Traverse/Get(b)/Id case.
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                     .filterValues =
                         fromjson("{result: [1, [999,999], [2,3], [4,5], null, [[999]]]}"),
                     .filterPosInfo = {1, 1, 2, 1, 1},
                     .projectValues =
                         fromjson("{result: [1, [999,999], [[2, 3], [4,5]], null, [[[999]]]]}")},
        // Get(a)/Get(b)/Id case. This case does not correspond to any MQL equivalent, but we
        // still want it to work.
        PathTestCase{.path = {Get{"a"}, Get{"b"}, Id{}},
                     .filterValues = fromjson("{result: [1, [999,999], null, null, null]}"),
                     .filterPosInfo = {1, 1, 1, 1, 1},
                     .projectValues = fromjson("{result: [1, [999,999], null, null, null]}")},
        // Get(A)/Traverse/Get(b)/Traverse/Id case.
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: [1,999,999,2,3,4,5, null, [999]]}"),
                     .filterPosInfo = {1, 2, 4, 1, 1},
                     .projectValues =
                         fromjson("{result: [1, [999,999], [[2, 3], [4,5]], null, [[[999]]]]}")}};
    testPaths(tests, bsons);
}

TEST_F(BsonBlockDecodingTest, DoublyNestedArrays) {
    std::vector<BSONObj> bsons{
        fromjson("{a: [[{b: 1}], {b:2}]}"),
        fromjson("{a: [{b: [[3,4]]}, {b: [5, 6]}, {b:7}]}"),
    };

    const auto getFieldAResult =
        fromjson("{result: [[[{b: 1}], {b:2}], [{b: [[3,4]]}, {b: [5, 6]}, {b:7}]]}");

    std::vector<PathTestCase> tests{
        // Get(A)/Id case.
        PathTestCase{.path = {Get{"a"}, Id{}},
                     .filterValues = getFieldAResult,
                     .filterPosInfo = {1, 1},
                     .projectValues = getFieldAResult},
        // Get(A)/Traverse/Id case.
        PathTestCase{.path = {Get{"a"}, Traverse{}, Id{}},
                     .filterValues =
                         fromjson("{result: [[{b: 1}], {b:2}, {b: [[3,4]]}, {b: [5, 6]}, {b:7}]}"),
                     .filterPosInfo = {2, 3},
                     .projectValues = getFieldAResult},
        // Get(A)/Traverse/Get(b)/Id case.
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                     // We expect that objects within doubly nested arrays (e.g. {b:1}) are NOT
                     // traversed. Arrays directly nested within arrays (e.g. [3,4]) are treated
                     // as "blobs" and are not traversed.
                     .filterValues = fromjson("{result: [2, [[3,4]], [5, 6], 7]}"),
                     .filterPosInfo = {1, 3},
                     .projectValues = fromjson("{result: [[2], [[[3,4]], [5, 6], 7]]}")},
        // Get(a)/Get(b)/Id case. This case does not correspond to any MQL equivalent, but we
        // still want it to work.
        PathTestCase{.path = {Get{"a"}, Get{"b"}, Id{}},
                     .filterValues = fromjson("{result: [null, null]}"),
                     .filterPosInfo = {1, 1},
                     .projectValues = fromjson("{result: [null, null]}")},
        // Get(A)/Traverse/Get(b)/Traverse/Id case.
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: [2, [3,4], 5, 6, 7]}"),
                     .filterPosInfo = {1, 4},
                     .projectValues = fromjson("{result: [[2], [[[3,4]], [5, 6], 7]]}")}};
    testPaths(tests, bsons);
}

TEST_F(BsonBlockDecodingTest, BSONDocumentEmptyArrays) {
    {
        std::vector<BSONObj> bsons{
            fromjson("{a: {b: 1}}"),
            fromjson("{a: {b: []}}"),
            fromjson("{a: {b: [2, 3]}}"),
        };
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                         .filterValues = fromjson("{result: [1, [], [2,3]]}"),
                         .filterPosInfo = {1, 1, 1},
                         .projectValues = fromjson("{result: [1, [], [2,3]]}")},
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
                         .filterValues = fromjson("{result: [1, 2,3]}"),
                         .filterPosInfo = {1, 0, 2},
                         .projectValues = fromjson("{result: [1, [], [2,3]]}")},
        };
        testPaths(tests, bsons);
    }

    {
        std::vector<BSONObj> bsons{
            fromjson("{a: [{b: []}, {b: [1,2]}, {b: []}]}"),
            fromjson("{a: [{b: []}, {b: []}, {b: []}]}"),
            fromjson("{a: {b: [3, 4]}}"),
        };
        std::vector<PathTestCase> tests{
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Id{}},
                         .filterValues = fromjson("{result: [[], [1, 2], [], [], [], [], [3, 4]]}"),
                         .filterPosInfo = {3, 3, 1},
                         .projectValues =
                             fromjson("{result: [[[], [1, 2], []], [[], [], []], [3, 4]]}")},
            PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
                         .filterValues = fromjson("{result: [1,2,3,4]}"),
                         .filterPosInfo = {2, 0, 2},
                         .projectValues =
                             fromjson("{result: [[[], [1, 2], []], [[], [], []], [3, 4]]}")},

        };
        testPaths(tests, bsons);
    }
}

TEST_F(BsonBlockDecodingTest, BSONDocumentBlockFieldDoesNotExist) {
    std::vector<BSONObj> bsons{
        fromjson("{a: {b: 1}}"),

        // These documents have no values at a.b, however MQL semantics demand
        // they must know about the array in 'a' for "$a.b" but not for $match.
        //
        // This tests that the information kept in Get A Traverse Get B Traverse projection
        // position info is enough to recover the fact that 'a' had an array.
        fromjson("{a: [{OtherField: 123}]}"),
        fromjson("{a: [1, 2, 3]}"),

        // These have a.b values with the missing value in the middle.
        fromjson("{a: [{b: [4, 5]}, {b: []}, {b: [6, 7]}]}")};


    std::vector<PathTestCase> tests{
        PathTestCase{.path = {Get{"a"}, Traverse{}, Get{"b"}, Traverse{}, Id{}},
                     .filterValues = fromjson("{result: [1, null, null, 4, 5, 6, 7]}"),
                     .filterPosInfo = {1, 1, 1, 4},
                     .projectValues = fromjson("{result: [1, [], [], [[4, 5], [], [6, 7]]]}")}};
    testPaths(tests, bsons);
}

class ValueBlockTest : public mongo::unittest::Test {
public:
    ValueBlockTest() = default;
};

static const auto testOp1 =
    value::makeColumnOp<ColumnOpType::kNoFlags>([](TypeTags tag, Value val) {
        return std::pair(TypeTags::Boolean, value::bitcastFrom<bool>(value::isString(tag)));
    });

static const auto testOp2 =
    value::makeColumnOp<ColumnOpType::kNoFlags>([](TypeTags tag, Value val) {
        if (tag == TypeTags::NumberDouble) {
            double d = value::bitcastTo<double>(val);
            return std::pair(TypeTags::Boolean, value::bitcastFrom<bool>(d >= 5.0));
        } else {
            return std::pair(TypeTags::Nothing, Value{0u});
        }
    });

static const auto testOp3 = value::makeColumnOp<ColumnOpType::kNoFlags>(
    [](TypeTags tag, Value val) {
        return std::pair(TypeTags::Boolean, value::bitcastFrom<bool>(tag != TypeTags::Nothing));
    },
    [](TypeTags tag, const Value* vals, TypeTags* outTags, Value* outVals, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            outTags[i] = TypeTags::Boolean;
            outVals[i] = value::bitcastFrom<bool>(tag != TypeTags::Nothing);
        }
    });

static const auto testOp4 = value::makeColumnOp<ColumnOpType::kNoFlags>(
    [](TypeTags tag, Value val) {
        if (tag == TypeTags::NumberDouble) {
            double d = value::bitcastTo<double>(val);
            return std::pair(TypeTags::NumberDouble, value::bitcastFrom<double>(d * 2.0 + 1.0));
        } else {
            return std::pair(TypeTags::Nothing, Value{0u});
        }
    },
    [](TypeTags tag, const Value* vals, TypeTags* outTags, Value* outVals, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            if (tag == TypeTags::NumberDouble) {
                double d = value::bitcastTo<double>(vals[i]);
                outTags[i] = TypeTags::NumberDouble;
                outVals[i] = value::bitcastFrom<double>(d * 2.0 + 1.0);
            } else {
                outTags[i] = TypeTags::Nothing;
                outVals[i] = Value{0u};
            }
        }
    });

// Test HeterogenousBlock::map().
TEST_F(ValueBlockTest, HeterogeneousBlockMap) {
    auto block = std::make_unique<value::HeterogeneousBlock>();

    block->push_back(TypeTags::Nothing, Value{0u});
    block->push_back(TypeTags::Boolean, value::bitcastFrom<bool>(false));
    block->push_back(TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));
    block->push_back(TypeTags::NumberDouble, value::bitcastFrom<double>(10.0));

    auto [strTag, strVal] = value::makeNewString("not a small string");
    block->push_back(strTag, strVal);

    auto outBlock1 = block->map(testOp1);
    auto output1 = blockToBsonArr(*outBlock1);
    ASSERT_BSONOBJ_EQ(output1, fromjson("{result: [false, false, false, false, true]}"));

    auto outBlock2 = block->map(testOp2);
    auto output2 = blockToBsonArr(*outBlock2);
    ASSERT_BSONOBJ_EQ(output2, fromjson("{result: [null, null, false, true, null]}"));

    auto outBlock3 = block->map(testOp3);
    auto output3 = blockToBsonArr(*outBlock3);
    ASSERT_BSONOBJ_EQ(output3, fromjson("{result: [false, true, true, true, true]}"));

    auto outBlock4 = block->map(testOp4);
    auto output4 = blockToBsonArr(*outBlock4);
    ASSERT_BSONOBJ_EQ(output4, fromjson("{result: [null, null, 7.0, 21.0, null]}"));
}

// Test HomogeneousBlock::map().
TEST_F(ValueBlockTest, HomogeneousBlockMap) {
    auto block = std::make_unique<value::DoubleBlock>();

    block->pushNothing();
    block->push_back(3.0);
    block->push_back(10.0);
    block->pushNothing();

    auto outBlock1 = block->map(testOp1);
    auto output1 = blockToBsonArr(*outBlock1);
    ASSERT_BSONOBJ_EQ(output1, fromjson("{result: [false, false, false, false]}"));

    auto outBlock2 = block->map(testOp2);
    auto output2 = blockToBsonArr(*outBlock2);
    ASSERT_BSONOBJ_EQ(output2, fromjson("{result: [null, false, true, null]}"));

    auto outBlock3 = block->map(testOp3);
    auto output3 = blockToBsonArr(*outBlock3);
    ASSERT_BSONOBJ_EQ(output3, fromjson("{result: [false, true, true, false]}"));

    auto outBlock4 = block->map(testOp4);
    auto output4 = blockToBsonArr(*outBlock4);
    ASSERT_BSONOBJ_EQ(output4, fromjson("{result: [null, 7.0, 21.0, null]}"));
}

// Test MonoBlock::map().
TEST_F(ValueBlockTest, MonoBlockMap) {
    {
        auto block = std::make_unique<value::MonoBlock>(3, TypeTags::Nothing, Value{0u});

        auto outBlock1 = block->map(testOp1);
        auto output1 = blockToBsonArr(*outBlock1);
        ASSERT_BSONOBJ_EQ(output1, fromjson("{result: [false, false, false]}"));

        auto outBlock2 = block->map(testOp2);
        auto output2 = blockToBsonArr(*outBlock2);
        ASSERT_BSONOBJ_EQ(output2, fromjson("{result: [null, null, null]}"));

        auto outBlock3 = block->map(testOp3);
        auto output3 = blockToBsonArr(*outBlock3);
        ASSERT_BSONOBJ_EQ(output3, fromjson("{result: [false, false, false]}"));

        auto outBlock4 = block->map(testOp4);
        auto output4 = blockToBsonArr(*outBlock4);
        ASSERT_BSONOBJ_EQ(output4, fromjson("{result: [null, null, null]}"));
    }

    {
        auto block = std::make_unique<value::MonoBlock>(
            3, TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));

        auto outBlock1 = block->map(testOp1);
        auto output1 = blockToBsonArr(*outBlock1);
        ASSERT_BSONOBJ_EQ(output1, fromjson("{result: [false, false, false]}"));

        auto outBlock2 = block->map(testOp2);
        auto output2 = blockToBsonArr(*outBlock2);
        ASSERT_BSONOBJ_EQ(output2, fromjson("{result: [false, false, false]}"));

        auto outBlock3 = block->map(testOp3);
        auto output3 = blockToBsonArr(*outBlock3);
        ASSERT_BSONOBJ_EQ(output3, fromjson("{result: [true, true, true]}"));

        auto outBlock4 = block->map(testOp4);
        auto output4 = blockToBsonArr(*outBlock4);
        ASSERT_BSONOBJ_EQ(output4, fromjson("{result: [7.0, 7.0, 7.0]}"));
    }

    {
        auto block = std::make_unique<value::MonoBlock>(
            2, TypeTags::NumberDouble, value::bitcastFrom<double>(10.0));

        auto outBlock1 = block->map(testOp1);
        auto output1 = blockToBsonArr(*outBlock1);
        ASSERT_BSONOBJ_EQ(output1, fromjson("{result: [false, false]}"));

        auto outBlock2 = block->map(testOp2);
        auto output2 = blockToBsonArr(*outBlock2);
        ASSERT_BSONOBJ_EQ(output2, fromjson("{result: [true, true]}"));

        auto outBlock3 = block->map(testOp3);
        auto output3 = blockToBsonArr(*outBlock3);
        ASSERT_BSONOBJ_EQ(output3, fromjson("{result: [true, true]}"));

        auto outBlock4 = block->map(testOp4);
        auto output4 = blockToBsonArr(*outBlock4);
        ASSERT_BSONOBJ_EQ(output4, fromjson("{result: [21.0, 21.0]}"));
    }

    {
        auto [strTag, strVal] = value::makeNewString("not a small string");
        value::ValueGuard strGuard(strTag, strVal);
        auto block = std::make_unique<value::MonoBlock>(4, strTag, strVal);

        auto outBlock1 = block->map(testOp1);
        auto output1 = blockToBsonArr(*outBlock1);
        ASSERT_BSONOBJ_EQ(output1, fromjson("{result: [true, true, true, true]}"));

        auto outBlock2 = block->map(testOp2);
        auto output2 = blockToBsonArr(*outBlock2);
        ASSERT_BSONOBJ_EQ(output2, fromjson("{result: [null, null, null, null]}"));

        auto outBlock3 = block->map(testOp3);
        auto output3 = blockToBsonArr(*outBlock3);
        ASSERT_BSONOBJ_EQ(output3, fromjson("{result: [true, true, true, true]}"));

        auto outBlock4 = block->map(testOp4);
        auto output4 = blockToBsonArr(*outBlock4);
        ASSERT_BSONOBJ_EQ(output4, fromjson("{result: [null, null, null, null]}"));
    }
}

class TestBlock : public value::ValueBlock {
public:
    TestBlock() = default;
    TestBlock(const TestBlock& o) : value::ValueBlock(o) {
        _vals.resize(o._vals.size(), Value{0u});
        _tags.resize(o._tags.size(), TypeTags::Nothing);
        for (size_t i = 0; i < o._vals.size(); ++i) {
            auto [copyTag, copyVal] = copyValue(o._tags[i], o._vals[i]);
            _vals[i] = copyVal;
            _tags[i] = copyTag;
        }
    }
    TestBlock(TestBlock&& o)
        : value::ValueBlock(std::move(o)), _vals(std::move(o._vals)), _tags(std::move(o._tags)) {
        o._vals = {};
        o._tags = {};
    }
    ~TestBlock() {
        for (size_t i = 0; i < _vals.size(); ++i) {
            releaseValue(_tags[i], _vals[i]);
        }
    }

    void push_back(TypeTags t, Value v) {
        _vals.push_back(v);
        _tags.push_back(t);
    }
    boost::optional<size_t> tryCount() const override {
        return _vals.size();
    }
    value::DeblockedTagVals deblock(
        boost::optional<value::DeblockedTagValStorage>& storage) override {
        return {_vals.size(), _tags.data(), _vals.data()};
    }
    std::unique_ptr<value::ValueBlock> clone() const override {
        return std::make_unique<TestBlock>(*this);
    }

    std::pair<value::TypeTags, value::Value> tryMin() const override {
        if (_minVal) {
            return *_minVal;
        }
        return value::ValueBlock::tryMin();
    }
    std::pair<value::TypeTags, value::Value> tryMax() const override {
        if (_maxVal) {
            return *_maxVal;
        }
        return value::ValueBlock::tryMax();
    }

    void setMin(value::TypeTags tag, value::Value val) {
        _minVal.emplace(tag, val);
    }
    void setMax(value::TypeTags tag, value::Value val) {
        _maxVal.emplace(tag, val);
    }

private:
    std::vector<Value> _vals;
    std::vector<TypeTags> _tags;
    boost::optional<std::pair<value::TypeTags, value::Value>> _minVal, _maxVal;
};

// Test ValueBlock::defaultMapImpl().
TEST_F(ValueBlockTest, TestBlockMap) {
    auto block = std::make_unique<TestBlock>();

    block->push_back(TypeTags::Nothing, Value{0u});
    block->push_back(TypeTags::Boolean, value::bitcastFrom<bool>(false));
    block->push_back(TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));
    block->push_back(TypeTags::NumberDouble, value::bitcastFrom<double>(10.0));

    auto [strTag, strVal] = value::makeNewString("not a small string");
    block->push_back(strTag, strVal);

    auto outBlock1 = block->map(testOp1);
    auto output1 = blockToBsonArr(*outBlock1);
    ASSERT_BSONOBJ_EQ(output1, fromjson("{result: [false, false, false, false, true]}"));

    auto outBlock2 = block->map(testOp2);
    auto output2 = blockToBsonArr(*outBlock2);
    ASSERT_BSONOBJ_EQ(output2, fromjson("{result: [null, null, false, true, null]}"));

    auto outBlock3 = block->map(testOp3);
    auto output3 = blockToBsonArr(*outBlock3);
    ASSERT_BSONOBJ_EQ(output3, fromjson("{result: [false, true, true, true, true]}"));

    auto outBlock4 = block->map(testOp4);
    auto output4 = blockToBsonArr(*outBlock4);
    ASSERT_BSONOBJ_EQ(output4, fromjson("{result: [null, null, 7.0, 21.0, null]}"));
}

// Test monotonic shortcut in ValueBlock::defaultMapImpl().
static const auto testOp5 = value::makeColumnOp<ColumnOpType::kMonotonic>(
    [](TypeTags tag, Value val) { return value::makeBigString("fake result from map"); });

TEST_F(ValueBlockTest, TestBlockMapFast) {
    auto block = std::make_unique<TestBlock>();

    auto [strTag1, strVal1] = value::makeNewString("not a small string");
    block->push_back(strTag1, strVal1);
    auto [strTag2, strVal2] = value::makeNewString("a slightly longer string");
    block->push_back(strTag2, strVal2);

    block->setMin(strTag2, strVal2);
    block->setMax(strTag1, strVal1);

    auto outBlock1 = block->map(testOp5);
    auto output1 = blockToBsonArr(*outBlock1);
    ASSERT_BSONOBJ_EQ(output1,
                      fromjson("{result: ['fake result from map', 'fake result from map']}"));
}

// Test ValueBlock::tokenize().
TEST_F(ValueBlockTest, TestTokenize) {
    auto block = std::make_unique<TestBlock>();

    auto [tag1, val1] = value::makeNewString("foofoofoo"_sd);
    block->push_back(tag1, val1);
    auto [tag2, val2] = value::makeNewString("bar"_sd);  // StringSmall
    block->push_back(tag2, val2);
    auto [tag3, val3] = value::makeNewString("bazbazbaz"_sd);
    block->push_back(tag3, val3);
    auto [tag4, val4] = value::makeNewString("bar"_sd);  // StringSmall
    block->push_back(tag4, val4);
    auto [tag5, val5] = value::makeNewString("bar"_sd);  // StringSmall
    block->push_back(tag5, val5);
    block->push_back(TypeTags::NumberInt32, value::bitcastFrom<int32_t>(999));
    block->push_back(TypeTags::Nothing, Value{0u});
    auto [tag6, val6] = value::makeNewString("foofoofoo"_sd);
    block->push_back(tag6, val6);
    block->push_back(TypeTags::Nothing, Value{0u});

    auto [outTokens, outIdxs] = block->tokenize();
    ASSERT_EQ(outTokens->tryCount(), boost::optional<size_t>(5));

    auto outTokensBson = blockToBsonArr(*outTokens);
    ASSERT_BSONOBJ_EQ(outTokensBson,
                      fromjson("{result: [\"foofoofoo\", \"bar\", \"bazbazbaz\", 999, null]}"));

    std::vector<size_t> expIdxs{0, 1, 2, 1, 1, 3, 4, 0, 4};
    ASSERT_EQ(outIdxs, expIdxs);
}

// Test MonoBlock::tokenize().
TEST_F(ValueBlockTest, MonoBlockTokenize) {
    {
        auto [strTag, strVal] = value::makeNewString("not a small string"_sd);
        value::ValueGuard strGuard(strTag, strVal);
        auto block = std::make_unique<value::MonoBlock>(4, strTag, strVal);

        auto [outTokens, outIdxs] = block->tokenize();
        ASSERT_EQ(outTokens->tryCount(), boost::optional<size_t>(1));

        auto outTokensBson = blockToBsonArr(*outTokens);
        ASSERT_BSONOBJ_EQ(outTokensBson, fromjson("{result: [\"not a small string\"]}"));

        std::vector<size_t> expIdxs{0, 0, 0, 0};
        ASSERT_EQ(outIdxs, expIdxs);
    }

    {
        auto block = std::make_unique<value::MonoBlock>(4, TypeTags::Nothing, Value{0u});

        auto [outTokens, outIdxs] = block->tokenize();
        ASSERT_EQ(outTokens->tryCount(), boost::optional<size_t>(1));

        auto outTokensBson = blockToBsonArr(*outTokens);
        ASSERT_BSONOBJ_EQ(outTokensBson, fromjson("{result: [null]}"));

        std::vector<size_t> expIdxs{0, 0, 0, 0};
        ASSERT_EQ(outIdxs, expIdxs);
    }
}

// Int32Block::tokenize(), Int64Block::tokenize(), and DateBlock::tokenize() are effectively
// identical so they are combined into 1 test.
TEST_F(ValueBlockTest, IntBlockTokenize) {
    {
        // Test that first token is Nothing for non-dense blocks.
        auto block = std::make_unique<value::Int32Block>();

        block->push_back(value::bitcastFrom<int32_t>(2));
        block->pushNothing();
        block->push_back(value::bitcastFrom<int32_t>(0));
        block->push_back(value::bitcastFrom<int32_t>(2));
        block->pushNothing();
        block->pushNothing();
        block->push_back(value::bitcastFrom<int32_t>(1));
        block->pushNothing();
        block->pushNothing();

        auto [outTokens, outIdxs] = block->tokenize();
        ASSERT_EQ(outTokens->tryCount(), boost::optional<size_t>(4));

        auto outTokensBson = blockToBsonArr(*outTokens);
        ASSERT_BSONOBJ_EQ(outTokensBson, fromjson("{result: [null, 2, 0, 1]}"));

        std::vector<size_t> expIdxs{1, 0, 2, 1, 0, 0, 3, 0, 0};
        ASSERT_EQ(outIdxs, expIdxs);
    }

    {
        // Test on leading Nothing.
        auto block = std::make_unique<value::Int32Block>();

        block->pushNothing();
        block->push_back(value::bitcastFrom<int32_t>(0));

        auto [outTokens, outIdxs] = block->tokenize();
        ASSERT_EQ(outTokens->tryCount(), boost::optional<size_t>(2));

        auto outTokensBson = blockToBsonArr(*outTokens);
        ASSERT_BSONOBJ_EQ(outTokensBson, fromjson("{result: [null, 0]}"));

        std::vector<size_t> expIdxs{0, 1};
        ASSERT_EQ(outIdxs, expIdxs);
    }

    {
        // Test on dense input.
        auto block = std::make_unique<value::Int32Block>();

        block->push_back(value::bitcastFrom<int32_t>(2));
        block->push_back(value::bitcastFrom<int32_t>(0));
        block->push_back(value::bitcastFrom<int32_t>(2));
        block->push_back(value::bitcastFrom<int32_t>(1));

        auto [outTokens, outIdxs] = block->tokenize();
        ASSERT_EQ(outTokens->tryCount(), boost::optional<size_t>(3));

        auto outTokensBson = blockToBsonArr(*outTokens);
        ASSERT_BSONOBJ_EQ(outTokensBson, fromjson("{result: [2, 0, 1]}"));

        std::vector<size_t> expIdxs{0, 1, 0, 2};
        ASSERT_EQ(outIdxs, expIdxs);
    }
}

// Test that default implementation still works for DoubleBlock's.
TEST_F(ValueBlockTest, DoubleBlockTokenize) {
    auto block = std::make_unique<value::DoubleBlock>();

    block->pushNothing();
    block->push_back(1.1);
    block->push_back(std::numeric_limits<double>::quiet_NaN());
    block->pushNothing();
    block->pushNothing();
    block->push_back(std::numeric_limits<double>::signaling_NaN());
    block->push_back(2.2);
    block->push_back(1.1);
    block->push_back(std::numeric_limits<double>::quiet_NaN());

    auto [outTokens, outIdxs] = block->tokenize();
    ASSERT_EQ(outTokens->tryCount(), boost::optional<size_t>(4));

    auto outTokensBson = blockToBsonArr(*outTokens);
    ASSERT_BSONOBJ_EQ(outTokensBson, fromjson("{result: [null, 1.1, NaN, 2.2]}"));

    std::vector<size_t> expIdxs{0, 1, 2, 0, 0, 2, 3, 1, 2};
    ASSERT_EQ(outIdxs, expIdxs);
}

}  // namespace mongo::sbe
