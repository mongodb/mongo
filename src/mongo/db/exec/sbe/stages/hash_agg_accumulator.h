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

#pragma once

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/hashagg_base.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/code_fragment.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/query_knobs_gen.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
/**
 * Base class for the executable accumulator operators used by SBE's HashAggStage.
 */
class HashAggAccumulator {
public:
    HashAggAccumulator(value::SlotId outSlot, value::SlotId spillSlot)
        : _outSlot(outSlot), _spillSlot(spillSlot) {}

    virtual ~HashAggAccumulator() = default;

    virtual std::unique_ptr<HashAggAccumulator> clone() const = 0;

    value::SlotId getOutSlot() const {
        return _outSlot;
    }

    value::SlotId getSpillSlot() const {
        return _spillSlot;
    }

    /**
     * Must be called at least once before executing any initialize, accumulate, or finalize
     * operations.
     */
    virtual void prepare(CompileCtx& ctx, value::SlotAccessor* accumulatorAccessor) = 0;

    /**
     * Must be called at least once before executing an merge operations.
     */
    virtual void prepareForMerge(CompileCtx& ctx, value::SlotAccessor* accumulatorAccessor) = 0;

    /**
     * Updates the 'accumulatorState' accessor to set the accumulator to its initial state, in which
     * a 'finalize()' call will produce the output expected from accumulating an exmpty list. Must
     * be called before executing any accumulate or merge operations.
     */
    virtual void initialize(vm::ByteCode& bytecode, HashAggAccessor& accumulatorState) const = 0;

    /**
     * Reads the current accumulator state and a new value and writes out the updated accumulator
     * state---based on the new value---to the 'accumulatorState' accessor.
     *
     * The sources of the input accumulator state and input value are defined by the child
     * implementations of this method.
     */
    virtual void accumulate(vm::ByteCode& bytecode, HashAggAccessor& accumulatorState) const = 0;

    /**
     * Reads the current accumulator state, updates the accumulator's state with the recovered state
     * of an accumulator that was spilled to disk, and writes the updated state to the
     * 'accumulatorState' accessor.
     *
     * The sources of the input accumulator state and input recovered state are defined by the child
     * implementations of this method.
     */
    virtual void merge(vm::ByteCode& bytecode,
                       value::MaterializedSingleRowAccessor& accumulatorState) const = 0;

    /**
     * Computes the final accumulation result based on the values added with `accumulate()` so far
     * and saves it to the `accumulatorState` accessor.
     */
    virtual void finalize(vm::ByteCode& bytecode,
                          value::AssignableSlotAccessor& accumulatorState) const = 0;

    virtual size_t estimateSize() const = 0;

    /**
     * Returns a developer-readable representation of the accumulator's intialization expression if
     * it has one, std::none otherwise.
     */
    virtual boost::optional<std::vector<DebugPrinter::Block>> debugPrintInitialize() const = 0;

    /**
     * Returns a developer-readable reprsentation of the accumulator's accumulation expression. When
     * accumulation is computed by the SBE VM, this representation will be an ABT expression. In
     * other cases, it will be a keyword that will uniquely identify the execution method.
     */
    virtual std::vector<DebugPrinter::Block> debugPrintAccumulate() const = 0;

    /**
     * Returns a developer-readable reprsentation of the accumulator's merge expression. When
     * merging is computed by the SBE VM, this representation will be an ABT expression. In other
     * cases, it will be a keyword that will uniquely identify the execution method.
     */
    virtual std::vector<DebugPrinter::Block> debugPrintMerge() const = 0;

protected:
    /**
     * The slot that stores the accumulator's state and usually also stores its final value.
     * Accumulator implementations do not reference this slot, but it is stored with the accumulator
     * for the HashAggStage to use.
     */
    value::SlotId _outSlot;

    /**
     * The slot that stores the recovered state of a spilled accumulator after reading it from disk.
     * Accumulator implementations do not reference this slot, but it is stored with the accumulator
     * for the HashAggStage to use.
     */
    value::SlotId _spillSlot;
};

/**
 * A general-purpose accumulator whose behavior is specified by initialize, accumulate, and merge
 * expressions, which are defined with ABT and execute in the SBE VM.
 */
class CompiledHashAggAccumulator : public HashAggAccumulator {
public:
    CompiledHashAggAccumulator(value::SlotId outSlot,
                               value::SlotId spillSlot,
                               std::unique_ptr<EExpression> accumulatorExpr,
                               std::unique_ptr<EExpression> mergingExpr,
                               std::unique_ptr<EExpression> optionalInitializerExpression = {})
        : HashAggAccumulator(outSlot, spillSlot),
          _accumulatorExpr(std::move(accumulatorExpr)),
          _mergingExpr(std::move(mergingExpr)),
          _optionalInitializerExpr(std::move(optionalInitializerExpression)) {}

    std::unique_ptr<HashAggAccumulator> clone() const final {
        return std::make_unique<CompiledHashAggAccumulator>(
            _outSlot,
            _spillSlot,
            _accumulatorExpr->clone(),
            _mergingExpr->clone(),
            _optionalInitializerExpr ? _optionalInitializerExpr->clone() : nullptr);
    };

    void prepare(CompileCtx& ctx, value::SlotAccessor* accumulatorAccessor) final;

    void prepareForMerge(CompileCtx& ctx, value::SlotAccessor* accumulatorAccessor) final;

    void initialize(vm::ByteCode& bytecode, HashAggAccessor& accumulatorState) const final;

    void accumulate(vm::ByteCode& bytecode, HashAggAccessor& accumulatorState) const final {
        auto [owned, tag, val] = bytecode.run(_accumulatorCode.get());
        accumulatorState.reset(owned, tag, val);
    };

    void merge(vm::ByteCode& bytecode,
               value::MaterializedSingleRowAccessor& accumulatorState) const final;

    void finalize(vm::ByteCode&, value::AssignableSlotAccessor&) const final {};

    size_t estimateSize() const final;

    boost::optional<std::vector<DebugPrinter::Block>> debugPrintInitialize() const final;
    std::vector<DebugPrinter::Block> debugPrintAccumulate() const final;
    std::vector<DebugPrinter::Block> debugPrintMerge() const final;

private:
    /**
     * An EExpression program that
     *   1. reads the current accumulator state,
     *.  2. reads a value and transforms it as necessary to produce a desired accumulator input, and
     *.  3. produces the updated accumulator state.
     *
     * In normal use, the input accumulator state will be read from a slot that is bound to the
     * accumulatorState' accessor used as the destination for initialize, accumulate, and merge
     * operations.
     *
     * The program is compiled to a 'CodeFragment' as part of the 'prepare()' step.
     */
    std::unique_ptr<EExpression> _accumulatorExpr;
    std::unique_ptr<vm::CodeFragment> _accumulatorCode;

    /**
     * An EExpression program that
     *   1. reads the current accumulator state,
     *.  2. reads the recovered state from a previously spilled accumulator, and
     *.  3. produces the updated accumulator state.
     *
     * In normal use, the input accumulator state will be read from a slot that is bound to the
     * 'accumulatorState' accessor used as the destination for initialize, accumulate, and merge
     * operations, and the recovered state will be read from the '_spillSlot' input.
     *
     * The program is compiled to a 'CodeFragment' as part of the 'prepareForMerge()' step.
     */
    std::unique_ptr<EExpression> _mergingExpr;
    std::unique_ptr<vm::CodeFragment> _mergingCode;

    /**
     * An EExpression program that produces a value appropriate for an accumulator that has not yet
     * processed any input values.
     *
     * This expression may be null. When it is non-null, it is compiled to a 'CodeFragment' as part
     * of the 'prepareForMerge()' step.
     */
    std::unique_ptr<EExpression> _optionalInitializerExpr;
    std::unique_ptr<vm::CodeFragment> _optionalInitializerCode;
};

/**
 * A base class for accumulators whose accumulate, merge, and finalize operations are implemented
 * natively instead of executing on the SBE VM.
 */
class SinglePurposeHashAggAccumulator : public HashAggAccumulator {
public:
    SinglePurposeHashAggAccumulator(value::SlotId outSlot,
                                    value::SlotId spillSlot,
                                    std::unique_ptr<EExpression> transformExpr,
                                    boost::optional<value::SlotId> collatorSlot)
        : HashAggAccumulator(outSlot, spillSlot), _transformExpr(std::move(transformExpr)) {}

    void prepare(CompileCtx& ctx, value::SlotAccessor* accumulatorAccessor) final;

    void prepareForMerge(CompileCtx& ctx, value::SlotAccessor* accumulatorAccessor) final;

    void accumulate(vm::ByteCode& bytecode, HashAggAccessor& accumulatorState) const final;

    void merge(vm::ByteCode& bytecode,
               value::MaterializedSingleRowAccessor& accumulatorState) const final;

    void finalize(vm::ByteCode&, value::AssignableSlotAccessor& accumulatorState) const override;

    size_t estimateSize() const override;

    boost::optional<std::vector<DebugPrinter::Block>> debugPrintInitialize() const final;
    std::vector<DebugPrinter::Block> debugPrintAccumulate() const final;
    std::vector<DebugPrinter::Block> debugPrintMerge() const final;

protected:
    /**
     * Child implementations of this class can override this method to perform any follow-on
     * preparation that may be necessary after 'SinglePurposeHashAggAccumulator::prepare()'
     * finishes.
     */
    virtual void singlePurposePrepare(CompileCtx& ctx) {}

    /**
     * Child implementations of this class must override this method to accept an input value as an
     * (owned, type tag, value) pair and update the accumulator state stored in the
     * 'accumulatorState' accessor to incorporate the new value.
     */
    virtual void accumulateTransformedValue(bool ownedField,
                                            value::TypeTags tagField,
                                            value::Value valField,
                                            HashAggAccessor& accumulatorState) const = 0;

    /**
     * Child implementations of this class must override this method to accept the recovered state
     * of a spilled accumulator as a (type tag, value pair) and merge it into the saved state in the
     * 'accumulatorState' accessor.
     */
    virtual void mergeRecoveredState(
        bool ownedRecoveredState,
        value::TypeTags tagRecoveredState,
        value::Value valRecoveredState,
        value::MaterializedSingleRowAccessor& accumulatorState) const = 0;

    /**
     * Child implementations of this class must override this method to accept the accumulator state
     * as a (type tag, value) pair and write the final result of the accumulation to the
     * 'result' accessor.
     */
    virtual void finalizePartialAggregate(value::TypeTags tagPartialAggregate,
                                          value::Value valPartialAggregate,  // Owned
                                          value::AssignableSlotAccessor& result) const = 0;

    /**
     * Child implementations of this class must override this method to provide a unique name that
     * will identify the implementation in diagostic descriptions of HashAggStages.
     */
    virtual std::string getDebugName() const = 0;

    /**
     * An EExpression program that reads a value and transforms it as necessary to produce a desired
     * accumulator input. Although a single-purpose accumulator does not use the VM to compute the
     * accumulation function, it still uses this program to produce the accumulator inputs.
     */
    std::unique_ptr<EExpression> _transformExpr;

private:
    /**
     * The '_transformExpr' program is compiled to a 'CodeFragment' as part of the 'prepare()' step.
     */
    std::unique_ptr<vm::CodeFragment> _transformCode;

    /**
     * An EExpression program that reads the recovered state from a previously spilled accumulator.
     * In normal use, the recovered state will be read from the '_spillSlot' input.
     * The program is compiled to a 'CodeFragment' as part of the 'prepareForMerge()' step.
     */
    std::unique_ptr<EExpression> _recoverSpilledStateExpr;
    std::unique_ptr<vm::CodeFragment> _recoverSpilledStateCode;
};

/**
 * Base class for the single-purpose implementation of the $avg accumulator.
 */
class ArithmeticAverageHashAggAccumulatorBase : public SinglePurposeHashAggAccumulator {
public:
    using SinglePurposeHashAggAccumulator::SinglePurposeHashAggAccumulator;

    void initialize(vm::ByteCode& bytecode, HashAggAccessor& accumulatorState) const final;

protected:
    void accumulateTransformedValue(bool ownedField,
                                    value::TypeTags tagField,
                                    value::Value valField,
                                    HashAggAccessor& accumulatorState) const final;

    void mergeRecoveredState(bool ownedRecoveredState,
                             value::TypeTags tagRecoveredState,
                             value::Value valRecoveredState,
                             value::MaterializedSingleRowAccessor& accumulatorState) const final;

    std::string getDebugName() const final {
        return "_internalArithmeticAverage";
    }
};

/**
 * Single-purpose implementation of the $avg accumulator for when the desired output is the final
 * result of the $avg expression.
 */
class ArithmeticAverageHashAggAccumulatorTerminal : public ArithmeticAverageHashAggAccumulatorBase {
public:
    using ArithmeticAverageHashAggAccumulatorBase::ArithmeticAverageHashAggAccumulatorBase;

    std::unique_ptr<HashAggAccumulator> clone() const final {
        return std::make_unique<ArithmeticAverageHashAggAccumulatorTerminal>(
            _outSlot, _spillSlot, _transformExpr->clone(), boost::none);
    }

protected:
    void finalizePartialAggregate(value::TypeTags tagPartialAggregate,
                                  value::Value valPartialAggregate,  // Owned
                                  value::AssignableSlotAccessor& result) const final;
};

/**
 * Single-purpose implementation of the $avg accumulator for use by shards producing results for a
 * merge operation, in which case the desired output is not the final result but instead an array
 * containing the separated count and sum values (i.e., the partial aggregate).
 */
class ArithmeticAverageHashAggAccumulatorPartial : public ArithmeticAverageHashAggAccumulatorBase {
public:
    using ArithmeticAverageHashAggAccumulatorBase::ArithmeticAverageHashAggAccumulatorBase;

    std::unique_ptr<HashAggAccumulator> clone() const final {
        return std::make_unique<ArithmeticAverageHashAggAccumulatorPartial>(
            _outSlot, _spillSlot, _transformExpr->clone(), boost::none);
    }

protected:
    void finalizePartialAggregate(value::TypeTags tagPartialAggregate,
                                  value::Value valPartialAggregate,  // Owned
                                  value::AssignableSlotAccessor& result) const final;
};

class AddToSetHashAggAccumulator : public SinglePurposeHashAggAccumulator {
public:
    AddToSetHashAggAccumulator(value::SlotId outSlot,
                               value::SlotId spillSlot,
                               std::unique_ptr<EExpression> transformExpr,
                               boost::optional<value::SlotId> collatorSlot)
        : SinglePurposeHashAggAccumulator(
              outSlot, spillSlot, std::move(transformExpr), collatorSlot),
          _collatorSlot(collatorSlot) {
        _sizeCap = internalQueryMaxAddToSetBytes.load();
    }

    AddToSetHashAggAccumulator(value::SlotId outSlot,
                               value::SlotId spillSlot,
                               std::unique_ptr<EExpression> transformExpr,
                               boost::optional<value::SlotId> collatorSlot,
                               int64_t sizeCap)
        : SinglePurposeHashAggAccumulator(
              outSlot, spillSlot, std::move(transformExpr), collatorSlot),
          _collatorSlot(collatorSlot),
          _sizeCap(sizeCap) {}

    std::unique_ptr<HashAggAccumulator> clone() const final {
        return std::make_unique<AddToSetHashAggAccumulator>(
            _outSlot, _spillSlot, _transformExpr->clone(), _collatorSlot, _sizeCap);
    }

    void initialize(vm::ByteCode& bytecode, HashAggAccessor& accumulatorState) const final;

protected:
    void singlePurposePrepare(CompileCtx& ctx) final;

    void accumulateTransformedValue(bool ownedField,
                                    value::TypeTags tagField,
                                    value::Value valField,
                                    HashAggAccessor& accumulatorState) const final;

    void mergeRecoveredState(bool ownedRecoveredState,
                             value::TypeTags tagRecoveredState,
                             value::Value valRecoveredState,
                             value::MaterializedSingleRowAccessor& accumulatorState) const final;

    void finalizePartialAggregate(value::TypeTags tagPartialAggregate,
                                  value::Value valPartialAggregate,  // Owned
                                  value::AssignableSlotAccessor& result) const final;

    std::string getDebugName() const final {
        return "_internalAddToSet";
    }

private:
    boost::optional<value::SlotId> _collatorSlot;
    value::SlotAccessor* _collatorAccessor = nullptr;

    int64_t _sizeCap;
};

namespace size_estimator {
inline size_t estimate(const std::unique_ptr<HashAggAccumulator>& accumulator) {
    return accumulator->estimateSize();
}
}  // namespace size_estimator
}  // namespace sbe
}  // namespace mongo
