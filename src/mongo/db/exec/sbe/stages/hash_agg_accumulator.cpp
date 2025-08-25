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

#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"

#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"  // countName, partialSumName
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <cstdint>

namespace mongo {
namespace sbe {
void CompiledHashAggAccumulator::prepare(CompileCtx& ctx,
                                         value::SlotAccessor* accumulatorAccessor) {
    if (_optionalInitializerExpr) {
        _optionalInitializerCode = _optionalInitializerExpr->compile(ctx);
    }

    // Temporarily update the compiler context to indicate that the target is an "agg
    // expression" program and to provide an accessor for the accumulator slot.
    ON_BLOCK_EXIT(
        [&ctx,
         savedIsAggExpression = std::exchange(ctx.aggExpression, true),
         savedAccumulatorAccessor = std::exchange(ctx.accumulator, accumulatorAccessor)]() {
            // Restore the compiler context when the 'prepare()' method exits.
            ctx.aggExpression = savedIsAggExpression;
            ctx.accumulator = savedAccumulatorAccessor;
        });
    _accumulatorCode = _accumulatorExpr->compile(ctx);
}

void CompiledHashAggAccumulator::prepareForMerge(CompileCtx& ctx,
                                                 value::SlotAccessor* accumulatorAccessor) {
    // Temporarily update the compiler context to indicate that the target is an "agg
    // expression" program and to provide an accessor for the accumulator slot.
    ON_BLOCK_EXIT(
        [&ctx,
         savedIsAggExpression = std::exchange(ctx.aggExpression, true),
         savedAccumulatorAccessor = std::exchange(ctx.accumulator, accumulatorAccessor)]() {
            // Restore the compiler context when the 'prepareForMerge()' method exits.
            ctx.aggExpression = savedIsAggExpression;
            ctx.accumulator = savedAccumulatorAccessor;
        });
    _mergingCode = _mergingExpr->compile(ctx);
}

void CompiledHashAggAccumulator::initialize(vm::ByteCode& bytecode,
                                            HashAggAccessor& accumulatorState) const {
    if (_optionalInitializerCode) {
        auto [owned, tag, val] = bytecode.run(_optionalInitializerCode.get());
        accumulatorState.reset(owned, tag, val);
    }
}

void CompiledHashAggAccumulator::merge(
    vm::ByteCode& bytecode, value::MaterializedSingleRowAccessor& accumulatorState) const {
    tassert(8186807, "Not prepared for merging partial aggregates", _mergingCode);

    auto [owned, tag, val] = bytecode.run(_mergingCode.get());
    accumulatorState.reset(owned, tag, val);
}

size_t CompiledHashAggAccumulator::estimateSize() const {
    return sizeof(*this) + size_estimator::estimate(_accumulatorExpr) +
        size_estimator::estimate(_mergingExpr) +
        (_optionalInitializerExpr ? size_estimator::estimate(_optionalInitializerExpr) : 0);
}

boost::optional<std::vector<DebugPrinter::Block>> CompiledHashAggAccumulator::debugPrintInitialize()
    const {
    if (!_optionalInitializerExpr) {
        return {};
    }

    std::vector<DebugPrinter::Block> debugOutput;
    DebugPrinter::addBlocks(debugOutput, _optionalInitializerExpr->debugPrint());
    return debugOutput;
}

std::vector<DebugPrinter::Block> CompiledHashAggAccumulator::debugPrintAccumulate() const {
    std::vector<DebugPrinter::Block> debugOutput;
    DebugPrinter::addBlocks(debugOutput, _accumulatorExpr->debugPrint());
    return debugOutput;
}

std::vector<DebugPrinter::Block> CompiledHashAggAccumulator::debugPrintMerge() const {
    std::vector<DebugPrinter::Block> debugOutput;
    DebugPrinter::addBlocks(debugOutput, _mergingExpr->debugPrint());
    return debugOutput;
}

void SinglePurposeHashAggAccumulator::prepare(CompileCtx& ctx,
                                              value::SlotAccessor* accumulatorAccessor) {
    _transformCode = _transformExpr->compile(ctx);

    singlePurposePrepare(ctx);
}

void SinglePurposeHashAggAccumulator::prepareForMerge(CompileCtx& ctx,
                                                      value::SlotAccessor* accumulatorAccessor) {
    _recoverSpilledStateExpr = makeE<EVariable>(_spillSlot);
    _recoverSpilledStateCode = _recoverSpilledStateExpr->compile(ctx);
}

void SinglePurposeHashAggAccumulator::accumulate(vm::ByteCode& bytecode,
                                                 HashAggAccessor& accumulatorState) const {
    auto [owned, tag, val] = bytecode.run(_transformCode.get());

    accumulateTransformedValue(owned, tag, val, accumulatorState);
}

void SinglePurposeHashAggAccumulator::finalize(
    vm::ByteCode&, value::AssignableSlotAccessor& accumulatorState) const {
    auto [tag, val] = accumulatorState.copyOrMoveValue();
    finalizePartialAggregate(tag, val, accumulatorState);
}

void SinglePurposeHashAggAccumulator::merge(
    vm::ByteCode& bytecode, value::MaterializedSingleRowAccessor& accumulatorState) const {
    tassert(8186802, "Not prepared for merging partial aggregates", _recoverSpilledStateCode);

    auto [ownedRecoveredState, tagRecoveredState, valRecoveredState] =
        bytecode.run(_recoverSpilledStateCode.get());
    mergeRecoveredState(
        ownedRecoveredState, tagRecoveredState, valRecoveredState, accumulatorState);
}

size_t SinglePurposeHashAggAccumulator::estimateSize() const {
    return sizeof(*this) + size_estimator::estimate(_transformExpr);
}

boost::optional<std::vector<DebugPrinter::Block>>
SinglePurposeHashAggAccumulator::debugPrintInitialize() const {
    return {};
}

std::vector<DebugPrinter::Block> SinglePurposeHashAggAccumulator::debugPrintAccumulate() const {
    std::vector<DebugPrinter::Block> debugOutput;

    DebugPrinter::addKeyword(debugOutput, getDebugName());
    debugOutput.emplace_back(DebugPrinter::Block("`::`"));
    DebugPrinter::addKeyword(debugOutput, "accumulate");
    debugOutput.emplace_back(DebugPrinter::Block("`(`"));
    DebugPrinter::addIdentifier(debugOutput, _outSlot);
    debugOutput.emplace_back(DebugPrinter::Block(",`"));
    DebugPrinter::addBlocks(debugOutput, _transformExpr->debugPrint());
    debugOutput.emplace_back(DebugPrinter::Block("`)"));

    return debugOutput;
}

std::vector<DebugPrinter::Block> SinglePurposeHashAggAccumulator::debugPrintMerge() const {
    std::vector<DebugPrinter::Block> debugOutput;

    DebugPrinter::addKeyword(debugOutput, getDebugName());
    debugOutput.emplace_back(DebugPrinter::Block("`::`"));
    DebugPrinter::addKeyword(debugOutput, "merge");
    debugOutput.emplace_back(DebugPrinter::Block("`(`"));
    DebugPrinter::addBlocks(debugOutput, _recoverSpilledStateExpr->debugPrint());
    debugOutput.emplace_back(DebugPrinter::Block("`)"));

    return debugOutput;
}

void ArithmeticAverageHashAggAccumulatorBase::initialize(vm::ByteCode& bytecode,
                                                         HashAggAccessor& accumulatorState) const {
    // Create the array for the sum of inputs. See enum AggSumValueElems for documentation of what
    // these fields mean. The kNonDecimalTotalSum and kNonDecimalTotalAddend must be NumberDouble
    // per tassert 5755312.
    auto [tagSum, valSum] = value::makeNewArray();
    value::Array* arraySum = reinterpret_cast<value::Array*>(valSum);
    arraySum->push_back(value::TypeTags::NumberInt32, 0);   // AggSumValueElems::kNonDecimalTotalTag
    arraySum->push_back(value::TypeTags::NumberDouble, 0);  // AggSumValueElems::kNonDecimalTotalSum
    arraySum->push_back(value::TypeTags::NumberDouble,
                        0);  // AggSumValueElems::kNonDecimalTotalAddend
    // AggSumValueElems::kDecimalTotal is added at runtime only if a decimal value is encountered.

    // Create the value for the count of inputs, which is just a scalar NumberInt64.
    value::TypeTags tagCount = value::TypeTags::NumberInt64;
    value::Value valCount = 0;

    // Create an array of [sum, count] states as the complete state value for new-style $avg.
    auto [tagState, valState] = value::makeNewArray();
    value::Array* arrayState = reinterpret_cast<value::Array*>(valState);
    arrayState->push_back(tagSum, valSum);
    arrayState->push_back(tagCount, valCount);

    // Set 'accState' to the initial state we just constructed.
    accumulatorState.reset(true /* owned */, tagState, valState);
}

void ArithmeticAverageHashAggAccumulatorBase::accumulateTransformedValue(
    bool ownedField,
    value::TypeTags tagField,
    value::Value valField,
    HashAggAccessor& accState) const {
    value::ValueGuard guardField(ownedField, tagField, valField);

    if (!value::isNumber(tagField)) {
        return;
    }

    auto [tagState, valState] = accState.getViewOfValue();
    value::Array* arrayState = reinterpret_cast<value::Array*>(valState);

    // Add the input value to the sum, which is a subarray at index position 0 of 'arrayState'
    // described by the header for enum AggSumValueElems.
    auto [tagSum, valSum] = arrayState->getAt(0);
    value::Array* arraySum = reinterpret_cast<value::Array*>(valSum);
    vm::ByteCode::aggDoubleDoubleSumImpl(arraySum, tagField, valField);  // updates 'arraySum'

    // Increment the count, which is in index position 1.
    auto [tagCount, valCount] = arrayState->getAt(1);
    uassert(8186801,
            "Expected count to have 64-bit integer type",
            tagCount == value::TypeTags::NumberInt64);
    valCount = value::bitcastFrom<int64_t>(value::bitcastTo<int64_t>(valCount) + 1);
    arrayState->setAt(1 /* idx */, tagCount, valCount);
}

void ArithmeticAverageHashAggAccumulatorBase::mergeRecoveredState(
    bool ownedRecoveredState,
    value::TypeTags tagRecoveredState,
    value::Value valRecoveredState,
    value::MaterializedSingleRowAccessor& accumulatorState) const {
    value::ValueGuard guard(ownedRecoveredState, tagRecoveredState, valRecoveredState);

    // The recovered state and the accumulator state should each be a 'value::Array' with two
    // elements: a sum and a count.
    tassert(8186803,
            "Expected recovered partial aggregate to have array type",
            tagRecoveredState == value::TypeTags::Array);
    value::Array* recoveredStateArray = value::getArrayView(valRecoveredState);

    auto [tagAccumulatedState, valAccumulatedState] = accumulatorState.getViewOfValue();
    tassert(8186804,
            "Expected partial aggregate to have array type",
            tagAccumulatedState == value::TypeTags::Array);
    value::Array* accumulatorStateArray = value::getArrayView(valAccumulatedState);

    // The sum elements of the recovered state and the accumulator state should each be a
    // 'value::Array' structured appropriately for the SBE VM's "double double sum" instructions.
    auto [tagRecoveredSum, valRecoveredSum] = recoveredStateArray->getAt(0);
    auto [tagAccumulatedSum, valAccumulatedSum] = accumulatorStateArray->getAt(0);
    tassert(8186805,
            "Expected partial sum to have array type",
            tagAccumulatedSum == value::TypeTags::Array);

    // Merge the partial sum from the recovered partial aggregate into the partial sum in the
    // running total aggregate.
    vm::ByteCode::aggMergeDoubleDoubleSumsImpl(value::getArrayView(valAccumulatedSum),
                                               tagRecoveredSum,
                                               valRecoveredSum);  // updates 'arraySum'

    // Add the counts from the partial aggregate and running total aggregate and store the result in
    // the running total aggregate.
    auto [tagRecoveredCount, valRecoveredCount] = recoveredStateArray->getAt(1);
    auto [tagAccumulatedCount, valAccumulatedCount] = accumulatorStateArray->getAt(1);
    tassert(8186806,
            "Expected partial aggregate counts to have 64-bit integer type",
            tagRecoveredCount == value::TypeTags::NumberInt64 &&
                tagAccumulatedCount == value::TypeTags::NumberInt64);
    auto mergedCount = value::bitcastTo<int64_t>(valAccumulatedCount) +
        value::bitcastTo<int64_t>(valRecoveredCount);
    accumulatorStateArray->setAt(
        1, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(mergedCount));
}  // SinglePurposeHashAggAccumulatorAvg::diskMergeFn

void ArithmeticAverageHashAggAccumulatorTerminal::finalizePartialAggregate(
    value::TypeTags tagPartialAggregate,
    value::Value valPartialAggregate,  // Owned
    value::AssignableSlotAccessor& result) const {
    value::ValueGuard guardPartialAggregate(tagPartialAggregate, valPartialAggregate);

    tassert(8186808,
            "Expected partial aggregate to have array type before finalization",
            tagPartialAggregate == value::TypeTags::Array);
    value::Array* partialAggregateArray = value::getArrayView(valPartialAggregate);

    auto [tagPartialSum, valPartialSum] = partialAggregateArray->getAt(0);
    tassert(8186809,
            "Expected partial sum to have array type",
            tagPartialSum == value::TypeTags::Array);
    value::Array* partialSum = value::getArrayView(valPartialSum);

    auto [tagCount, valCount] = partialAggregateArray->getAt(1);
    tassert(8186811,
            "Expected partial aggregate count to have 64-bit integer type",
            tagCount == value::TypeTags::NumberInt64);
    if (value::bitcastTo<int64_t>(valCount) == 0) {
        result.reset(true, value::TypeTags::Null, 0);
        return;
    }

    auto [ownedFinalSum, tagFinalSum, valFinalSum] =
        vm::ByteCode::aggDoubleDoubleSumFinalizeImpl(partialSum);
    value::ValueGuard finalSumGuard(ownedFinalSum, tagFinalSum, valFinalSum);

    auto [ownedAverage, tagAverage, valAverage] =
        vm::ByteCode::genericDiv(tagFinalSum, valFinalSum, tagCount, valCount);
    result.reset(ownedAverage, tagAverage, valAverage);
}

void ArithmeticAverageHashAggAccumulatorPartial::finalizePartialAggregate(
    value::TypeTags tagPartialAggregate,
    value::Value valPartialAggregate,  // Owned
    value::AssignableSlotAccessor& result) const {
    value::ValueGuard guardPartialAggregate(tagPartialAggregate, valPartialAggregate);

    auto [tagResultObject, valResultObject] = value::makeNewObject();
    value::ValueGuard guardResultObject(tagResultObject, valResultObject);

    tassert(8186813, "New object has unexpected type", tagResultObject == value::TypeTags::Object);
    value::Object* resultObject = value::getObjectView(valResultObject);
    resultObject->reserve(2);

    tassert(8186815,
            "Expected partial aggregate to have array type before finalization",
            tagPartialAggregate == value::TypeTags::Array);
    value::Array* partialAggregateArray = reinterpret_cast<value::Array*>(valPartialAggregate);

    auto [tagCount, valCount] = partialAggregateArray->getAt(1);
    resultObject->push_back(mongo::stage_builder::countName, tagCount, valCount);

    auto [tagPartialSum, valPartialSum] = partialAggregateArray->getAt(0);
    auto [ownedFinalizedSum, tagFinalizedSum, valFinalizedSum] =
        vm::ByteCode::builtinDoubleDoublePartialSumFinalizeImpl(tagPartialSum, valPartialSum);
    resultObject->push_back(mongo::stage_builder::partialSumName, tagFinalizedSum, valFinalizedSum);

    guardResultObject.reset();
    result.reset(true, tagResultObject, valResultObject);
}

void AddToSetHashAggAccumulator::singlePurposePrepare(CompileCtx& ctx) {
    _collatorAccessor = _collatorSlot ? ctx.getAccessor(*_collatorSlot) : nullptr;
}

void AddToSetHashAggAccumulator::initialize(vm::ByteCode& bytecode,
                                            HashAggAccessor& accumulatorState) const {
    accumulatorState.reset(false /* owned */, value::TypeTags::Nothing, 0);
}

void AddToSetHashAggAccumulator::accumulateTransformedValue(bool ownedField,
                                                            value::TypeTags tagField,
                                                            value::Value valField,
                                                            HashAggAccessor& accState) const {
    value::ValueGuard guardField(ownedField, tagField, valField);

    CollatorInterface* collator = nullptr;
    if (_collatorAccessor != nullptr) {
        auto [tagCollator, valCollator] = _collatorAccessor->getViewOfValue();
        tassert(10936805, "Collator has unexpected type", tagCollator == value::TypeTags::collator);
        collator = value::getCollatorView(valCollator);
    }

    auto [tagAccumulatorState, valAccumulatorState] = accState.copyOrMoveValue();
    guardField.reset();
    auto [ownedUpdatedState, tagUpdatedState, valUpdatedState] =
        vm::ByteCode::addToSetCappedImpl(tagAccumulatorState,
                                         valAccumulatorState,
                                         ownedField,
                                         tagField,
                                         valField,
                                         _sizeCap,
                                         collator);
    accState.reset(ownedUpdatedState, tagUpdatedState, valUpdatedState);
}

void AddToSetHashAggAccumulator::mergeRecoveredState(
    bool ownedRecoveredState,
    value::TypeTags tagRecoveredState,
    value::Value valRecoveredState,
    value::MaterializedSingleRowAccessor& accumulatorState) const {
    value::ValueGuard guardRecoveredState(
        ownedRecoveredState, tagRecoveredState, valRecoveredState);

    if (tagRecoveredState == value::TypeTags::Nothing) {
        // Leave 'accumulatorState' as is.
        return;
    }

    CollatorInterface* collator = nullptr;
    if (_collatorAccessor != nullptr) {
        auto [tagCollator, valCollator] = _collatorAccessor->getViewOfValue();
        tassert(10936806, "Collator has unexpected type", tagCollator == value::TypeTags::collator);
        collator = value::getCollatorView(valCollator);
    }

    tassert(10936808,
            "Expected $addToSet recovered state to be an array",
            tagRecoveredState == value::TypeTags::Array);
    auto recoveredState = value::getArrayView(valRecoveredState);

    // Note that ownership of both 'valNewSetMembers' and 'valAccumulatorState' passes to the
    // 'setUnionAccumImpl()' function.
    auto [tagNewSetMembers, valNewSetMembers] = [&]() {
        auto [tagNewSetMembers, valNewSetMembers] =
            recoveredState->getAt(static_cast<size_t>(vm::AggArrayWithSize::kValues));
        return value::copyValue(tagNewSetMembers, valNewSetMembers);
    }();
    auto [tagAccumulatorState, valAccumulatorState] = accumulatorState.copyOrMoveValue();

    auto [ownedResult, tagResult, valResult] = vm::ByteCode::setUnionAccumImpl(tagAccumulatorState,
                                                                               valAccumulatorState,
                                                                               tagNewSetMembers,
                                                                               valNewSetMembers,
                                                                               _sizeCap,
                                                                               collator);
    accumulatorState.reset(ownedResult, tagResult, valResult);
}

void AddToSetHashAggAccumulator::finalizePartialAggregate(
    value::TypeTags tagPartialAggregate,
    value::Value valPartialAggregate,  // Owned
    value::AssignableSlotAccessor& result) const {
    value::ValueGuard guardPartialAggregate(tagPartialAggregate, valPartialAggregate);

    if (tagPartialAggregate == value::TypeTags::Nothing) {
        result.reset(false, value::TypeTags::Nothing, 0);
        return;
    }

    tassert(10936807,
            "Expected partial aggregate to have array type before finalization",
            tagPartialAggregate == value::TypeTags::Array);
    // Move the 'kValues' ArraySet out of its parent 'partialAggregate' array, transfering ownership
    // of it from the 'partialAggregate' array to the calling function.
    auto [tagResult, valResult] =
        value::getArrayView(valPartialAggregate)
            ->swapAt(static_cast<size_t>(vm::AggArrayWithSize::kValues), value::TypeTags::Null, 0);

    result.reset(true, tagResult, valResult);
}
}  // namespace sbe
}  // namespace mongo
