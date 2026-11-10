// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/compact_hash_agg_state.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/double_double_sum_state.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/code_fragment.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace mongo::sbe {

/**
 * Base class for compact SBE $group accumulators. The accumulator implementations that predate this
 * used SBE values as intermediate accumulator state. There is some overhead in that approach. The
 * accumulator implementations here use a more compact form, which saves on memory allocations
 * during query execution.
 */
class CompactHashAggAccumulator {
public:
    CompactHashAggAccumulator(value::SlotId outSlot, std::unique_ptr<EExpression> inputExpr)
        : _outSlot(outSlot), _inputExpr(std::move(inputExpr)) {}

    virtual ~CompactHashAggAccumulator() = default;

    virtual std::unique_ptr<CompactHashAggAccumulator> clone() const = 0;

    value::SlotId getOutSlot() const {
        return _outSlot;
    }

    void prepare(CompileCtx& ctx);

    virtual void addState(CompactStateLayout& layout) = 0;

    virtual void accumulate(vm::ByteCode& bytecode, std::byte* buffer) const = 0;

    virtual void finalize(std::byte* buffer, value::OwnedValueAccessor& result) const = 0;

    virtual size_t heapMemUsage(const std::byte* buffer) const = 0;

    virtual size_t estimateSize() const = 0;

    std::vector<DebugPrinter::Block> debugPrint() const;

protected:
    value::TagValueMaybeOwned runInput(vm::ByteCode& bytecode) const;

    size_t estimateSizeWithInput(size_t objectSize) const;

    virtual std::string getDebugName() const = 0;

    value::SlotId _outSlot;
    std::unique_ptr<EExpression> _inputExpr;

private:
    std::unique_ptr<vm::CodeFragment> _inputCode;
};

/**
 * Base class for CRTP pattern. 'State' is the per-group state this accumulator constructs in each
 * group's buffer.
 */
template <class State>
class TypedCompactHashAggAccumulator : public CompactHashAggAccumulator {
public:
    using CompactHashAggAccumulator::CompactHashAggAccumulator;

    void addState(CompactStateLayout& layout) final {
        _position = layout.add<State>();
    }

    void accumulate(vm::ByteCode& bytecode, std::byte* buffer) const final {
        accumulateValue(runInput(bytecode), _position(buffer));
    }

    void finalize(std::byte* buffer, value::OwnedValueAccessor& result) const final {
        result.reset(finalizeState(_position(buffer)));
    }

    size_t heapMemUsage(const std::byte* buffer) const final {
        return stateHeapMemUsage(_position(buffer));
    }

protected:
    virtual void accumulateValue(value::TagValueMaybeOwned input, State& state) const = 0;
    virtual value::TagValueMaybeOwned finalizeState(State& state) const = 0;
    virtual size_t stateHeapMemUsage(const State& state) const = 0;

private:
    CompactStatePosition<State> _position;
};

class CompactSumHashAggAccumulatorBase
    : public TypedCompactHashAggAccumulator<value::DoubleDoubleSumState> {
public:
    using TypedCompactHashAggAccumulator::TypedCompactHashAggAccumulator;

    size_t estimateSize() const final {
        return estimateSizeWithInput(sizeof(*this));
    }

protected:
    void accumulateValue(value::TagValueMaybeOwned input,
                         value::DoubleDoubleSumState& state) const final;

    size_t stateHeapMemUsage(const value::DoubleDoubleSumState& state) const final {
        return 0;
    }
};

class CompactSumHashAggAccumulatorTerminal final : public CompactSumHashAggAccumulatorBase {
public:
    using CompactSumHashAggAccumulatorBase::CompactSumHashAggAccumulatorBase;

    std::unique_ptr<CompactHashAggAccumulator> clone() const final {
        return std::make_unique<CompactSumHashAggAccumulatorTerminal>(_outSlot,
                                                                      _inputExpr->clone());
    }

protected:
    value::TagValueMaybeOwned finalizeState(value::DoubleDoubleSumState& state) const final;

    std::string getDebugName() const final {
        return "_compactSum";
    }
};

class CompactSumHashAggAccumulatorPartial final : public CompactSumHashAggAccumulatorBase {
public:
    using CompactSumHashAggAccumulatorBase::CompactSumHashAggAccumulatorBase;

    std::unique_ptr<CompactHashAggAccumulator> clone() const final {
        return std::make_unique<CompactSumHashAggAccumulatorPartial>(_outSlot, _inputExpr->clone());
    }

protected:
    value::TagValueMaybeOwned finalizeState(value::DoubleDoubleSumState& state) const final;

    std::string getDebugName() const final {
        return "_compactPartialSum";
    }
};

namespace size_estimator {
inline size_t estimate(const std::unique_ptr<CompactHashAggAccumulator>& accumulator) {
    return accumulator->estimateSize();
}
}  // namespace size_estimator
}  // namespace mongo::sbe
