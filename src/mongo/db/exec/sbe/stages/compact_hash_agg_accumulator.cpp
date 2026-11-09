// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/compact_hash_agg_accumulator.h"

#include "mongo/db/exec/sbe/size_estimator.h"

#include <memory>
#include <type_traits>

namespace mongo::sbe {

void CompactHashAggAccumulator::prepare(CompileCtx& ctx) {
    _inputCode = _inputExpr->compile(ctx);
}

value::TagValueMaybeOwned CompactHashAggAccumulator::runInput(vm::ByteCode& bytecode) const {
    return bytecode.run(_inputCode.get());
}

size_t CompactHashAggAccumulator::estimateSizeWithInput(size_t objectSize) const {
    return objectSize + size_estimator::estimate(_inputExpr);
}

std::vector<DebugPrinter::Block> CompactHashAggAccumulator::debugPrint() const {
    std::vector<DebugPrinter::Block> blocks;
    DebugPrinter::addKeyword(blocks, getDebugName());
    blocks.emplace_back("`(`");
    DebugPrinter::addBlocks(blocks, _inputExpr->debugPrint());
    blocks.emplace_back("`)");
    return blocks;
}

void CompactSumHashAggAccumulatorBase::accumulateValue(value::TagValueMaybeOwned input,
                                                       value::DoubleDoubleSumState& state) const {
    state.add(input.tag(), input.value());
}

value::TagValueMaybeOwned CompactSumHashAggAccumulatorTerminal::finalizeState(
    value::DoubleDoubleSumState& state) const {
    return state.finalize();
}

value::TagValueMaybeOwned CompactSumHashAggAccumulatorPartial::finalizeState(
    value::DoubleDoubleSumState& state) const {
    auto sumArray =
        value::TagValueOwned::fromRaw(vm::ByteCode::genericInitializeDoubleDoubleSumState());
    state.write(*value::getArrayViewChecked(sumArray));
    return vm::ByteCode::builtinDoubleDoublePartialSumFinalizeImpl(sumArray.tag(),
                                                                   sumArray.value());
}

void CompactPushHashAggAccumulator::accumulateValue(value::TagValueMaybeOwned input,
                                                    CompactPushState& state) const {
    if (input.tag() == value::TypeTags::Nothing) {
        return;
    }
    vm::ByteCode::addToArrayCapped(state.values, state.valuesSize, std::move(input), _sizeCap);
}

value::TagValueMaybeOwned CompactPushHashAggAccumulator::finalizeState(
    CompactPushState& state) const {
    auto values = std::make_unique<value::Array>(std::move(state.values));
    state.valuesSize = 0;
    return value::TagValueOwned::fromRaw(value::TypeTags::Array,
                                         value::bitcastFrom<value::Array*>(values.release()));
}

size_t CompactPushHashAggAccumulator::stateHeapMemUsage(const CompactPushState& state) const {
    const auto& values = state.values.values();
    using Element = std::remove_cvref_t<decltype(values)>::value_type;
    static_assert(std::is_same_v<Element, std::pair<value::TypeTags, value::Value>>);
    constexpr size_t kInlineSize = sizeof(value::TypeTags) + sizeof(value::Value);
    return
        /* The vector's whole buffer, including unused capacity. */
        values.capacity() * sizeof(Element) +
        /* The approximate size of every element added so far, including the inline (tag, value)
           pair of each one. */
        state.valuesSize -
        /* Remove the inline (tag, value) pairs, which are already in the buffer counted above, so
           that only the out-of-line payload of each element is left. */
        values.size() * kInlineSize;
}

}  // namespace mongo::sbe
