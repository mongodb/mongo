// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/compact_hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/compact_hash_agg_state.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mongo::sbe {
namespace {

using value::TagValueOwned;
using value::TagValueView;
using Sequence = std::vector<TagValueOwned>;

TagValueOwned decimal(const std::string& literal) {
    return TagValueOwned::fromRaw(value::makeCopyDecimal(Decimal128{literal}));
}

Sequence sequence(std::initializer_list<TagValueView> views) {
    Sequence values;
    for (auto view : views) {
        values.push_back(view.copy());
    }
    return values;
}

TagValueOwned array(std::initializer_list<TagValueView> views) {
    auto result = TagValueOwned::fromRaw(value::makeNewArray());
    for (auto view : views) {
        value::getArrayViewChecked(result)->push_back(view.copy());
    }
    return result;
}

Sequence edgeValues() {
    using DoubleLimits = std::numeric_limits<double>;
    auto values = sequence({TagValueView::numberInt32(1),
                            TagValueView::numberInt32(-1),
                            TagValueView::numberInt32(std::numeric_limits<int32_t>::max()),
                            TagValueView::numberInt64(int64_t{1} << 53),
                            TagValueView::numberInt64(std::numeric_limits<int64_t>::max()),
                            TagValueView::numberInt64(std::numeric_limits<int64_t>::min()),
                            TagValueView::numberDouble(-0.0),
                            TagValueView::numberDouble(0.1),
                            TagValueView::numberDouble(DoubleLimits::max()),
                            TagValueView::numberDouble(DoubleLimits::denorm_min()),
                            TagValueView::numberDouble(DoubleLimits::infinity()),
                            TagValueView::numberDouble(-DoubleLimits::infinity()),
                            TagValueView::numberDouble(DoubleLimits::quiet_NaN()),
                            TagValueView::null(),
                            TagValueView::nothing()});
    for (auto literal : {"1.5", "-0", "1E+6000", "Infinity", "NaN"}) {
        values.push_back(decimal(literal));
    }
    values.push_back(TagValueOwned::fromRaw(value::makeNewString("not a number")));
    values.push_back(array({TagValueView::numberInt32(1), values.back().view()}));
    return values;
}

std::vector<Sequence> sequences() {
    std::vector<Sequence> result(1);
    auto edges = edgeValues();
    for (const auto& first : edges) {
        result.push_back(sequence({first.view()}));
        for (const auto& second : edges) {
            result.push_back(sequence({first.view(), second.view()}));
        }
    }
    result.push_back(sequence({TagValueView::numberDouble(std::numeric_limits<double>::max()),
                               TagValueView::numberDouble(-std::ldexp(1.0, 969)),
                               TagValueView::numberDouble(-std::ldexp(1.0, 969)),
                               TagValueView::numberDouble(std::ldexp(1.0, 970)),
                               TagValueView::numberDouble(-std::numeric_limits<double>::infinity()),
                               TagValueView::numberDouble(1)}));
    result.emplace_back();
    for (int i = 0; i < 10; ++i) {
        result.back().push_back(TagValueOwned::numberDouble(0.1));
    }
    return result;
}

// Compare representations, not just values, to verify that compact accumulation
// exactly matches the VM. Recurse into arrays to apply the same strict checks to
// partial-sum elements, and compare Decimal128 encodings rather than numeric values.
bool identical(TagValueView lhs, TagValueView rhs) {
    if (lhs.tag != rhs.tag) {
        return false;
    }
    switch (lhs.tag) {
        case value::TypeTags::Array:
            return std::ranges::equal(value::getArrayView(lhs.value)->values(),
                                      value::getArrayView(rhs.value)->values(),
                                      [](auto left, auto right) {
                                          return identical({left.first, left.second},
                                                           {right.first, right.second});
                                      });
        case value::TypeTags::NumberDecimal:
            return value::bitcastTo<Decimal128>(lhs.value).isBinaryEqual(
                value::bitcastTo<Decimal128>(rhs.value));
        default:
            if (value::isShallowType(lhs.tag)) {
                return lhs.value == rhs.value;
            }
            auto [tag, val] = value::compareValue(lhs.tag, lhs.value, rhs.tag, rhs.value);
            return tag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(val) == 0;
    }
}

BSONObj toBson(TagValueView view) {
    BSONObjBuilder builder;
    bson::appendValueToBsonObj(builder, "", view.tag, view.value);
    return builder.obj();
}

class CompactHashAggAccumulatorTest : public unittest::Test {
protected:
    TagValueOwned vmSum(const Sequence& inputs, bool partial) {
        auto state = TagValueOwned::fromRaw(vm::ByteCode::genericInitializeDoubleDoubleSumState());
        auto* array = value::getArrayViewChecked(state);
        for (const auto& input : inputs) {
            vm::ByteCode::aggDoubleDoubleSumImpl(array, input.tag(), input.value());
        }
        return (partial ? vm::ByteCode::builtinDoubleDoublePartialSumFinalizeImpl(state.tag(),
                                                                                  state.value())
                        : vm::ByteCode::aggDoubleDoubleSumFinalizeImpl(array))
            .moveToOwned();
    }

    size_t inputSize() const {
        std::unique_ptr<EExpression> input = makeVariable(_inSlot);
        return size_estimator::estimate(input);
    }

    template <class Accumulator>
    Accumulator sumAccumulator() {
        return {_outSlot, makeVariable(_inSlot)};
    }

    TagValueOwned compactSum(const Sequence& inputs, bool partial) {
        if (partial) {
            auto accumulator = sumAccumulator<CompactSumHashAggAccumulatorPartial>();
            return run(accumulator, inputs);
        }
        auto accumulator = sumAccumulator<CompactSumHashAggAccumulatorTerminal>();
        return run(accumulator, inputs);
    }

    static CompactStateLayout layoutOf(CompactHashAggAccumulator& accumulator) {
        CompactStateLayout layout;
        accumulator.addState(layout);
        return layout;
    }

    template <class Body>
    auto withState(CompactHashAggAccumulator& accumulator, Body body) {
        accumulator.prepare(_ctx);
        auto layout = layoutOf(accumulator);
        CompactStateTable table{layout};
        return body(insertGroup(table));
    }

    static std::byte* insertGroup(CompactStateTable& table) {
        return table.findOrInsert(value::MaterializedRow{})->second;
    }

    void accumulate(const CompactHashAggAccumulator& accumulator,
                    std::byte* buffer,
                    const Sequence& inputs) {
        vm::ByteCode bytecode;
        for (const auto& input : inputs) {
            _ctx.getRuntimeEnvAccessor(_inSlot)->reset(input.view());
            accumulator.accumulate(bytecode, buffer);
        }
    }

    std::pair<TagValueOwned, TagValueOwned> sumInputsAndConstantInOneBuffer(const Sequence& inputs,
                                                                            int32_t constant) {
        CompactSumHashAggAccumulatorTerminal first{_outSlot, makeVariable(_inSlot)};
        CompactSumHashAggAccumulatorTerminal second{_outSlot, makeInt32Constant(constant)};
        first.prepare(_ctx);
        second.prepare(_ctx);
        CompactStateLayout layout;
        first.addState(layout);
        second.addState(layout);
        CompactStateTable table{layout};
        std::byte* buffer = insertGroup(table);
        vm::ByteCode bytecode;
        for (const auto& input : inputs) {
            _ctx.getRuntimeEnvAccessor(_inSlot)->reset(input.view());
            first.accumulate(bytecode, buffer);
            second.accumulate(bytecode, buffer);
        }
        value::OwnedValueAccessor firstResult;
        value::OwnedValueAccessor secondResult;
        first.finalize(buffer, firstResult);
        second.finalize(buffer, secondResult);
        return {firstResult.copyOrMoveValue(), secondResult.copyOrMoveValue()};
    }

private:
    TagValueOwned run(CompactHashAggAccumulator& accumulator, const Sequence& inputs) {
        return withState(accumulator, [&](std::byte* buffer) {
            accumulate(accumulator, buffer, inputs);
            value::OwnedValueAccessor result;
            accumulator.finalize(buffer, result);
            return result.copyOrMoveValue();
        });
    }

    value::SlotIdGenerator _ids;
    std::unique_ptr<RuntimeEnvironment> _env = std::make_unique<RuntimeEnvironment>();
    value::SlotId _inSlot = _env->registerSlot(value::TypeTags::Nothing, 0, false, &_ids);
    value::SlotId _outSlot = _ids.generate();
    CompileCtx _ctx{std::move(_env)};
};

TEST_F(CompactHashAggAccumulatorTest, Sum) {
    auto sum = sumAccumulator<CompactSumHashAggAccumulatorTerminal>();
    ASSERT_TRUE(layoutOf(sum).triviallyDestructible());
    ASSERT_BSONOBJ_EQ(BSON("" << 0), toBson(compactSum({}, false).view()));
    ASSERT_BSONOBJ_EQ(BSON("" << 5),
                      toBson(compactSum(sequence({TagValueView::numberInt32(2),
                                                  TagValueView::null(),
                                                  TagValueView::numberInt32(3)}),
                                        false)
                                 .view()));
    ASSERT_BSONOBJ_EQ(
        BSON("" << 5.5),
        toBson(compactSum(sequence({TagValueView::numberInt64(2), TagValueView::numberDouble(3.5)}),
                          false)
                   .view()));
    auto withDecimal = sequence({TagValueView::numberInt32(2)});
    withDecimal.push_back(decimal("1.5"));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128("3.5")), toBson(compactSum(withDecimal, false).view()));
}

TEST_F(CompactHashAggAccumulatorTest, SumEstimateSizeCountsTheConcreteAccumulator) {
    auto terminal = sumAccumulator<CompactSumHashAggAccumulatorTerminal>();
    ASSERT_EQ(sizeof(CompactSumHashAggAccumulatorTerminal) + inputSize(), terminal.estimateSize());
    auto partial = sumAccumulator<CompactSumHashAggAccumulatorPartial>();
    ASSERT_EQ(sizeof(CompactSumHashAggAccumulatorPartial) + inputSize(), partial.estimateSize());
}

TEST_F(CompactHashAggAccumulatorTest, AccumulatorsSharingABufferKeepSeparateStates) {
    auto [inputSum, constantSum] =
        sumInputsAndConstantInOneBuffer(sequence({TagValueView::numberInt32(1),
                                                  TagValueView::numberInt32(2),
                                                  TagValueView::numberInt32(3)}),
                                        10);
    ASSERT_BSONOBJ_EQ(BSON("" << 6), toBson(inputSum.view()));
    ASSERT_BSONOBJ_EQ(BSON("" << 30), toBson(constantSum.view()));
}

TEST_F(CompactHashAggAccumulatorTest, PartialSum) {
    ASSERT_BSONOBJ_EQ(BSON("" << BSON_ARRAY(16 << 0.0 << 0.0)),
                      toBson(compactSum({}, true).view()));
    ASSERT_BSONOBJ_EQ(
        BSON("" << BSON_ARRAY(1 << 5.5 << 0.0)),
        toBson(compactSum(sequence({TagValueView::numberInt64(2), TagValueView::numberDouble(3.5)}),
                          true)
                   .view()));
    auto withDecimal = sequence({TagValueView::numberInt32(2)});
    withDecimal.push_back(decimal("1.5"));
    ASSERT_BSONOBJ_EQ(BSON("" << BSON_ARRAY(16 << 2.0 << 0.0 << Decimal128("1.5"))),
                      toBson(compactSum(withDecimal, true).view()));
}

TEST_F(CompactHashAggAccumulatorTest, SumMatchesVm) {
    for (const auto& inputs : sequences()) {
        for (bool partial : {false, true}) {
            auto expected = vmSum(inputs, partial);
            auto actual = compactSum(inputs, partial);
            ASSERT_TRUE(identical(expected.view(), actual.view()))
                << expected.view() << " != " << actual.view();
        }
    }
}

}  // namespace
}  // namespace mongo::sbe
