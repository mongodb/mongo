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
    ASSERT_EQ(extracted.count, 1);
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
    ASSERT_EQ(extracted.count, 1);
}

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

    std::vector<std::unique_ptr<value::CellBlock>> extractCellBlocks(
        const std::vector<value::CellBlock::PathRequest>& paths,
        const std::vector<BSONObj>& bsons) {

        if (useTsImpl) {
            // Shred the bsons here, produce a time series "bucket"-like thing, and pass it to the
            // TS decoding implementation.
            StringMap<std::unique_ptr<BSONColumnBuilder>> shredMap;

            size_t bsonIdx = 0;
            for (auto&& bson : bsons) {
                // Keep track of which fields we visited for this bson, so we can pad the
                // unvisited fields with 'gaps'.
                StringDataSet fieldsVisited;
                for (auto elt : bson) {
                    auto [it, inserted] = shredMap.insert(
                        std::pair(elt.fieldName(), std::make_unique<BSONColumnBuilder>()));

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
            return extractor.extractCellBlocks(_bucketStorage);
        } else {
            return value::extractCellBlocksFromBsons(paths, bsons);
        }
    }

private:
    BSONObj _bucketStorage;

    bool useTsImpl = false;
};

BSONObj blockToBsonArr(value::ValueBlock& block) {
    auto extracted = block.extract();
    BSONArrayBuilder arr;
    for (size_t i = 0; i < extracted.count; ++i) {
        auto tag = extracted.tags[i];
        auto val = extracted.vals[i];

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

std::string posInfoToString(const std::vector<char>& posInfo) {
    std::string out;
    for (auto c : posInfo) {
        out.push_back(c ? '1' : '0');
    }
    return out;
}

TEST_F(BsonBlockDecodingTest, BSONDocumentBlockSimple) {
    std::vector<BSONObj> bsons{
        fromjson("{a:1, b:1}"),
        fromjson("{a:2, b:2}"),
        fromjson("{a:[3,4], b:3}"),
        fromjson("{x: 123}"),
        fromjson("{a:6}"),
    };

    value::CellBlock::PathRequest aReq{{value::CellBlock::Get{"a"}, value::CellBlock::Id{}}};
    value::CellBlock::PathRequest bReq{{value::CellBlock::Get{"b"}, value::CellBlock::Id{}}};

    auto cellBlocks = extractCellBlocks({aReq, bReq}, bsons);

    ASSERT_EQ(cellBlocks.size(), 2);

    {
        auto& valsOut = cellBlocks[0]->getValueBlock();
        auto numObj = blockToBsonArr(valsOut);
        ASSERT_BSONOBJ_EQ(numObj, fromjson("{result: [1,2,[3,4], null, 6]}"));

        ASSERT_EQ(posInfoToString(cellBlocks[0]->filterPositionInfo()), "11111");
    }

    {
        auto& valsOut = cellBlocks[1]->getValueBlock();
        auto numObj = blockToBsonArr(valsOut);
        ASSERT_BSONOBJ_EQ(numObj, fromjson("{result: [1,2,3,null,null]}"));
        ASSERT_EQ(posInfoToString(cellBlocks[0]->filterPositionInfo()), "11111");
    }
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

    value::CellBlock::PathRequest aReq{{value::CellBlock::Get{"a"}, value::CellBlock::Id{}}};

    auto cellBlocks = extractCellBlocks({aReq}, bsons);

    ASSERT_EQ(cellBlocks.size(), 1);

    {
        auto& valsOut = cellBlocks[0]->getValueBlock();
        auto numObj = blockToBsonArr(valsOut);
        ASSERT_BSONOBJ_EQ(numObj, fromjson("{result: [null, null, 1,2,[[3],4], null, 6, null]}"));

        ASSERT_EQ(posInfoToString(cellBlocks[0]->filterPositionInfo()), "11111111");
    }
}

TEST_F(BsonBlockDecodingTest, BSONDocumentBlockGetTraverse) {
    std::vector<BSONObj> bsons{
        fromjson("{a:1, b:1}"),
        fromjson("{a:2, b:2}"),
        fromjson("{a:[3,4,[999]], b:2}"),
        fromjson("{a:5, b:2}"),
    };

    value::CellBlock::PathRequest req{
        {value::CellBlock::Get{"a"}, value::CellBlock::Traverse{}, value::CellBlock::Id{}}};

    auto cellBlocks = extractCellBlocks({req}, bsons);

    ASSERT_EQ(cellBlocks.size(), 1);

    {
        auto& valsOut = cellBlocks[0]->getValueBlock();
        auto numObj = blockToBsonArr(valsOut);
        ASSERT_BSONOBJ_EQ(numObj, fromjson("{result: [1,2,3,4,[999], 5]}"));
        ASSERT_EQ(posInfoToString(cellBlocks[0]->filterPositionInfo()), "111001");
    }
}

TEST_F(BsonBlockDecodingTest, BSONDocumentBlockSubfield) {
    std::vector<BSONObj> bsons{
        fromjson("{a: {b: 1}}"),
        fromjson("{a: {b: [999, 999]}}"),
        fromjson("{a: [{b: [2, 3]}, {b: [4, 5]}]}"),
        fromjson("{OtherField: 1}"),
        fromjson("{a: [{b: [[999]]}]}"),
    };

    {
        // Get A Id.
        value::CellBlock::PathRequest req{{value::CellBlock::Get{"a"}, value::CellBlock::Id{}}};

        auto cellBlocks = extractCellBlocks({req}, bsons);
        ASSERT_EQ(cellBlocks.size(), 1);

        auto& valsOut = cellBlocks[0]->getValueBlock();
        auto numObj = blockToBsonArr(valsOut);
        ASSERT_BSONOBJ_EQ(numObj,
                          fromjson("{result: [{b:1}, {b: [999,999]}, [{b: [2,3]}, {b: [4,5]}], "
                                   "null, [{b: [[999]]}]]}"));
        ASSERT_EQ(posInfoToString(cellBlocks[0]->filterPositionInfo()), "11111");
    }

    {
        // Get A Traverse Id.
        value::CellBlock::PathRequest req{
            {value::CellBlock::Get{"a"}, value::CellBlock::Traverse{}, value::CellBlock::Id{}}};

        auto cellBlocks = extractCellBlocks({req}, bsons);
        ASSERT_EQ(cellBlocks.size(), 1);

        auto& valsOut = cellBlocks[0]->getValueBlock();
        auto numObj = blockToBsonArr(valsOut);
        ASSERT_BSONOBJ_EQ(
            numObj, fromjson("{result: [null, null, {b: [2,3]}, {b: [4,5]}, null, {b: [[999]]}]}"));
        ASSERT_EQ(posInfoToString(cellBlocks[0]->filterPositionInfo()), "111011");
    }

    {
        // Get A Traverse Get B Id.
        value::CellBlock::PathRequest req{{value::CellBlock::Get{"a"},
                                           value::CellBlock::Traverse{},
                                           value::CellBlock::Get{"b"},
                                           value::CellBlock::Id{}}};

        auto cellBlocks = extractCellBlocks({req}, bsons);
        ASSERT_EQ(cellBlocks.size(), 1);

        auto& valsOut = cellBlocks[0]->getValueBlock();
        auto numObj = blockToBsonArr(valsOut);
        ASSERT_BSONOBJ_EQ(numObj,
                          fromjson("{result: [1, [999,999], [2,3], [4,5], null, [[999]]]}"));
        ASSERT_EQ(posInfoToString(cellBlocks[0]->filterPositionInfo()), "111011");
    }

    {
        // Get A Traverse Get B Traverse Id.
        value::CellBlock::PathRequest req{{value::CellBlock::Get{"a"},
                                           value::CellBlock::Traverse{},
                                           value::CellBlock::Get{"b"},
                                           value::CellBlock::Traverse{},
                                           value::CellBlock::Id{}}};

        auto cellBlocks = extractCellBlocks({req}, bsons);
        ASSERT_EQ(cellBlocks.size(), 1);

        auto& valsOut = cellBlocks[0]->getValueBlock();
        auto numObj = blockToBsonArr(valsOut);
        ASSERT_BSONOBJ_EQ(numObj, fromjson("{result: [1,999,999,2,3,4,5, null, [999]]}"));
        ASSERT_EQ(posInfoToString(cellBlocks[0]->filterPositionInfo()), "110100011");
    }
}

TEST_F(BsonBlockDecodingTest, BSONDocumentBlockFieldDoesNotExist) {
    std::vector<BSONObj> bsons{
        fromjson("{a: {b: 1}}"),

        // These documents have no values at a.b, however MQL semantics demand
        // they must know about the array in 'a' for "$a.b" but not for $match.
        //
        // This tests that the information kept in Get A Traverse Get B Traverse projection position
        // info is enough to recover the fact that 'a' had an array.
        fromjson("{a: [{OtherField: 123}]}"),
        fromjson("{a: [1, 2, 3]}"),

        // These have a.b values with the missing value in the middle.
        fromjson("{a: [{b: [4, 5]}, {b: []}, {b: [6, 7]}]}")};


    {
        // Get A Traverse Get B Traverse Id.
        value::CellBlock::PathRequest req{{value::CellBlock::Get{"a"},
                                           value::CellBlock::Traverse{},
                                           value::CellBlock::Get{"b"},
                                           value::CellBlock::Traverse{},
                                           value::CellBlock::Id{}}};

        auto cellBlocks = extractCellBlocks({req}, bsons);
        ASSERT_EQ(cellBlocks.size(), 1);

        auto& valsOut = cellBlocks[0]->getValueBlock();
        auto numObj = blockToBsonArr(valsOut);
        ASSERT_BSONOBJ_EQ(numObj, fromjson("{result: [1, null, null, 4, 5, 6, 7]}"));
        ASSERT_EQ(posInfoToString(cellBlocks[0]->filterPositionInfo()), "1111000");
    }
}

class ValueBlockTest : public mongo::unittest::Test {
public:
    ValueBlockTest() = default;
};

static constexpr auto testOp1Type = ColumnOpType{ColumnOpType::kOutputNonNothingOnExpectedInput,
                                                 TypeTags::Nothing,
                                                 TypeTags::Boolean,
                                                 ColumnOpType::ReturnBoolOnMissing{}};
static const auto testOp1 = value::makeColumnOp<testOp1Type>([](TypeTags tag, Value val) {
    return std::pair(TypeTags::Boolean, value::bitcastFrom<bool>(value::isString(tag)));
});

static constexpr auto testOp2Type = ColumnOpType{ColumnOpType::kOutputNonNothingOnExpectedInput,
                                                 TypeTags::NumberDouble,
                                                 TypeTags::Boolean,
                                                 ColumnOpType::ReturnNothingOnMissing{}};
static const auto testOp2 = value::makeColumnOp<testOp2Type>([](TypeTags tag, Value val) {
    if (tag == TypeTags::NumberDouble) {
        double d = value::bitcastTo<double>(val);
        return std::pair(TypeTags::Boolean, value::bitcastFrom<bool>(d >= 5.0));
    } else {
        return std::pair(TypeTags::Nothing, Value{0u});
    }
});

static constexpr auto testOp3Type = ColumnOpType{ColumnOpType::kOutputNonNothingOnExpectedInput,
                                                 TypeTags::Nothing,
                                                 TypeTags::Boolean,
                                                 ColumnOpType::ReturnBoolOnMissing{}};
static const auto testOp3 = value::makeColumnOp<testOp3Type>(
    [](TypeTags tag, Value val) {
        return std::pair(TypeTags::Boolean, value::bitcastFrom<bool>(tag != TypeTags::Nothing));
    },
    [](const TypeTags* tags, const Value* vals, TypeTags* outTags, Value* outVals, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            outTags[i] = TypeTags::Boolean;
            outVals[i] = value::bitcastFrom<bool>(tags[i] != TypeTags::Nothing);
        }
    });

static constexpr auto testOp4Type = ColumnOpType{ColumnOpType::kOutputNonNothingOnExpectedInput,
                                                 TypeTags::NumberDouble,
                                                 TypeTags::NumberDouble,
                                                 ColumnOpType::ReturnNothingOnMissing{}};
static const auto testOp4 = value::makeColumnOp<testOp4Type>(
    [](TypeTags tag, Value val) {
        if (tag == TypeTags::NumberDouble) {
            double d = value::bitcastTo<double>(val);
            return std::pair(TypeTags::NumberDouble, value::bitcastFrom<double>(d * 2.0 + 1.0));
        } else {
            return std::pair(TypeTags::Nothing, Value{0u});
        }
    },
    [](const TypeTags* tags, const Value* vals, TypeTags* outTags, Value* outVals, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            if (tags[i] == TypeTags::NumberDouble) {
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
        boost::optional<value::DeblockedTagValStorage>& storage) const override {
        return {_vals.size(), _tags.data(), _vals.data()};
    }
    std::unique_ptr<value::ValueBlock> clone() const override {
        return std::make_unique<TestBlock>(*this);
    }

private:
    std::vector<Value> _vals;
    std::vector<TypeTags> _tags;
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

}  // namespace mongo::sbe
