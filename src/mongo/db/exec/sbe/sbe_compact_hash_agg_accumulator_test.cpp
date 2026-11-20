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
#include "mongo/db/exec/sbe/values/value_size.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
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

    template <class Accumulator>
    Accumulator countAccumulator() {
        return Accumulator{_outSlot};
    }

    TagValueOwned compactSum(const Sequence& inputs, bool partial) {
        if (partial) {
            auto accumulator = sumAccumulator<CompactSumHashAggAccumulatorPartial>();
            return run(accumulator, inputs);
        }
        auto accumulator = sumAccumulator<CompactSumHashAggAccumulatorTerminal>();
        return run(accumulator, inputs);
    }

    TagValueOwned vmPush(const Sequence& inputs, int32_t sizeCap = kNoCap) {
        auto state = TagValueOwned::nothing();
        for (const auto& input : inputs) {
            state = vm::ByteCode::builtinAddToArrayCappedImpl(
                        std::move(state), input.view().copy(), sizeCap)
                        .moveToOwned();
        }
        return value::getArrayViewChecked(state)
            ->getAt(static_cast<size_t>(vm::AggArrayWithSize::kValues))
            .copy();
    }

    CompactPushHashAggAccumulator pushAccumulator(int32_t sizeCap = kNoCap) {
        return {_outSlot, makeVariable(_inSlot), sizeCap};
    }

    TagValueOwned compactPush(const Sequence& inputs, int32_t sizeCap = kNoCap) {
        auto accumulator = pushAccumulator(sizeCap);
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
        CompactStateTable table{std::move(layout)};
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
        CompactStateTable table{std::move(layout)};
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

    static constexpr int32_t kNoCap = std::numeric_limits<int32_t>::max();

    TagValueOwned run(CompactHashAggAccumulator& accumulator, const Sequence& inputs) {
        return withState(accumulator, [&](std::byte* buffer) {
            accumulate(accumulator, buffer, inputs);
            value::OwnedValueAccessor result;
            accumulator.finalize(buffer, result);
            return result.copyOrMoveValue();
        });
    }

private:
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

TEST_F(CompactHashAggAccumulatorTest, Count) {
    auto terminal = countAccumulator<CompactCountHashAggAccumulatorTerminal>();
    auto partial = countAccumulator<CompactCountHashAggAccumulatorPartial>();
    ASSERT_EQ(layoutOf(terminal).size(), sizeof(uint64_t));
    ASSERT_TRUE(identical(run(terminal, {}).view(), TagValueView::numberInt32(0)));

    auto inputs =
        sequence({TagValueView::numberInt32(9), TagValueView::null(), TagValueView::nothing()});
    ASSERT_TRUE(identical(run(terminal, inputs).view(), TagValueView::numberInt32(3)));
    ASSERT_BSONOBJ_EQ(BSON("" << BSON_ARRAY(16 << 3.0 << 0.0)),
                      toBson(run(partial, inputs).view()));
}

TEST_F(CompactHashAggAccumulatorTest, CountPromotesResult) {
    auto terminal = countAccumulator<CompactCountHashAggAccumulatorTerminal>();
    auto partial = countAccumulator<CompactCountHashAggAccumulatorPartial>();
    const auto position = CompactStateLayout{}.add<uint64_t>();
    auto afterIncrement = [&](CompactHashAggAccumulator& accumulator, uint64_t initialCount) {
        return withState(accumulator, [&](std::byte* buffer) {
            position(buffer) = initialCount;
            vm::ByteCode bytecode;
            accumulator.accumulate(bytecode, buffer);
            value::OwnedValueAccessor result;
            accumulator.finalize(buffer, result);
            return result.copyOrMoveValue();
        });
    };

    constexpr int64_t maxInt = std::numeric_limits<int32_t>::max();
    ASSERT_TRUE(
        identical(afterIncrement(terminal, maxInt).view(), TagValueView::numberInt64(maxInt + 1)));
    ASSERT_BSONOBJ_EQ(BSON("" << BSON_ARRAY(18 << static_cast<double>(maxInt + 1) << 0.0)),
                      toBson(afterIncrement(partial, maxInt).view()));
    ASSERT_TRUE(identical(afterIncrement(terminal, std::numeric_limits<int64_t>::max()).view(),
                          TagValueView::numberDouble(std::ldexp(1.0, 63))));
}

TEST_F(CompactHashAggAccumulatorTest, Push) {
    auto push = pushAccumulator();
    ASSERT_FALSE(layoutOf(push).triviallyDestructible());
    ASSERT_BSONOBJ_EQ(BSON("" << BSONArray()), toBson(compactPush({}).view()));
    ASSERT_BSONOBJ_EQ(BSON("" << BSONArray()),
                      toBson(compactPush(sequence({TagValueView::nothing()})).view()));
    auto inputs = sequence({TagValueView::numberInt32(1), TagValueView::nothing()});
    inputs.push_back(TagValueOwned::fromRaw(value::makeNewString("a long string")));
    inputs.push_back(array({TagValueView::numberDouble(2.5), TagValueView::null()}));
    ASSERT_BSONOBJ_EQ(BSON("" << BSON_ARRAY(1 << "a long string" << BSON_ARRAY(2.5 << BSONNULL))),
                      toBson(compactPush(inputs).view()));
}

TEST_F(CompactHashAggAccumulatorTest, PushMatchesVm) {
    for (const auto& inputs : sequences()) {
        if (inputs.empty()) {
            continue;
        }
        auto expected = vmPush(inputs);
        auto actual = compactPush(inputs);
        ASSERT_TRUE(identical(expected.view(), actual.view()))
            << expected.view() << " != " << actual.view();
    }
}

TEST_F(CompactHashAggAccumulatorTest, PushSizeCap) {
    auto inputs = sequence({TagValueView::numberInt32(1), TagValueView::numberInt32(2)});
    int32_t cap = 2 * value::getApproximateSize(value::TypeTags::NumberInt32, 0);
    ASSERT_THROWS_CODE(vmPush(inputs, cap), DBException, ErrorCodes::ExceededMemoryLimit);
    ASSERT_THROWS_CODE(compactPush(inputs, cap), DBException, ErrorCodes::ExceededMemoryLimit);
    ASSERT_BSONOBJ_EQ(BSON("" << BSON_ARRAY(1 << 2)), toBson(compactPush(inputs, cap + 1).view()));
}

TEST_F(CompactHashAggAccumulatorTest, PushHeapMemUsage) {
    auto accumulator = pushAccumulator();
    withState(accumulator, [&](std::byte* buffer) {
        CompactStateLayout single;
        const auto& values = single.add<CompactPushState>()(buffer).values;
        auto slots = [&] {
            return values.values().capacity() * sizeof(std::pair<value::TypeTags, value::Value>);
        };
        ASSERT_EQ(0u, accumulator.heapMemUsage(buffer));

        accumulate(accumulator,
                   buffer,
                   sequence({TagValueView::numberInt32(1),
                             TagValueView::nothing(),
                             TagValueView::numberDouble(2.5)}));
        ASSERT_EQ(2u, values.size());
        ASSERT_EQ(slots(), accumulator.heapMemUsage(buffer));

        Sequence strings;
        strings.push_back(TagValueOwned::fromRaw(value::makeNewString(std::string(1000, 'x'))));
        size_t payload = value::getApproximateSize(strings[0].tag(), strings[0].value()) -
            (sizeof(value::TypeTags) + sizeof(value::Value));
        accumulate(accumulator, buffer, strings);
        ASSERT_EQ(slots() + payload, accumulator.heapMemUsage(buffer));
    });
}

TEST_F(CompactHashAggAccumulatorTest, PushHeapMemUsageIsZeroAfterFinalize) {
    auto accumulator = pushAccumulator();
    withState(accumulator, [&](std::byte* buffer) {
        accumulate(accumulator, buffer, sequence({TagValueView::numberInt32(1)}));
        value::OwnedValueAccessor result;
        accumulator.finalize(buffer, result);
        ASSERT_EQ(0u, accumulator.heapMemUsage(buffer));
    });
}

TEST_F(CompactHashAggAccumulatorTest, PushEstimateSizeCountsTheConcreteAccumulator) {
    auto push = pushAccumulator();
    ASSERT_EQ(sizeof(CompactPushHashAggAccumulator) + inputSize(), push.estimateSize());
}

TEST_F(CompactHashAggAccumulatorTest, SumAndPushLayout) {
    CompactStateLayout layout;
    // Derive the expected offsets from sizeof/alignof, since the size of the standard library types
    // inside these states (e.g. std::vector) varies by platform and build mode.
    constexpr auto alignUp = [](size_t n, size_t alignment) {
        return (n + alignment - 1) & ~(alignment - 1);
    };
    constexpr size_t sumOffset = 0;
    constexpr size_t pushOffset =
        alignUp(sumOffset + sizeof(value::DoubleDoubleSumState), alignof(CompactPushState));
    constexpr size_t layoutAlignment =
        std::max(alignof(value::DoubleDoubleSumState), alignof(CompactPushState));
    ASSERT_EQ(sumOffset, layout.add<value::DoubleDoubleSumState>().offset());
    ASSERT_EQ(pushOffset, layout.add<CompactPushState>().offset());
    ASSERT_EQ(pushOffset + sizeof(CompactPushState), layout.size());
    ASSERT_EQ(layoutAlignment, layout.alignment());
    ASSERT_FALSE(layout.triviallyDestructible());
}

}  // namespace
}  // namespace mongo::sbe
