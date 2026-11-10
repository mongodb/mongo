// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/compact_hash_agg_accumulator.h"

#include "mongo/db/exec/sbe/size_estimator.h"

#include <memory>

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

}  // namespace mongo::sbe
