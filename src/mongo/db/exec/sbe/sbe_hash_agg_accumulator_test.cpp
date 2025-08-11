/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
 * This file contains tests for sbe::HashAggStage.
 */

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/hashagg_base.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sbe {
class HashAggAccumulatorTest : public unittest::Test {
public:
    void setUp() override {
        unittest::Test::setUp();

        _mockSpilledAggregateStorage.clear();

        _table.clear();
        _table.emplace(std::make_pair(
            value::MaterializedRow(0),  // The key is not relevant to testing the accumulator.
            value::MaterializedRow(1))  // The one value in this row stores the accumulator's state.
        );
        _tableEntryIt = _table.begin();

        // The '_accumulatorState' accessor is a view into the accumulator's table entry.
        _accumulatorState = std::make_unique<HashAggAccessor>(
            _tableEntryIt,
            0 /* The first and only column in the row that '_tableEntryIt' references. */);

        auto env = std::make_unique<RuntimeEnvironment>();

        _inSlot = env->registerSlot(value::TypeTags::Nothing, 0, false, &_slotIdGenerator);
        _spillSlot = env->registerSlot(value::TypeTags::Nothing, 0, false, &_slotIdGenerator);
        _outSlot = _slotIdGenerator.generate();

        _compileCtx = std::make_unique<CompileCtx>(std::move(env));
    }

    void tearDown() override {
        unittest::Test::tearDown();
    }

protected:
    HashAggAccessor& accumulatorState() {
        return *_accumulatorState;
    }

    value::SlotId generateSlotId() {
        return _slotIdGenerator.generate();
    }

    value::SlotIdGenerator& slotIdGenerator() {
        return _slotIdGenerator;
    }

    CompileCtx& compileContext() {
        return *_compileCtx;
    }

    value::SlotId inSlot() const {
        return _inSlot;
    }

    value::SlotId spillSlot() const {
        return _spillSlot;
    }

    value::SlotId outSlot() const {
        return _outSlot;
    }

    RuntimeEnvironment::Accessor& inAccessor() const {
        return *_compileCtx->getRuntimeEnvAccessor(_inSlot);
    }

    RuntimeEnvironment::Accessor& spillAccessor() const {
        return *_compileCtx->getRuntimeEnvAccessor(_spillSlot);
    }

    void moveAccumulatorStateToMockSpillStorage() {
        _mockSpilledAggregateStorage.emplace_back();
        auto [tagSpilled, valSpilled] = _accumulatorState->copyOrMoveValue();
        _mockSpilledAggregateStorage.back().reset(true, tagSpilled, valSpilled);
    }

    bool isMockSpillStorageEmpty() const {
        return _mockSpilledAggregateStorage.empty();
    }

    std::pair<value::TypeTags, value::Value> consumePartialAggregateFromMockSpillStorage() {
        invariant(!_mockSpilledAggregateStorage.empty());
        ON_BLOCK_EXIT([&]() { _mockSpilledAggregateStorage.pop_front(); });
        return _mockSpilledAggregateStorage.front().copyOrMoveValue();
    }

private:
    /**
     * The test fixture maintains a hash agg table with one row whose value has one column that
     * stores the accumulator's state.
     */
    TableType _table;
    typename TableType::iterator _tableEntryIt;

    std::unique_ptr<HashAggAccessor> _accumulatorState;

    value::SlotIdGenerator _slotIdGenerator;
    std::unique_ptr<CompileCtx> _compileCtx;

    value::SlotId _inSlot;
    value::SlotId _spillSlot;
    value::SlotId _outSlot;

    std::deque<value::OwnedValueAccessor> _mockSpilledAggregateStorage;
};

TEST_F(HashAggAccumulatorTest, ArithmeticAverageHashAggAccumulatorTerminal) {
    ArithmeticAverageHashAggAccumulatorTerminal accumulator(
        outSlot(), spillSlot(), makeVariable(inSlot()));

    vm::ByteCode bytecode;
    accumulator.prepare(compileContext(), &accumulatorState());

    //
    // Test the genral case: add several values and ensure the result is the average value.
    //
    accumulator.initialize(bytecode, accumulatorState());

    // Input 10 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10));
    accumulator.accumulate(bytecode, accumulatorState());

    // Input 90 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(90));
    accumulator.accumulate(bytecode, accumulatorState());

    // Input a string to the accumulator, which we expect it to ignore.
    auto [tagInput, valInput] = value::makeSmallString(":/");
    inAccessor().reset(true, tagInput, valInput);
    accumulator.accumulate(bytecode, accumulatorState());

    // Input 990 to the accumulator.
    std::tie(tagInput, valInput) = value::makeCopyDecimal(Decimal128(200));
    inAccessor().reset(true, tagInput, valInput);
    accumulator.accumulate(bytecode, accumulatorState());

    accumulator.finalize(bytecode, accumulatorState());

    auto [tagResult, valResult] = accumulatorState().getViewOfValue();
    ASSERT_EQ(tagResult, value::TypeTags::NumberDecimal);
    ASSERT_EQ(value::bitcastTo<Decimal128>(valResult), Decimal128(100));

    //
    // Test that we can reinitialize the same accumulator and use it again.
    //
    accumulator.initialize(bytecode, accumulatorState());

    // Input 10 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10));
    accumulator.accumulate(bytecode, accumulatorState());

    // Input 90 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(90));
    accumulator.accumulate(bytecode, accumulatorState());

    accumulator.finalize(bytecode, accumulatorState());

    std::tie(tagResult, valResult) = accumulatorState().getViewOfValue();
    ASSERT_EQ(tagResult, value::TypeTags::NumberDouble);
    ASSERT_EQ(value::bitcastTo<double>(valResult), double{50});
}

TEST_F(HashAggAccumulatorTest, ArithmeticAverageHashAggAccumulatorTerminalEmpty) {
    ArithmeticAverageHashAggAccumulatorTerminal accumulator(
        outSlot(), spillSlot(), makeVariable(inSlot()));

    vm::ByteCode bytecode;
    accumulator.prepare(compileContext(), &accumulatorState());

    //
    // Accumulating any empty list of values should produce a null value.
    //
    accumulator.initialize(bytecode, accumulatorState());

    accumulator.finalize(bytecode, accumulatorState());

    auto [tagResult, valResult] = accumulatorState().getViewOfValue();
    ASSERT_EQ(tagResult, value::TypeTags::Null);
}

TEST_F(HashAggAccumulatorTest, ArithmeticAverageHashAggAccumulatorTerminalSpilled) {
    ArithmeticAverageHashAggAccumulatorTerminal accumulator(
        outSlot(), spillSlot(), makeVariable(inSlot()));

    vm::ByteCode bytecode;
    accumulator.prepare(compileContext(), &accumulatorState());
    accumulator.prepareForMerge(compileContext(), &accumulatorState());

    accumulator.initialize(bytecode, accumulatorState());

    // Input 4 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4));
    accumulator.accumulate(bytecode, accumulatorState());

    // Input 40 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(40));
    accumulator.accumulate(bytecode, accumulatorState());

    // Spill and reset the accumulator.
    moveAccumulatorStateToMockSpillStorage();
    accumulator.initialize(bytecode, accumulatorState());

    // Input 400 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(400));
    accumulator.accumulate(bytecode, accumulatorState());

    // Input 4,000 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4000));
    accumulator.accumulate(bytecode, accumulatorState());

    // Spill and reset the accumulator.
    moveAccumulatorStateToMockSpillStorage();
    accumulator.initialize(bytecode, accumulatorState());

    // Spill and then reset the empty accumulator.
    moveAccumulatorStateToMockSpillStorage();
    accumulator.initialize(bytecode, accumulatorState());

    // Input 40,000 to the accumulator.
    auto [tagInput, valInput] = value::makeCopyDecimal(Decimal128(40000));
    inAccessor().reset(true, tagInput, valInput);
    accumulator.accumulate(bytecode, accumulatorState());

    // Input 400,000 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(400000));
    accumulator.accumulate(bytecode, accumulatorState());

    // Spill and reset the accumulator.
    moveAccumulatorStateToMockSpillStorage();
    accumulator.initialize(bytecode, accumulatorState());

    // Merge each of the spilled partial aggregates.
    value::MaterializedRow mergedAggregate(1);
    value::MaterializedSingleRowAccessor mergedAggregateAccessor(mergedAggregate, 0);
    auto [tagRecovered, valRecovered] = consumePartialAggregateFromMockSpillStorage();
    mergedAggregate.reset(0, true, tagRecovered, valRecovered);

    while (!isMockSpillStorageEmpty()) {
        std::tie(tagRecovered, valRecovered) = consumePartialAggregateFromMockSpillStorage();
        spillAccessor().reset(true, tagRecovered, valRecovered);
        accumulator.merge(bytecode, mergedAggregateAccessor);
    }

    // Repopulate the accumulator's state slot with the merged aggregate value so that we can
    // finalize it and validate the result.
    auto [tagMerged, valMerged] = mergedAggregate.copyOrMoveValue(0);
    accumulatorState().reset(true, tagMerged, valMerged);
    accumulator.finalize(bytecode, accumulatorState());

    auto [tagResult, valResult] = accumulatorState().getViewOfValue();
    ASSERT_EQ(tagResult, value::TypeTags::NumberDecimal);
    ASSERT_EQ(value::bitcastTo<Decimal128>(valResult), Decimal128(74074));
}

TEST_F(HashAggAccumulatorTest, ArithmeticAverageHashAggAccumulatorPartial) {
    ArithmeticAverageHashAggAccumulatorPartial accumulator(
        outSlot(), spillSlot(), makeVariable(inSlot()));

    vm::ByteCode bytecode;
    accumulator.prepare(compileContext(), &accumulatorState());

    //
    // Test the genral case: add several values and ensure the result is the average value
    // reprsented as a partial sum in the format that the merging node expects to see.
    //
    accumulator.initialize(bytecode, accumulatorState());

    // Input 10 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10));
    accumulator.accumulate(bytecode, accumulatorState());

    // Input 90 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(90));
    accumulator.accumulate(bytecode, accumulatorState());

    // Input a string to the accumulator, which we expect it to ignore.
    auto [tagInput, valInput] = value::makeSmallString(":/");
    inAccessor().reset(true, tagInput, valInput);
    accumulator.accumulate(bytecode, accumulatorState());

    // Input 990 to the accumulator.
    std::tie(tagInput, valInput) = value::makeCopyDecimal(Decimal128(200));
    inAccessor().reset(true, tagInput, valInput);
    accumulator.accumulate(bytecode, accumulatorState());

    accumulator.finalize(bytecode, accumulatorState());

    auto [tagResult, valResult] = accumulatorState().getViewOfValue();
    ASSERT_EQ(tagResult, value::TypeTags::Object);
    // Convert the result to BSON to simplify the comparison.
    auto resultObj = [&]() {
        BSONObjBuilder resultBuilder;
        bson::convertToBsonObj(resultBuilder, value::getObjectView(valResult));
        return resultBuilder.obj();
    }();
    ASSERT_BSONOBJ_EQ_UNORDERED(BSON("count" << 3 << "ps" << BSON_ARRAY(1 << 100 << 0 << 200)),
                                resultObj);

    //
    // Test that we can reinitialize the same accumulator and use it again.
    //
    accumulator.initialize(bytecode, accumulatorState());

    // Input 10 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10));
    accumulator.accumulate(bytecode, accumulatorState());

    // Input 90 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(90));
    accumulator.accumulate(bytecode, accumulatorState());

    accumulator.finalize(bytecode, accumulatorState());

    std::tie(tagResult, valResult) = accumulatorState().getViewOfValue();
    ASSERT_EQ(tagResult, value::TypeTags::Object);
    resultObj = [&]() {
        BSONObjBuilder resultBuilder;
        bson::convertToBsonObj(resultBuilder, value::getObjectView(valResult));
        return resultBuilder.obj();
    }();
    ASSERT_BSONOBJ_EQ_UNORDERED(BSON("count" << 2 << "ps" << BSON_ARRAY(1 << 100 << 0)), resultObj);
}

TEST_F(HashAggAccumulatorTest, ArithmeticAverageHashAggAccumulatorPartialEmpty) {
    ArithmeticAverageHashAggAccumulatorPartial accumulator(
        outSlot(), spillSlot(), makeVariable(inSlot()));

    vm::ByteCode bytecode;
    accumulator.prepare(compileContext(), &accumulatorState());

    //
    // Accumulating any empty list of values should produce an empty partial sum.
    //
    accumulator.initialize(bytecode, accumulatorState());

    accumulator.finalize(bytecode, accumulatorState());

    auto [tagResult, valResult] = accumulatorState().getViewOfValue();
    ASSERT_EQ(tagResult, value::TypeTags::Object);
    // Convert the result to BSON to simplify the comparison.
    auto resultObj = [&]() {
        BSONObjBuilder resultBuilder;
        bson::convertToBsonObj(resultBuilder, value::getObjectView(valResult));
        return resultBuilder.obj();
    }();
    ASSERT_BSONOBJ_EQ_UNORDERED(BSON("count" << 0 << "ps" << BSON_ARRAY(16 << 0 << 0)), resultObj);
}

TEST_F(HashAggAccumulatorTest, ArithmeticAverageHashAggAccumulatorPartialSpilled) {
    ArithmeticAverageHashAggAccumulatorPartial accumulator(
        outSlot(), spillSlot(), makeVariable(inSlot()));

    vm::ByteCode bytecode;
    accumulator.prepare(compileContext(), &accumulatorState());
    accumulator.prepareForMerge(compileContext(), &accumulatorState());

    accumulator.initialize(bytecode, accumulatorState());

    // Input 4 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4));
    accumulator.accumulate(bytecode, accumulatorState());

    // Input 40 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(40));
    accumulator.accumulate(bytecode, accumulatorState());

    // Spill and reset the accumulator.
    moveAccumulatorStateToMockSpillStorage();
    accumulator.initialize(bytecode, accumulatorState());

    // Input 400 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(400));
    accumulator.accumulate(bytecode, accumulatorState());

    // Input 4,000 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4000));
    accumulator.accumulate(bytecode, accumulatorState());

    // Spill and reset the accumulator.
    moveAccumulatorStateToMockSpillStorage();
    accumulator.initialize(bytecode, accumulatorState());

    // Spill and then reset the empty accumulator.
    moveAccumulatorStateToMockSpillStorage();
    accumulator.initialize(bytecode, accumulatorState());

    // Input 40,000 to the accumulator.
    auto [tagInput, valInput] = value::makeCopyDecimal(Decimal128(40000));
    inAccessor().reset(true, tagInput, valInput);
    accumulator.accumulate(bytecode, accumulatorState());

    // Input 400,000 to the accumulator.
    inAccessor().reset(false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(400000));
    accumulator.accumulate(bytecode, accumulatorState());

    // Spill and reset the accumulator.
    moveAccumulatorStateToMockSpillStorage();
    accumulator.initialize(bytecode, accumulatorState());

    // Merge each of the spilled partial aggregates.
    value::MaterializedRow mergedAggregate(1);
    value::MaterializedSingleRowAccessor mergedAggregateAccessor(mergedAggregate, 0);
    auto [tagRecovered, valRecovered] = consumePartialAggregateFromMockSpillStorage();
    mergedAggregate.reset(0, true, tagRecovered, valRecovered);

    while (!isMockSpillStorageEmpty()) {
        std::tie(tagRecovered, valRecovered) = consumePartialAggregateFromMockSpillStorage();
        spillAccessor().reset(true, tagRecovered, valRecovered);
        accumulator.merge(bytecode, mergedAggregateAccessor);
    }

    // Repopulate the accumulator's state slot with the merged aggregate value so that we can
    // finalize it and validate the result.
    auto [tagMerged, valMerged] = mergedAggregate.copyOrMoveValue(0);
    accumulatorState().reset(true, tagMerged, valMerged);
    accumulator.finalize(bytecode, accumulatorState());

    auto [tagResult, valResult] = accumulatorState().getViewOfValue();
    ASSERT_EQ(tagResult, value::TypeTags::Object);
    // Convert the result to BSON to simplify the comparison.
    auto resultObj = [&]() {
        BSONObjBuilder resultBuilder;
        bson::convertToBsonObj(resultBuilder, value::getObjectView(valResult));
        return resultBuilder.obj();
    }();
    ASSERT_BSONOBJ_EQ_UNORDERED(BSON("count" << 6 << "ps" << BSON_ARRAY(1 << 404444 << 0 << 40000)),
                                resultObj);
}
}  // namespace mongo::sbe
