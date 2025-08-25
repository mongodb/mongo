/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/sbe/vm/vm_datetime.h"
#include "mongo/db/query/collation/collator_interface.h"

namespace mongo {
namespace sbe {
namespace vm {
template <bool merging>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggDoubleDoubleSum(
    ArityType arity) {
    auto [_, fieldTag, fieldValue] = getFromStack(1);
    // Move the incoming accumulator state from the stack. Given that we are now the owner of the
    // state we are free to do any in-place update as we see fit.
    auto [accTag, accValue] = moveOwnedFromStack(0);

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        std::tie(accTag, accValue) = genericInitializeDoubleDoubleSumState();
    }

    value::ValueGuard guard{accTag, accValue};
    tassert(5755317, "The result slot must be Array-typed", accTag == value::TypeTags::Array);
    value::Array* accumulator = value::getArrayView(accValue);

    if constexpr (merging) {
        aggMergeDoubleDoubleSumsImpl(accumulator, fieldTag, fieldValue);
    } else {
        aggDoubleDoubleSumImpl(accumulator, fieldTag, fieldValue);
    }

    guard.reset();
    return {true, accTag, accValue};
}
template FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggDoubleDoubleSum<false>(
    ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggDoubleDoubleSum<true>(
    ArityType arity);

template <bool merging>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggStdDev(ArityType arity) {
    auto [_, fieldTag, fieldValue] = getFromStack(1);
    // Move the incoming accumulator state from the stack. Given that we are now the owner of the
    // state we are free to do any in-place update as we see fit.
    auto [accTag, accValue] = moveOwnedFromStack(0);

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        std::tie(accTag, accValue) = value::makeNewArray();
        value::ValueGuard newArrGuard{accTag, accValue};
        auto arr = value::getArrayView(accValue);
        arr->reserve(AggStdDevValueElems::kSizeOfArray);

        // The order of the following three elements should match to 'AggStdDevValueElems'.
        arr->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
        arr->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0));
        arr->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0));
        newArrGuard.reset();
    }

    value::ValueGuard guard{accTag, accValue};
    tassert(5755210, "The result slot must be Array-typed", accTag == value::TypeTags::Array);
    auto accumulator = value::getArrayView(accValue);

    if constexpr (merging) {
        aggMergeStdDevsImpl(accumulator, fieldTag, fieldValue);
    } else {
        aggStdDevImpl(accumulator, fieldTag, fieldValue);
    }

    guard.reset();
    return {true, accTag, accValue};
}
template FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggStdDev<true>(
    ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggStdDev<false>(
    ArityType arity);

FastTuple<bool, value::TypeTags, value::Value> ByteCode::concatArraysAccumImpl(
    value::TypeTags newElemTag,
    value::Value newElemVal,
    int32_t sizeCap,
    bool arrOwned,
    value::TypeTags arrTag,
    value::Value arrVal,
    int64_t sizeOfNewElems) {
    // Create a new array to hold size and added elements, if it does not exist yet.
    if (arrTag == value::TypeTags::Nothing) {
        arrOwned = true;
        std::tie(arrTag, arrVal) = value::makeNewArray();
        auto arr = value::getArrayView(arrVal);

        auto [accArrTag, accArrVal] = value::makeNewArray();

        // The order is important! The accumulated array should be at index
        // AggArrayWithSize::kValues, and the size should be at index
        // AggArrayWithSize::kSizeOfValues.
        arr->push_back(accArrTag, accArrVal);
        arr->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }

    // If the field resolves to Nothing (e.g. if it is missing in the document), then we want to
    // leave the accumulator as is.
    if (newElemTag == value::TypeTags::Nothing) {
        return {arrOwned, arrTag, arrVal};
    }

    tassert(7039513, "expected array to be owned", arrOwned);
    value::ValueGuard accumulatorGuard{arrTag, arrVal};

    // We expect the field to be an array. Thus, we return Nothing on an unexpected input type.
    if (!value::isArray(newElemTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    tassert(7039514, "expected accumulator to have type 'Array'", arrTag == value::TypeTags::Array);
    auto arr = value::getArrayView(arrVal);
    tassert(7039515,
            "accumulator was array of unexpected size",
            arr->size() == static_cast<size_t>(AggArrayWithSize::kLast));

    auto newArr = value::getArrayView(newElemVal);

    // Check that the accumulated size after concatentation won't exceed the limit.
    {
        auto [accSizeTag, accSizeVal] =
            arr->getAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues));

        tassert(7039516, "expected 64-bit int", accSizeTag == value::TypeTags::NumberInt64);
        const int64_t currentSize = value::bitcastTo<int64_t>(accSizeVal);
        const int64_t totalSize = currentSize + sizeOfNewElems;

        if (totalSize >= static_cast<int64_t>(sizeCap)) {
            uasserted(ErrorCodes::ExceededMemoryLimit,
                      str::stream()
                          << "Used too much memory for a single array. Memory limit: " << sizeCap
                          << ". Concatentating array of " << arr->size() << " elements and "
                          << currentSize << " bytes with array of " << newArr->size()
                          << " elements and " << sizeOfNewElems << " bytes.");
        }

        // We are still under the size limit. Set the new total size in the accumulator.
        arr->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
                   value::TypeTags::NumberInt64,
                   value::bitcastFrom<int64_t>(totalSize));
    }

    auto [accArrTag, accArrVal] = arr->getAt(static_cast<size_t>(AggArrayWithSize::kValues));
    tassert(7039518, "expected value of type 'Array'", accArrTag == value::TypeTags::Array);
    auto accArr = value::getArrayView(accArrVal);

    value::arrayForEach<true>(
        newElemTag, newElemVal, [&](value::TypeTags elTag, value::Value elVal) {
            accArr->push_back(elTag, elVal);
        });


    accumulatorGuard.reset();
    return {arrOwned, arrTag, arrVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggConcatArraysCapped(
    ArityType arity) {
    auto [arrOwned, arrTag, arrVal] = getFromStack(0);
    auto [newElemTag, newElemVal] = moveOwnedFromStack(1);

    // Note that we do not call 'reset()' on the guard below, as 'concatArraysAccumImpl' assumes
    // that callers will manage the memory associated with 'tag/valNewElem'. See the comment on
    // 'concatArraysAccumImpl' for more details.
    value::ValueGuard newElemGuard{newElemTag, newElemVal};

    auto [_, sizeCapTag, sizeCapVal] = getFromStack(2);
    tassert(7039508,
            "'cap' parameter must be a 32-bit int",
            sizeCapTag == value::TypeTags::NumberInt32);
    const int32_t sizeCap = value::bitcastTo<int32_t>(sizeCapVal);

    // We expect the new value we are adding to the accumulator to be a two-element array where
    // the first element is the array to concatenate and the second value is the corresponding size.
    tassert(7039512, "expected value of type 'Array'", newElemTag == value::TypeTags::Array);
    auto newArr = value::getArrayView(newElemVal);
    tassert(7039527,
            "array had unexpected size",
            newArr->size() == static_cast<size_t>(AggArrayWithSize::kLast));

    auto [newArrayTag, newArrayVal] = newArr->getAt(static_cast<size_t>(AggArrayWithSize::kValues));
    tassert(7039519, "expected value of type 'Array'", newArrayTag == value::TypeTags::Array);

    auto [newSizeTag, newSizeVal] =
        newArr->getAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues));
    tassert(7039517, "expected 64-bit int", newSizeTag == value::TypeTags::NumberInt64);

    return concatArraysAccumImpl(newArrayTag,
                                 newArrayVal,
                                 sizeCap,
                                 arrOwned,
                                 arrTag,
                                 arrVal,
                                 value::bitcastTo<int64_t>(newSizeVal));
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggSetUnion(ArityType arity) {
    auto [ownAcc, tagAcc, valAcc] = getFromStack(0);

    if (tagAcc == value::TypeTags::Nothing) {
        // Initialize the accumulator.
        ownAcc = true;
        std::tie(tagAcc, valAcc) = value::makeNewArraySet();
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }

    tassert(7039552, "accumulator must be owned", ownAcc);
    value::ValueGuard guardAcc{tagAcc, valAcc};
    tassert(7039553, "accumulator must be of type ArraySet", tagAcc == value::TypeTags::ArraySet);
    auto acc = value::getArraySetView(valAcc);

    auto [tagNewSet, valNewSet] = moveOwnedFromStack(1);
    value::ValueGuard guardNewSet{tagNewSet, valNewSet};
    if (!value::isArray(tagNewSet)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    value::arrayForEach(tagNewSet, valNewSet, [&](value::TypeTags elTag, value::Value elVal) {
        auto [copyTag, copyVal] = value::copyValue(elTag, elVal);
        acc->push_back(copyTag, copyVal);
    });

    guardAcc.reset();
    return {ownAcc, tagAcc, valAcc};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggCollSetUnion(ArityType arity) {
    auto [ownAcc, tagAcc, valAcc] = getFromStack(0);

    if (tagAcc == value::TypeTags::Nothing) {
        auto [_, collatorTag, collatorVal] = getFromStack(1);
        tassert(
            7690402, "Expected value of type 'collator'", collatorTag == value::TypeTags::collator);
        CollatorInterface* collator = value::getCollatorView(collatorVal);

        // Initialize the accumulator.
        ownAcc = true;
        std::tie(tagAcc, valAcc) = value::makeNewArraySet(collator);
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }

    tassert(7690403, "Accumulator must be owned", ownAcc);
    value::ValueGuard guardAcc{tagAcc, valAcc};
    tassert(7690404, "Accumulator must be of type ArraySet", tagAcc == value::TypeTags::ArraySet);
    auto acc = value::getArraySetView(valAcc);

    auto [tagNewSet, valNewSet] = moveOwnedFromStack(2);
    value::ValueGuard guardNewSet{tagNewSet, valNewSet};
    if (!value::isArray(tagNewSet)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    value::arrayForEach(tagNewSet, valNewSet, [&](value::TypeTags elTag, value::Value elVal) {
        auto [copyTag, copyVal] = value::copyValue(elTag, elVal);
        acc->push_back(copyTag, copyVal);
    });

    guardAcc.reset();
    return {ownAcc, tagAcc, valAcc};
}

namespace {
FastTuple<bool, value::TypeTags, value::Value> builtinAggSetUnionCappedImpl(
    value::TypeTags tagLhsAccumulatorState,
    value::Value valLhsAccumulatorState,  // Owned
    value::TypeTags tagRhsAccumulatorState,
    value::Value valRhsAccumulatorState,  // Owned
    int32_t sizeCap,
    CollatorInterface* collator) {
    value::ValueGuard guardLhsAccumulatorState{tagLhsAccumulatorState, valLhsAccumulatorState};
    value::ValueGuard guardRhsAccumulatorState{tagRhsAccumulatorState, valRhsAccumulatorState};

    tassert(7039526,
            "Expected array for capped set union operand",
            tagRhsAccumulatorState == value::TypeTags::Array);

    auto rhsAccumulatorState = value::getArrayView(valRhsAccumulatorState);
    tassert(7039528,
            "Capped set union operand with invalid length",
            rhsAccumulatorState->size() == static_cast<size_t>(AggArrayWithSize::kLast));

    auto [tagNewSetMembers, valNewSetMembers] = rhsAccumulatorState->swapAt(
        static_cast<size_t>(AggArrayWithSize::kValues), value::TypeTags::Null, 0);
    value::ValueGuard guardNewSetMembers{tagNewSetMembers, valNewSetMembers};
    tassert(7039525,
            "Expected ArraySet in capped set union operand",
            tagNewSetMembers == value::TypeTags::ArraySet);

    guardLhsAccumulatorState.reset();
    guardNewSetMembers.reset();
    return ByteCode::setUnionAccumImpl(tagLhsAccumulatorState,
                                       valLhsAccumulatorState,
                                       tagNewSetMembers,
                                       valNewSetMembers,
                                       sizeCap,
                                       collator);
}
}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggSetUnionCapped(ArityType arity) {
    auto [tagLhsAccumulatorState, valLhsAccumulatorState] = moveOwnedFromStack(0);
    value::ValueGuard guardLhsAccumulatorState{tagLhsAccumulatorState, valLhsAccumulatorState};

    auto [tagRhsAccumulatorState, valRhsAccumulatorState] = moveOwnedFromStack(1);
    value::ValueGuard guardRhsAccumulatorState{tagRhsAccumulatorState, valRhsAccumulatorState};

    auto [_, tagSizeCap, valSizeCap] = getFromStack(2);
    tassert(7039509,
            "'cap' parameter must be a 32-bit int",
            tagSizeCap == value::TypeTags::NumberInt32);

    guardLhsAccumulatorState.reset();
    guardRhsAccumulatorState.reset();
    return builtinAggSetUnionCappedImpl(tagLhsAccumulatorState,
                                        valLhsAccumulatorState,
                                        tagRhsAccumulatorState,
                                        valRhsAccumulatorState,
                                        value::bitcastTo<int32_t>(valSizeCap),
                                        nullptr /*collator*/);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggCollSetUnionCapped(
    ArityType arity) {
    auto [tagLhsAccumulatorState, valLhsAccumulatorState] = moveOwnedFromStack(0);
    value::ValueGuard guardLhsAccumulatorState{tagLhsAccumulatorState, valLhsAccumulatorState};

    auto [_1, tagColl, valColl] = getFromStack(1);
    tassert(7039510, "expected value of type 'collator'", tagColl == value::TypeTags::collator);

    auto [tagRhsAccumulatorState, valRhsAccumulatorState] = moveOwnedFromStack(2);
    value::ValueGuard guardRhsAccumulatorState{tagRhsAccumulatorState, valRhsAccumulatorState};

    auto [_2, tagSizeCap, valSizeCap] = getFromStack(3);
    tassert(7039511,
            "'cap' parameter must be a 32-bit int",
            tagSizeCap == value::TypeTags::NumberInt32);

    guardLhsAccumulatorState.reset();
    guardRhsAccumulatorState.reset();
    return builtinAggSetUnionCappedImpl(tagLhsAccumulatorState,
                                        valLhsAccumulatorState,
                                        tagRhsAccumulatorState,
                                        valRhsAccumulatorState,
                                        value::bitcastTo<int32_t>(valSizeCap),
                                        value::getCollatorView(valColl));
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggFirstNNeedsMoreInput(
    ArityType arity) {
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);
    uassert(7695200, "Unexpected accumulator state ownership", !stateOwned);

    auto state = value::getArrayView(stateVal);
    uassert(
        7695201, "The accumulator state should be an array", stateTag == value::TypeTags::Array);

    auto [arrayTag, arrayVal] = state->getAt(static_cast<size_t>(AggMultiElems::kInternalArr));
    uassert(7695202,
            "Internal array component is not of correct type",
            arrayTag == value::TypeTags::Array);
    auto array = value::getArrayView(arrayVal);

    auto [maxSizeTag, maxSize] = state->getAt(static_cast<size_t>(AggMultiElems::kMaxSize));
    uassert(7695203,
            "MaxSize component should be a 64-bit integer",
            maxSizeTag == value::TypeTags::NumberInt64);

    bool needMoreInput = (array->size() < maxSize);
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(needMoreInput)};
}

namespace {
int32_t aggFirstN(value::Array* state,
                  value::Array* array,
                  size_t maxSize,
                  int32_t memUsage,
                  int32_t memLimit,
                  value::TypeTags fieldTag,
                  value::Value fieldVal) {
    value::ValueGuard fieldGuard{fieldTag, fieldVal};
    if (array->size() < maxSize) {
        memUsage = updateAndCheckMemUsage(
            state, memUsage, value::getApproximateSize(fieldTag, fieldVal), memLimit);

        // add to array
        fieldGuard.reset();
        array->push_back(fieldTag, fieldVal);
    }
    return memUsage;
}
}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggFirstN(ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTag, stateVal);

    auto [fieldTag, fieldVal] = moveOwnedFromStack(1);
    aggFirstN(state, array, maxSize, memUsage, memLimit, fieldTag, fieldVal);

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggFirstNMerge(ArityType arity) {
    auto [mergeStateTag, mergeStateVal] = moveOwnedFromStack(0);
    value::ValueGuard mergeStateGuard{mergeStateTag, mergeStateVal};

    auto [stateTag, stateVal] = moveOwnedFromStack(1);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [mergeState,
          mergeArray,
          mergeStartIdx,
          mergeMaxSize,
          mergeMemUsage,
          mergeMemLimit,
          mergeIsGroupAccum] = getMultiAccState(mergeStateTag, mergeStateVal);
    auto [state, array, accStartIdx, accMaxSize, accMemUsage, accMemLimit, accIsGroupAccum] =
        getMultiAccState(stateTag, stateVal);
    uassert(7548604,
            "Two arrays to merge should have the same MaxSize component",
            accMaxSize == mergeMaxSize);

    for (size_t i = 0; i < array->size(); ++i) {
        if (mergeArray->size() == mergeMaxSize) {
            break;
        }

        auto [tag, val] = array->swapAt(i, value::TypeTags::Null, 0);
        mergeMemUsage =
            aggFirstN(mergeState, mergeArray, mergeMaxSize, mergeMemUsage, mergeMemLimit, tag, val);
    }

    mergeStateGuard.reset();
    return {true, mergeStateTag, mergeStateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggFirstNFinalize(ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard guard{stateTag, stateVal};

    uassert(7548605, "expected an array", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);

    auto [isGroupAccTag, isGroupAccVal] =
        state->getAt(static_cast<size_t>(AggMultiElems::kIsGroupAccum));
    auto isGroupAcc = value::bitcastTo<bool>(isGroupAccVal);

    if (isGroupAcc) {
        auto [outputTag, outputVal] = state->swapAt(
            static_cast<size_t>(AggMultiElems::kInternalArr), value::TypeTags::Null, 0);
        return {true, outputTag, outputVal};
    } else {
        auto [arrTag, arrVal] = state->getAt(static_cast<size_t>(AggMultiElems::kInternalArr));
        auto [outputTag, outputVal] = value::copyValue(arrTag, arrVal);
        return {true, outputTag, outputVal};
    }
}

namespace {
size_t updateStartIdx(value::Array* state, size_t startIdx, size_t arrSize) {
    startIdx = (startIdx + 1) % arrSize;
    state->setAt(static_cast<size_t>(AggMultiElems::kStartIdx),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<size_t>(startIdx));
    return startIdx;
}

std::pair<size_t, int32_t> aggLastN(value::Array* state,
                                    value::Array* array,
                                    size_t startIdx,
                                    size_t maxSize,
                                    int32_t memUsage,
                                    int32_t memLimit,
                                    value::TypeTags fieldTag,
                                    value::Value fieldVal) {
    value::ValueGuard guard{fieldTag, fieldVal};
    if (array->size() < maxSize) {
        invariant(startIdx == 0);
        guard.reset();
        array->push_back(fieldTag, fieldVal);
    } else {
        invariant(array->size() == maxSize);
        guard.reset();
        auto [oldFieldTag, oldFieldVal] = array->swapAt(startIdx, fieldTag, fieldVal);
        memUsage -= value::getApproximateSize(oldFieldTag, oldFieldVal);
        value::releaseValue(oldFieldTag, oldFieldVal);
        startIdx = updateStartIdx(state, startIdx, maxSize);
    }
    memUsage = updateAndCheckMemUsage(
        state, memUsage, value::getApproximateSize(fieldTag, fieldVal), memLimit);
    return {startIdx, memUsage};
}
}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggLastN(ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTag, stateVal);

    auto [fieldTag, fieldVal] = moveOwnedFromStack(1);
    aggLastN(state, array, startIdx, maxSize, memUsage, memLimit, fieldTag, fieldVal);

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggLastNMerge(ArityType arity) {
    auto [mergeStateTag, mergeStateVal] = moveOwnedFromStack(0);
    value::ValueGuard mergeStateGuard{mergeStateTag, mergeStateVal};

    auto [stateTag, stateVal] = moveOwnedFromStack(1);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [mergeState,
          mergeArray,
          mergeStartIdx,
          mergeMaxSize,
          mergeMemUsage,
          mergeMemLimit,
          mergeIsGroupAccum] = getMultiAccState(mergeStateTag, mergeStateVal);
    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTag, stateVal);
    uassert(7548703,
            "Two arrays to merge should have the same MaxSize component",
            maxSize == mergeMaxSize);

    if (array->size() < maxSize) {
        // add values from accArr to mergeArray
        for (size_t i = 0; i < array->size(); ++i) {
            auto [tag, val] = array->swapAt(i, value::TypeTags::Null, 0);
            std::tie(mergeStartIdx, mergeMemUsage) = aggLastN(mergeState,
                                                              mergeArray,
                                                              mergeStartIdx,
                                                              mergeMaxSize,
                                                              mergeMemUsage,
                                                              mergeMemLimit,
                                                              tag,
                                                              val);
        }
        mergeStateGuard.reset();
        return {true, mergeStateTag, mergeStateVal};
    } else {
        // return accArray since it contains last n values
        invariant(array->size() == maxSize);
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggLastNFinalize(ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard guard{stateTag, stateVal};

    auto [state, arr, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTag, stateVal);
    if (startIdx == 0) {
        if (isGroupAccum) {
            auto [outTag, outVal] = state->swapAt(0, value::TypeTags::Null, 0);
            return {true, outTag, outVal};
        } else {
            auto [arrTag, arrVal] = state->getAt(0);
            auto [outTag, outVal] = value::copyValue(arrTag, arrVal);
            return {true, outTag, outVal};
        }
    }

    invariant(arr->size() == maxSize);
    auto [outArrayTag, outArrayVal] = value::makeNewArray();
    auto outArray = value::getArrayView(outArrayVal);
    outArray->reserve(maxSize);

    if (isGroupAccum) {
        for (size_t i = 0; i < maxSize; ++i) {
            auto srcIdx = (i + startIdx) % maxSize;
            auto [elemTag, elemVal] = arr->swapAt(srcIdx, value::TypeTags::Null, 0);
            outArray->push_back(elemTag, elemVal);
        }
    } else {
        for (size_t i = 0; i < maxSize; ++i) {
            auto srcIdx = (i + startIdx) % maxSize;
            auto [elemTag, elemVal] = arr->getAt(srcIdx);
            auto [copyTag, copyVal] = value::copyValue(elemTag, elemVal);
            outArray->push_back(copyTag, copyVal);
        }
    }
    return {true, outArrayTag, outArrayVal};
}

class ByteCode::TopBottomArgsDirect final : public ByteCode::TopBottomArgs {
public:
    TopBottomArgsDirect(TopBottomSense sense,
                        SortSpec* sortSpec,
                        FastTuple<bool, value::TypeTags, value::Value> key,
                        FastTuple<bool, value::TypeTags, value::Value> value)
        : TopBottomArgs(sense, sortSpec, false, false) {
        setDirectKeyArg(key);
        setDirectValueArg(value);
    }

    ~TopBottomArgsDirect() final = default;

    bool keySortsBeforeImpl(std::pair<value::TypeTags, value::Value> item) final {
        MONGO_UNREACHABLE_TASSERT(8448721);
    }
    std::pair<value::TypeTags, value::Value> getOwnedKeyImpl() final {
        MONGO_UNREACHABLE_TASSERT(8448722);
    }
    std::pair<value::TypeTags, value::Value> getOwnedValueImpl() final {
        MONGO_UNREACHABLE_TASSERT(8448723);
    }
};

class ByteCode::TopBottomArgsFromStack final : public ByteCode::TopBottomArgs {
public:
    TopBottomArgsFromStack(TopBottomSense sense,
                           SortSpec* sortSpec,
                           bool decomposedKey,
                           bool decomposedValue,
                           ByteCode* bytecode,
                           size_t keysStartOffset,
                           size_t numKeys,
                           size_t valuesStartOffset,
                           size_t numValues)
        : TopBottomArgs(sense, sortSpec, decomposedKey, decomposedValue),
          _bytecode(bytecode),
          _keysStartOffset(keysStartOffset),
          _numKeys(numKeys),
          _valuesStartOffset(valuesStartOffset),
          _numValues(numValues) {
        if (!_decomposedKey) {
            setDirectKeyArg(_bytecode->moveFromStack(_keysStartOffset));
        }
        if (!_decomposedValue) {
            setDirectValueArg(_bytecode->moveFromStack(_valuesStartOffset));
        }
    }

    ~TopBottomArgsFromStack() final = default;

protected:
    bool keySortsBeforeImpl(std::pair<value::TypeTags, value::Value> item) final {
        tassert(8448700, "Expected item to be an Array", item.first == value::TypeTags::Array);

        const SortPattern& sortPattern = _sortSpec->getSortPattern();
        tassert(8448701,
                "Expected numKeys to be equal to number of sort pattern parts",
                _numKeys == sortPattern.size());

        auto itemArray = value::getArrayView(item.second);
        tassert(8448702,
                "Expected size of item array to be equal to number of sort pattern parts",
                sortPattern.size() == itemArray->size());

        if (_sense == TopBottomSense::kTop) {
            for (size_t i = 0; i < sortPattern.size(); i++) {
                auto [_, keyTag, keyVal] = _bytecode->getFromStack(_keysStartOffset + i);
                auto [itemTag, itemVal] = itemArray->getAt(i);
                int32_t cmp = compare<TopBottomSense::kTop>(keyTag, keyVal, itemTag, itemVal);

                if (cmp != 0) {
                    return sortPattern[i].isAscending ? cmp < 0 : cmp > 0;
                }
            }
        } else {
            for (size_t i = 0; i < sortPattern.size(); i++) {
                auto [_, keyTag, keyVal] = _bytecode->getFromStack(_keysStartOffset + i);
                auto [itemTag, itemVal] = itemArray->getAt(i);
                int32_t cmp = compare<TopBottomSense::kBottom>(keyTag, keyVal, itemTag, itemVal);

                if (cmp != 0) {
                    return sortPattern[i].isAscending ? cmp < 0 : cmp > 0;
                }
            }
        }

        return false;
    }

    std::pair<value::TypeTags, value::Value> getOwnedKeyImpl() final {
        auto [keysArrTag, keysArrVal] = value::makeNewArray();
        value::ValueGuard keysArrGuard{keysArrTag, keysArrVal};
        auto keysArr = value::getArrayView(keysArrVal);

        for (size_t i = 0; i < _numKeys; ++i) {
            auto [keyTag, keyVal] = _bytecode->moveOwnedFromStack(_keysStartOffset + i);
            keysArr->push_back(keyTag, keyVal);
        }

        keysArrGuard.reset();
        return std::pair{keysArrTag, keysArrVal};
    }

    std::pair<value::TypeTags, value::Value> getOwnedValueImpl() final {
        auto [valuesArrTag, valuesArrVal] = value::makeNewArray();
        value::ValueGuard valuesArrGuard{valuesArrTag, valuesArrVal};
        auto valuesArr = value::getArrayView(valuesArrVal);

        for (size_t i = 0; i < _numValues; ++i) {
            auto [valueTag, valueVal] = _bytecode->moveOwnedFromStack(_valuesStartOffset + i);
            valuesArr->push_back(valueTag, valueVal);
        }

        valuesArrGuard.reset();
        return std::pair{valuesArrTag, valuesArrVal};
    }

private:
    ByteCode* _bytecode;
    size_t _keysStartOffset;
    size_t _numKeys;
    size_t _valuesStartOffset;
    size_t _numValues;
};  // class ByteCode::TopBottomArgsFromStack

template <TopBottomSense Sense, bool ValueIsDecomposedArray>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggTopBottomNImpl(ArityType arity) {
    using Less =
        std::conditional_t<Sense == TopBottomSense::kTop, SortPatternLess, SortPatternGreater>;

    auto [sortSpecOwned, sortSpecTag, sortSpecVal] = getFromStack(1);
    tassert(8448703, "Argument must be of sortSpec type", sortSpecTag == value::TypeTags::sortSpec);
    auto ss = value::getSortSpecView(sortSpecVal);

    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTag, stateVal);

    size_t numKeys = 1;
    bool keyIsDecomposed = false;
    auto [_, numKeysTag, numKeysVal] = getFromStack(2);
    if (numKeysTag == value::TypeTags::NumberInt32) {
        numKeys = static_cast<size_t>(value::bitcastTo<int32_t>(numKeysVal));
        keyIsDecomposed = true;
    } else {
        tassert(
            8448704, "Expected numKeys to be Null or Int32", numKeysTag == value::TypeTags::Null);
    }

    constexpr size_t keysStartOffset = 3;
    const size_t valuesStartOffset = keysStartOffset + numKeys;
    const size_t numValues = ValueIsDecomposedArray ? arity - valuesStartOffset : 1;

    if (!keyIsDecomposed && !ValueIsDecomposedArray) {
        auto key = moveFromStack(keysStartOffset);
        auto value = moveFromStack(valuesStartOffset);

        TopBottomArgsDirect topBottomArgs{Sense, ss, key, value};

        aggTopBottomNAdd<Sense>(state, array, maxSize, memUsage, memLimit, topBottomArgs);
    } else {
        TopBottomArgsFromStack topBottomArgs{Sense,
                                             ss,
                                             keyIsDecomposed,
                                             ValueIsDecomposedArray,
                                             this,
                                             keysStartOffset,
                                             numKeys,
                                             valuesStartOffset,
                                             numValues};

        aggTopBottomNAdd<Sense>(state, array, maxSize, memUsage, memLimit, topBottomArgs);
    }

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

template <TopBottomSense Sense>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggTopBottomN(ArityType arity) {
    return builtinAggTopBottomNImpl<Sense, false>(arity);
}
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggTopBottomN<(TopBottomSense)0>(ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggTopBottomN<(TopBottomSense)1>(ArityType arity);

template <TopBottomSense Sense>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggTopBottomNArray(
    ArityType arity) {
    return builtinAggTopBottomNImpl<Sense, true>(arity);
}
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggTopBottomNArray<(TopBottomSense)0>(ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggTopBottomNArray<(TopBottomSense)1>(ArityType arity);

template <TopBottomSense Sense>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggTopBottomNMerge(
    ArityType arity) {
    using OwnedTagValTuple = FastTuple<bool, value::TypeTags, value::Value>;

    auto [sortSpecOwned, sortSpecTag, sortSpecVal] = getFromStack(2);
    tassert(5807025, "Argument must be of sortSpec type", sortSpecTag == value::TypeTags::sortSpec);
    auto sortSpec = value::getSortSpecView(sortSpecVal);

    auto [stateTag, stateVal] = moveOwnedFromStack(1);
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto [mergeStateTag, mergeStateVal] = moveOwnedFromStack(0);
    value::ValueGuard mergeStateGuard{mergeStateTag, mergeStateVal};
    auto [mergeState,
          mergeArray,
          mergeStartIx,
          mergeMaxSize,
          mergeMemUsage,
          mergeMemLimit,
          mergeIsGroupAccum] = getMultiAccState(mergeStateTag, mergeStateVal);
    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTag, stateVal);
    tassert(5807008,
            "Two arrays to merge should have the same MaxSize component",
            maxSize == mergeMaxSize);

    for (auto [pairTag, pairVal] : array->values()) {
        auto pair = value::getArrayView(pairVal);
        auto key = pair->swapAt(0, value::TypeTags::Null, 0);
        auto value = pair->swapAt(1, value::TypeTags::Null, 0);

        TopBottomArgsDirect topBottomArgs{
            Sense, sortSpec, {true, key.first, key.second}, {true, value.first, value.second}};

        mergeMemUsage = aggTopBottomNAdd<Sense>(
            mergeState, mergeArray, mergeMaxSize, mergeMemUsage, mergeMemLimit, topBottomArgs);
    }

    mergeStateGuard.reset();
    return {true, mergeStateTag, mergeStateVal};
}
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggTopBottomNMerge<(TopBottomSense)0>(ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggTopBottomNMerge<(TopBottomSense)1>(ArityType arity);

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggTopBottomNFinalize(
    ArityType arity) {
    auto [sortSpecOwned, sortSpecTag, sortSpecVal] = getFromStack(1);
    tassert(5807026, "Argument must be of sortSpec type", sortSpecTag == value::TypeTags::sortSpec);
    auto sortSpec = value::getSortSpecView(sortSpecVal);

    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTag, stateVal);

    auto [outputArrayTag, outputArrayVal] = value::makeNewArray();
    value::ValueGuard outputArrayGuard{outputArrayTag, outputArrayVal};
    auto outputArray = value::getArrayView(outputArrayVal);
    outputArray->reserve(array->size());

    // We always output result in the order of sort pattern in according to MQL semantics.
    auto less = SortPatternLess(sortSpec);
    auto keyLess = PairKeyComp(less);
    std::sort(array->values().begin(), array->values().end(), keyLess);
    for (size_t i = 0; i < array->size(); ++i) {
        auto pair = value::getArrayView(array->getAt(i).second);
        if (isGroupAccum) {
            auto [outTag, outVal] = pair->swapAt(1, value::TypeTags::Null, 0);
            outputArray->push_back(outTag, outVal);
        } else {
            auto [outTag, outVal] = pair->getAt(1);
            auto [copyTag, copyVal] = value::copyValue(outTag, outVal);
            outputArray->push_back(copyTag, copyVal);
        }
    }

    outputArrayGuard.reset();
    return {true, outputArrayTag, outputArrayVal};
}

namespace {
template <AccumulatorMinMaxN::MinMaxSense S>
int32_t aggMinMaxN(value::Array* state,
                   value::Array* array,
                   size_t maxSize,
                   int32_t memUsage,
                   int32_t memLimit,
                   const CollatorInterface* collator,
                   value::TypeTags fieldTag,
                   value::Value fieldVal) {
    value::ValueGuard guard{fieldTag, fieldVal};
    auto& heap = array->values();

    constexpr auto less = []() -> bool {
        if constexpr (S == AccumulatorMinMaxN::MinMaxSense::kMax) {
            return false;
        }
        return true;
    }();
    value::ValueCompare<less> comp{collator};

    if (array->size() < maxSize) {
        memUsage = updateAndCheckMemUsage(
            state, memUsage, value::getApproximateSize(fieldTag, fieldVal), memLimit);
        guard.reset();

        array->push_back(fieldTag, fieldVal);
        std::push_heap(heap.begin(), heap.end(), comp);
    } else {
        uassert(7548800,
                "Heap should contain same number of elements as MaxSize",
                array->size() == maxSize);

        auto heapRoot = heap.front();
        if (comp({fieldTag, fieldVal}, heapRoot)) {
            memUsage =
                updateAndCheckMemUsage(state,
                                       memUsage,
                                       -value::getApproximateSize(heapRoot.first, heapRoot.second) +
                                           value::getApproximateSize(fieldTag, fieldVal),
                                       memLimit);
            std::pop_heap(heap.begin(), heap.end(), comp);
            guard.reset();
            array->setAt(maxSize - 1, fieldTag, fieldVal);
            std::push_heap(heap.begin(), heap.end(), comp);
        }
    }

    return memUsage;
}
}  // namespace

template <AccumulatorMinMaxN::MinMaxSense S>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggMinMaxN(ArityType arity) {
    invariant(arity == 2 || arity == 3);

    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [fieldTag, fieldVal] = moveOwnedFromStack(1);
    value::ValueGuard fieldGuard{fieldTag, fieldVal};
    if (value::isNullish(fieldTag)) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTag, stateVal);

    CollatorInterface* collator = nullptr;
    if (arity == 3) {
        auto [collOwned, collTag, collVal] = getFromStack(2);
        uassert(7548802, "expected a collator argument", collTag == value::TypeTags::collator);
        collator = value::getCollatorView(collVal);
    }
    fieldGuard.reset();
    aggMinMaxN<S>(state, array, maxSize, memUsage, memLimit, collator, fieldTag, fieldVal);

    stateGuard.reset();
    return {true, stateTag, stateVal};
}
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggMinMaxN<(AccumulatorMinMax::Sense)-1>(ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggMinMaxN<(AccumulatorMinMax::Sense)1>(ArityType arity);

template <AccumulatorMinMaxN::MinMaxSense S>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggMinMaxNMerge(ArityType arity) {
    invariant(arity == 2 || arity == 3);

    auto [mergeStateTag, mergeStateVal] = moveOwnedFromStack(0);
    value::ValueGuard mergeStateGuard{mergeStateTag, mergeStateVal};

    auto [stateTag, stateVal] = moveOwnedFromStack(1);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [mergeState,
          mergeArray,
          mergeStartIdx,
          mergeMaxSize,
          mergeMemUsage,
          mergeMemLimit,
          mergeIsGroupAccum] = getMultiAccState(mergeStateTag, mergeStateVal);
    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTag, stateVal);
    uassert(7548801,
            "Two arrays to merge should have the same MaxSize component",
            maxSize == mergeMaxSize);

    CollatorInterface* collator = nullptr;
    if (arity == 3) {
        auto [collOwned, collTag, collVal] = getFromStack(2);
        uassert(7548803, "expected a collator argument", collTag == value::TypeTags::collator);
        collator = value::getCollatorView(collVal);
    }

    for (size_t i = 0; i < array->size(); ++i) {
        auto [tag, val] = array->swapAt(i, value::TypeTags::Null, 0);
        mergeMemUsage = aggMinMaxN<S>(
            mergeState, mergeArray, mergeMaxSize, mergeMemUsage, mergeMemLimit, collator, tag, val);
    }

    mergeStateGuard.reset();
    return {true, mergeStateTag, mergeStateVal};
}
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggMinMaxNMerge<(AccumulatorMinMax::Sense)-1>(ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggMinMaxNMerge<(AccumulatorMinMax::Sense)1>(ArityType arity);

template <AccumulatorMinMaxN::MinMaxSense S>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggMinMaxNFinalize(
    ArityType arity) {
    invariant(arity == 2 || arity == 1);
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTag, stateVal);

    CollatorInterface* collator = nullptr;
    if (arity == 2) {
        auto [collOwned, collTag, collVal] = getFromStack(1);
        uassert(7548804, "expected a collator argument", collTag == value::TypeTags::collator);
        collator = value::getCollatorView(collVal);
    }

    constexpr auto less = []() -> bool {
        if constexpr (S == AccumulatorMinMaxN::MinMaxSense::kMax) {
            return false;
        }
        return true;
    }();
    value::ValueCompare<less> comp{collator};
    std::sort(array->values().begin(), array->values().end(), comp);
    if (isGroupAccum) {
        auto [arrayTag, arrayVal] = state->swapAt(
            static_cast<size_t>(AggMultiElems::kInternalArr), value::TypeTags::Null, 0);
        return {true, arrayTag, arrayVal};
    } else {
        auto [arrTag, arrVal] = state->getAt(0);
        auto [outTag, outVal] = value::copyValue(arrTag, arrVal);
        return {true, outTag, outVal};
    }
}
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggMinMaxNFinalize<(AccumulatorMinMax::Sense)-1>(ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggMinMaxNFinalize<(AccumulatorMinMax::Sense)1>(ArityType arity);

namespace {
std::tuple<value::Array*,
           std::pair<value::TypeTags, value::Value>,
           bool,
           int64_t,
           int64_t,
           SortSpec*>
rankState(value::TypeTags stateTag, value::Value stateVal) {
    uassert(
        7795500, "The accumulator state should be an array", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);

    uassert(7795501,
            "The accumulator state should have correct number of elements",
            state->size() == AggRankElems::kRankArraySize);

    auto lastValue = state->getAt(AggRankElems::kLastValue);
    auto [lastValueIsNothingTag, lastValueIsNothingVal] =
        state->getAt(AggRankElems::kLastValueIsNothing);
    auto [lastRankTag, lastRankVal] = state->getAt(AggRankElems::kLastRank);
    auto [sameRankCountTag, sameRankCountVal] = state->getAt(AggRankElems::kSameRankCount);
    auto [sortSpecTag, sortSpecVal] = state->getAt(AggRankElems::kSortSpec);

    uassert(8188900,
            "Last rank is nothing component should be a boolean",
            lastValueIsNothingTag == value::TypeTags::Boolean);
    auto lastValueIsNothing = value::bitcastTo<bool>(lastValueIsNothingVal);

    uassert(7795502,
            "Last rank component should be a 64-bit integer",
            lastRankTag == value::TypeTags::NumberInt64);
    auto lastRank = value::bitcastTo<int64_t>(lastRankVal);

    uassert(7795503,
            "Same rank component should be a 64-bit integer",
            sameRankCountTag == value::TypeTags::NumberInt64);
    auto sameRankCount = value::bitcastTo<int64_t>(sameRankCountVal);

    uassert(8216800,
            "Sort spec component should be a sort spec object",
            sortSpecTag == value::TypeTags::sortSpec);
    auto sortSpec = value::getSortSpecView(sortSpecVal);

    return {state, lastValue, lastValueIsNothing, lastRank, sameRankCount, sortSpec};
}

FastTuple<bool, value::TypeTags, value::Value> builtinAggRankImpl(
    value::TypeTags stateTag,
    value::Value stateVal,
    bool valueOwned,
    value::TypeTags valueTag,
    value::Value valueVal,
    bool isAscending,
    bool dense,
    CollatorInterface* collator = nullptr) {

    const char* kTempSortKeyField = "sortKey";
    // Initialize the accumulator.
    if (stateTag == value::TypeTags::Nothing) {
        auto [newStateTag, newStateVal] = value::makeNewArray();
        value::ValueGuard newStateGuard{newStateTag, newStateVal};
        auto newState = value::getArrayView(newStateVal);
        newState->reserve(AggRankElems::kRankArraySize);
        if (!valueOwned) {
            std::tie(valueTag, valueVal) = value::copyValue(valueTag, valueVal);
        }
        if (valueTag == value::TypeTags::Nothing) {
            newState->push_back(value::TypeTags::Null, 0);  // kLastValue
            newState->push_back(value::TypeTags::Boolean,
                                value::bitcastFrom<bool>(true));  // kLastValueIsNothing
        } else {
            newState->push_back(valueTag, valueVal);  // kLastValue
            newState->push_back(value::TypeTags::Boolean,
                                value::bitcastFrom<bool>(false));  // kLastValueIsNothing
        }
        newState->push_back(value::TypeTags::NumberInt64, 1);  // kLastRank
        newState->push_back(value::TypeTags::NumberInt64, 1);  // kSameRankCount

        auto sortSpec =
            std::make_unique<SortSpec>(BSON(kTempSortKeyField << (isAscending ? 1 : -1)));
        newState->push_back(value::TypeTags::sortSpec,
                            value::bitcastFrom<SortSpec*>(sortSpec.release()));  // kSortSpec
        newStateGuard.reset();
        return {true, newStateTag, newStateVal};
    }

    value::ValueGuard stateGuard{stateTag, stateVal};
    auto [state, lastValue, lastValueIsNothing, lastRank, sameRankCount, sortSpec] =
        rankState(stateTag, stateVal);
    // Update the last value to Nothing before comparison if the flag is set.
    if (lastValueIsNothing) {
        lastValue.first = value::TypeTags::Nothing;
        lastValue.second = 0;
    }

    // Define sort-order compliant comparison function which uses fast pass logic for null and
    // missing and full sort key logic for arrays.
    auto isSameValue = [&](SortSpec* keyGen,
                           std::pair<value::TypeTags, value::Value> currValue,
                           std::pair<value::TypeTags, value::Value> lastValue) {
        if (value::isNullish(currValue.first) && value::isNullish(lastValue.first)) {
            return true;
        }
        if (value::isArray(currValue.first) || value::isArray(lastValue.first)) {
            auto getSortKey = [&](value::TypeTags tag, value::Value val) {
                BSONObjBuilder builder;
                bson::appendValueToBsonObj(builder, kTempSortKeyField, tag, val);
                return keyGen->generateSortKey(builder.obj(), collator);
            };
            auto currKey = getSortKey(currValue.first, currValue.second);
            auto lastKey = getSortKey(lastValue.first, lastValue.second);
            return currKey.compare(lastKey) == 0;
        }
        auto [compareTag, compareVal] = value::compareValue(
            currValue.first, currValue.second, lastValue.first, lastValue.second, collator);
        return compareTag == value::TypeTags::NumberInt32 && compareVal == 0;
    };

    if (isSameValue(sortSpec, std::make_pair(valueTag, valueVal), lastValue)) {
        state->setAt(AggRankElems::kSameRankCount,
                     value::TypeTags::NumberInt64,
                     value::bitcastFrom<int64_t>(sameRankCount + 1));
    } else {
        if (!valueOwned) {
            std::tie(valueTag, valueVal) = value::copyValue(valueTag, valueVal);
        }
        if (valueTag == value::TypeTags::Nothing) {
            state->setAt(AggRankElems::kLastValue, value::TypeTags::Null, 0);
            state->setAt(AggRankElems::kLastValueIsNothing,
                         value::TypeTags::Boolean,
                         value::bitcastFrom<bool>(true));
        } else {
            state->setAt(AggRankElems::kLastValue, valueTag, valueVal);
            state->setAt(AggRankElems::kLastValueIsNothing,
                         value::TypeTags::Boolean,
                         value::bitcastFrom<bool>(false));
        }
        state->setAt(AggRankElems::kLastRank,
                     value::TypeTags::NumberInt64,
                     value::bitcastFrom<int64_t>(dense ? lastRank + 1 : lastRank + sameRankCount));
        state->setAt(AggRankElems::kSameRankCount,
                     value::TypeTags::NumberInt64,
                     value::bitcastFrom<int64_t>(1));
    }
    stateGuard.reset();
    return {true, stateTag, stateVal};
}  // builtinAggRankImpl
}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRankColl(ArityType arity) {
    invariant(arity == 4);
    auto [collatorOwned, collatorTag, collatorVal] = getFromStack(3);
    auto [isAscendingOwned, isAscendingTag, isAscendingVal] = getFromStack(2);
    auto [valueOwned, valueTag, valueVal] = getFromStack(1);
    auto [stateTag, stateVal] = moveOwnedFromStack(0);

    tassert(8216804,
            "Incorrect value type passed to aggRankColl for 'isAscending' parameter.",
            isAscendingTag == value::TypeTags::Boolean);
    auto isAscending = value::bitcastTo<bool>(isAscendingVal);

    tassert(7795504,
            "Incorrect value type passed to aggRankColl for collator.",
            collatorTag == value::TypeTags::collator);
    auto collator = value::getCollatorView(collatorVal);

    return builtinAggRankImpl(stateTag,
                              stateVal,
                              valueOwned,
                              valueTag,
                              valueVal,
                              isAscending,
                              false /* dense */,
                              collator);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggDenseRank(ArityType arity) {
    invariant(arity == 3);
    auto [isAscendingOwned, isAscendingTag, isAscendingVal] = getFromStack(2);
    auto [valueOwned, valueTag, valueVal] = getFromStack(1);
    auto [stateTag, stateVal] = moveOwnedFromStack(0);

    tassert(8216805,
            "Incorrect value type passed to aggDenseRank for 'isAscending' parameter.",
            isAscendingTag == value::TypeTags::Boolean);
    auto isAscending = value::bitcastTo<bool>(isAscendingVal);

    return builtinAggRankImpl(
        stateTag, stateVal, valueOwned, valueTag, valueVal, isAscending, true /* dense */);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRank(ArityType arity) {
    invariant(arity == 3);
    auto [isAscendingOwned, isAscendingTag, isAscendingVal] = getFromStack(2);
    auto [valueOwned, valueTag, valueVal] = getFromStack(1);
    auto [stateTag, stateVal] = moveOwnedFromStack(0);

    tassert(8216803,
            "Incorrect value type passed to aggRank for 'isAscending' parameter.",
            isAscendingTag == value::TypeTags::Boolean);
    auto isAscending = value::bitcastTo<bool>(isAscendingVal);

    return builtinAggRankImpl(
        stateTag, stateVal, valueOwned, valueTag, valueVal, isAscending, false /* dense */);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggDenseRankColl(ArityType arity) {
    invariant(arity == 4);
    auto [collatorOwned, collatorTag, collatorVal] = getFromStack(3);
    auto [isAscendingOwned, isAscendingTag, isAscendingVal] = getFromStack(2);
    auto [valueOwned, valueTag, valueVal] = getFromStack(1);
    auto [stateTag, stateVal] = moveOwnedFromStack(0);

    tassert(8216806,
            "Incorrect value type passed to aggDenseRankColl for 'isAscending' parameter.",
            isAscendingTag == value::TypeTags::Boolean);
    auto isAscending = value::bitcastTo<bool>(isAscendingVal);

    tassert(7795505,
            "Incorrect value type passed to aggDenseRankColl for collator.",
            collatorTag == value::TypeTags::collator);
    auto collator = value::getCollatorView(collatorVal);

    return builtinAggRankImpl(stateTag,
                              stateVal,
                              valueOwned,
                              valueTag,
                              valueVal,
                              isAscending,
                              true /* dense */,
                              collator);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRankFinalize(ArityType arity) {
    invariant(arity == 1);
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);
    auto [state, lastValue, lastValueIsNothing, lastRank, sameRankCount, sortSpec] =
        rankState(stateTag, stateVal);
    if (static_cast<int32_t>(lastRank) == lastRank) {
        return {true, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(lastRank)};
    }
    return {true, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(lastRank)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggExpMovingAvg(ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [fieldOwned, fieldTag, fieldVal] = getFromStack(1);
    if (!value::isNumber(fieldTag)) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    uassert(7821200, "State should be of array type", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);
    uassert(7821201,
            "Unexpected state array size",
            state->size() == static_cast<size_t>(AggExpMovingAvgElems::kSizeOfArray));

    auto [alphaTag, alphaVal] = state->getAt(static_cast<size_t>(AggExpMovingAvgElems::kAlpha));
    uassert(7821202, "alpha is not of decimal type", alphaTag == value::TypeTags::NumberDecimal);
    auto alpha = value::bitcastTo<Decimal128>(alphaVal);

    value::TypeTags currentResultTag;
    value::Value currentResultVal;
    std::tie(currentResultTag, currentResultVal) =
        state->getAt(static_cast<size_t>(AggExpMovingAvgElems::kResult));

    auto decimalVal = value::numericCast<Decimal128>(fieldTag, fieldVal);
    auto result = [&]() {
        if (currentResultTag == value::TypeTags::Null) {
            // Accumulator result has not been yet initialised. We will now
            // set it to decimalVal
            return decimalVal;
        } else {
            uassert(7821203,
                    "currentResultTag is not of decimal type",
                    currentResultTag == value::TypeTags::NumberDecimal);
            auto currentResult = value::bitcastTo<Decimal128>(currentResultVal);
            currentResult = decimalVal.multiply(alpha).add(
                currentResult.multiply(Decimal128(1).subtract(alpha)));
            return currentResult;
        }
    }();

    auto [resultTag, resultVal] = value::makeCopyDecimal(result);

    state->setAt(static_cast<size_t>(AggExpMovingAvgElems::kResult), resultTag, resultVal);
    if (fieldTag == value::TypeTags::NumberDecimal) {
        state->setAt(static_cast<size_t>(AggExpMovingAvgElems::kIsDecimal),
                     value::TypeTags::Boolean,
                     value::bitcastFrom<bool>(true));
    }

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggExpMovingAvgFinalize(
    ArityType arity) {
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);

    uassert(7821204, "State should be of array type", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);

    auto [resultTag, resultVal] = state->getAt(static_cast<size_t>(AggExpMovingAvgElems::kResult));
    if (resultTag == value::TypeTags::Null) {
        return {false, value::TypeTags::Null, 0};
    }
    uassert(7821205, "Unexpected result type", resultTag == value::TypeTags::NumberDecimal);

    auto [isDecimalTag, isDecimalVal] =
        state->getAt(static_cast<size_t>(AggExpMovingAvgElems::kIsDecimal));
    uassert(7821206, "Unexpected isDecimal type", isDecimalTag == value::TypeTags::Boolean);

    if (value::bitcastTo<bool>(isDecimalVal)) {
        std::tie(resultTag, resultVal) = value::copyValue(resultTag, resultVal);
        return {true, resultTag, resultVal};
    } else {
        auto result = value::bitcastTo<Decimal128>(resultVal).toDouble();
        return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
    }
}

namespace {
std::pair<value::TypeTags, value::Value> initializeRemovableSumState() {
    auto [stateTag, stateVal] = value::makeNewArray();
    value::ValueGuard newStateGuard{stateTag, stateVal};
    auto state = value::getArrayView(stateVal);
    state->reserve(static_cast<size_t>(AggRemovableSumElems::kSizeOfArray));

    auto [sumAccTag, sumAccVal] = ByteCode::genericInitializeDoubleDoubleSumState();
    state->push_back(sumAccTag, sumAccVal);  // kSumAcc
    state->push_back(value::TypeTags::NumberInt64,
                     value::bitcastFrom<int64_t>(0));  // kNanCount
    state->push_back(value::TypeTags::NumberInt64,
                     value::bitcastFrom<int64_t>(0));  // kPosInfinityCount
    state->push_back(value::TypeTags::NumberInt64,
                     value::bitcastFrom<int64_t>(0));  // kNegInfinityCount
    state->push_back(value::TypeTags::NumberInt64,
                     value::bitcastFrom<int64_t>(0));  // kDoubleCount
    state->push_back(value::TypeTags::NumberInt64,
                     value::bitcastFrom<int64_t>(0));  // kDecimalCount
    newStateGuard.reset();
    return {stateTag, stateVal};
}
}  // namespace

template <int sign>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableSum(ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    auto [_, fieldTag, fieldVal] = getFromStack(1);

    // Initialize the accumulator.
    if (stateTag == value::TypeTags::Nothing) {
        std::tie(stateTag, stateVal) = initializeRemovableSumState();
    }

    value::ValueGuard stateGuard{stateTag, stateVal};
    uassert(7795108, "state should be of array type", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);

    aggRemovableSumImpl<sign>(state, fieldTag, fieldVal);

    stateGuard.reset();
    return {true, stateTag, stateVal};
}
template FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableSum<-1>(
    ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableSum<1>(
    ArityType arity);

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableSumFinalize(
    ArityType arity) {
    auto [_, stateTag, stateVal] = getFromStack(0);

    uassert(7795109, "state should be of array type", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);
    return aggRemovableSumFinalizeImpl(state);
}

namespace {
// Initialize an array queue
std::tuple<value::TypeTags, value::Value> arrayQueueInit() {
    auto [arrayQueueTag, arrayQueueVal] = value::makeNewArray();
    value::ValueGuard arrayQueueGuard{arrayQueueTag, arrayQueueVal};
    auto arrayQueue = value::getArrayView(arrayQueueVal);
    arrayQueue->reserve(static_cast<size_t>(ArrayQueueElems::kSizeOfArray));

    auto [bufferTag, bufferVal] = value::makeNewArray();
    value::ValueGuard bufferGuard{bufferTag, bufferVal};

    // Make the buffer has at least 1 capacity so that the start index will always be valid.
    auto buffer = value::getArrayView(bufferVal);
    buffer->push_back(value::TypeTags::Null, 0);

    bufferGuard.reset();
    arrayQueue->push_back(bufferTag, bufferVal);
    arrayQueue->push_back(value::TypeTags::NumberInt64, 0);  // kStartIdx
    arrayQueue->push_back(value::TypeTags::NumberInt64, 0);  // kQueueSize
    arrayQueueGuard.reset();
    return {arrayQueueTag, arrayQueueVal};
}
}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggIntegralInit(ArityType arity) {
    auto [unitOwned, unitTag, unitVal] = getFromStack(0);
    auto [isNonRemovableOwned, isNonRemovableTag, isNonRemovableVal] = getFromStack(1);

    tassert(7996820,
            "Invalid unit type",
            unitTag == value::TypeTags::Null || unitTag == value::TypeTags::NumberInt64);
    tassert(7996821, "Invalid isNonRemovable type", isNonRemovableTag == value::TypeTags::Boolean);

    auto [stateTag, stateVal] = value::makeNewArray();
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto state = value::getArrayView(stateVal);
    state->reserve(static_cast<size_t>(AggIntegralElems::kMaxSizeOfArray));

    // AggIntegralElems::kInputQueue
    auto [inputQueueTag, inputQueueVal] = arrayQueueInit();
    state->push_back(inputQueueTag, inputQueueVal);

    // AggIntegralElems::kSortByQueue
    auto [sortByQueueTag, sortByQueueVal] = arrayQueueInit();
    state->push_back(sortByQueueTag, sortByQueueVal);

    // AggIntegralElems::kIntegral
    auto [integralTag, integralVal] = initializeRemovableSumState();
    state->push_back(integralTag, integralVal);

    // AggIntegralElems::kNanCount
    state->push_back(value::TypeTags::NumberInt64, 0);

    // AggIntegralElems::kUnitMillis
    state->push_back(unitTag, unitVal);

    // AggIntegralElems::kIsNonRemovable
    state->push_back(isNonRemovableTag, isNonRemovableVal);

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

namespace {
/**
 * Helper functions for integralAdd/Remove/Finalize
 */
std::tuple<value::Array*,
           value::Array*,
           value::Array*,
           value::Array*,
           int64_t,
           boost::optional<int64_t>,
           bool>
getIntegralState(value::TypeTags stateTag, value::Value stateVal) {
    uassert(
        7821103, "The accumulator state should be an array", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);

    auto maxSize = static_cast<size_t>(AggIntegralElems::kMaxSizeOfArray);
    uassert(7821104,
            "The accumulator state should have correct number of elements",
            state->size() == maxSize);

    auto [inputQueueTag, inputQueueVal] =
        state->getAt(static_cast<size_t>(AggIntegralElems::kInputQueue));
    uassert(7821105, "InputQueue should be of array type", inputQueueTag == value::TypeTags::Array);
    auto inputQueue = value::getArrayView(inputQueueVal);

    auto [sortByQueueTag, sortByQueueVal] =
        state->getAt(static_cast<size_t>(AggIntegralElems::kSortByQueue));
    uassert(
        7821121, "SortByQueue should be of array type", sortByQueueTag == value::TypeTags::Array);
    auto sortByQueue = value::getArrayView(sortByQueueVal);

    auto [integralTag, integralVal] =
        state->getAt(static_cast<size_t>(AggIntegralElems::kIntegral));
    uassert(7821106, "Integral should be of array type", integralTag == value::TypeTags::Array);
    auto integral = value::getArrayView(integralVal);

    auto [nanCountTag, nanCountVal] =
        state->getAt(static_cast<size_t>(AggIntegralElems::kNanCount));
    uassert(7821107,
            "nanCount should be of NumberInt64 type",
            nanCountTag == value::TypeTags::NumberInt64);
    auto nanCount = value::bitcastTo<int64_t>(nanCountVal);

    boost::optional<int64_t> unitMillis;
    auto [unitMillisTag, unitMillisVal] =
        state->getAt(static_cast<size_t>(AggIntegralElems::kUnitMillis));
    if (unitMillisTag != value::TypeTags::Null) {
        uassert(7821108,
                "unitMillis should be of type NumberInt64",
                unitMillisTag == value::TypeTags::NumberInt64);
        unitMillis = value::bitcastTo<int64_t>(unitMillisVal);
    }

    auto [isNonRemovableTag, isNonRemovableVal] =
        state->getAt(static_cast<size_t>(AggIntegralElems::kIsNonRemovable));
    uassert(7996800,
            "isNonRemovable should be of boolean type",
            isNonRemovableTag == value::TypeTags::Boolean);
    auto isNonRemovable = value::bitcastTo<bool>(isNonRemovableVal);

    return {state, inputQueue, sortByQueue, integral, nanCount, unitMillis, isNonRemovable};
}

void updateNaNCount(value::Array* state, int64_t nanCount) {
    state->setAt(static_cast<size_t>(AggIntegralElems::kNanCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(nanCount));
}

void assertTypesForIntegeral(value::TypeTags inputTag,
                             value::TypeTags sortByTag,
                             boost::optional<int64_t> unitMillis) {
    uassert(7821109, "input value should be of numberic type", value::isNumber(inputTag));
    if (unitMillis) {
        uassert(7821110,
                "Sort-by value should be of date type when unitMillis is provided",
                sortByTag == value::TypeTags::Date);
    } else {
        uassert(7821111, "Sort-by value should be of numeric type", value::isNumber(sortByTag));
    }
}
}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::integralOfTwoPointsByTrapezoidalRule(
    std::pair<value::TypeTags, value::Value> prevInput,
    std::pair<value::TypeTags, value::Value> prevSortByVal,
    std::pair<value::TypeTags, value::Value> newInput,
    std::pair<value::TypeTags, value::Value> newSortByVal) {
    if (value::isNaN(prevInput.first, prevInput.second) ||
        value::isNaN(prevSortByVal.first, prevSortByVal.second) ||
        value::isNaN(newInput.first, newInput.second) ||
        value::isNaN(newSortByVal.first, newSortByVal.second)) {
        return {false, value::TypeTags::NumberInt64, 0};
    }

    if ((prevSortByVal.first == value::TypeTags::Date &&
         newSortByVal.first == value::TypeTags::Date) ||
        (value::isNumber(prevSortByVal.first) && value::isNumber(newSortByVal.first))) {
        auto [deltaOwned, deltaTag, deltaVal] = genericSub(
            newSortByVal.first, newSortByVal.second, prevSortByVal.first, prevSortByVal.second);
        value::ValueGuard deltaGuard{deltaOwned, deltaTag, deltaVal};

        auto [sumYOwned, sumYTag, sumYVal] =
            genericAdd(newInput.first, newInput.second, prevInput.first, prevInput.second);
        value::ValueGuard sumYGuard{sumYOwned, sumYTag, sumYVal};

        auto [integralOwned, integralTag, integralVal] =
            genericMul(sumYTag, sumYVal, deltaTag, deltaVal);
        value::ValueGuard integralGuard{integralOwned, integralTag, integralVal};

        auto result = genericDiv(
            integralTag, integralVal, value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(2));
        return result;
    } else {
        return {false, value::TypeTags::NumberInt64, 0};
    }
}

namespace {
/**
 * Functions that operate on `ArrayQueue`
 */
// Get the underlying array, and start index and end index that demarcates the queue
std::tuple<value::Array*, size_t, size_t> getArrayQueueState(value::Array* arrayQueue) {
    auto [arrayTag, arrayVal] = arrayQueue->getAt(static_cast<size_t>(ArrayQueueElems::kArray));
    uassert(7821100, "Expected an array", arrayTag == value::TypeTags::Array);
    auto array = value::getArrayView(arrayVal);
    auto size = array->size();
    uassert(7821116, "Expected non-empty array", size > 0);

    auto [startIdxTag, startIdxVal] =
        arrayQueue->getAt(static_cast<size_t>(ArrayQueueElems::kStartIdx));
    uassert(7821101, "Expected NumberInt64 type", startIdxTag == value::TypeTags::NumberInt64);
    auto startIdx = value::bitcastTo<size_t>(startIdxVal);
    uassert(7821114,
            str::stream() << "Invalid startIdx " << startIdx << " with array size " << size,
            startIdx < size);

    auto [queueSizeTag, queueSizeVal] =
        arrayQueue->getAt(static_cast<size_t>(ArrayQueueElems::kQueueSize));
    uassert(7821102, "Expected NumberInt64 type", queueSizeTag == value::TypeTags::NumberInt64);
    auto queueSize = value::bitcastTo<size_t>(queueSizeVal);
    uassert(7821115,
            str::stream() << "Invalid queueSize " << queueSize << " with array size " << size,
            queueSize <= size);

    return {array, startIdx, queueSize};
}

// Update the startIdex and index of the `ArrayQueue`
void updateArrayQueueState(value::Array* arrayQueue, size_t startIdx, size_t queueSize) {
    arrayQueue->setAt(static_cast<size_t>(ArrayQueueElems::kStartIdx),
                      value::TypeTags::NumberInt64,
                      value::bitcastFrom<size_t>(startIdx));
    arrayQueue->setAt(static_cast<size_t>(ArrayQueueElems::kQueueSize),
                      value::TypeTags::NumberInt64,
                      value::bitcastFrom<size_t>(queueSize));
}

// Return the size of the queue
size_t arrayQueueSize(value::Array* arrayQueue) {
    auto [array, startIdx, queueSize] = getArrayQueueState(arrayQueue);
    return queueSize;
}

// Push an element {tag, value} into the queue
void arrayQueuePush(value::Array* arrayQueue, value::TypeTags tag, value::Value val) {
    /* The underlying array acts as a circular buffer for the queue with `startIdx` and `queueSize`
     * demarcating the filled region (with remaining region containing nulls). When pushing an
     * element to the queue, we set at the corresponding index [= (startIdx + queueSize) %
     * arraySize] the element to be added. If the underlying array is filled, we double the size of
     * the array (by adding nulls); the existing elements in the queue may need to be rearranged
     * when that happens.
     *
     * Eg, Push {v} :
     * => Initial State: (x = filled; _ = empty)
     *       [x x x x]
     *            |
     *         startIdx (queueSize = 4, arraySize = 4)
     *
     * => Double array size:
     *       [x x x x _ _ _ _]
     *            |
     *          startIdx (queueSize = 4, arraySize = 8)
     *
     * => Rearrange elements:
     *       [x x _ _ _ _ x x]
     *                    |
     *                    startIdx (queueSize = 4, arraySize = 8)
     *
     * => Add element:
     *       [x x v _ _ _ x x]
     *                    |
     *                   startIdx (queueSize = 5, arraySize = 8)
     */
    value::ValueGuard guard{tag, val};
    auto [array, startIdx, queueSize] = getArrayQueueState(arrayQueue);
    auto cap = array->size();

    if (queueSize == cap) {
        // reallocate with twice size
        auto newCap = cap * 2;
        array->reserve(newCap);
        auto extend = newCap - cap;

        for (size_t i = 0; i < extend; ++i) {
            array->push_back(value::TypeTags::Null, 0);
        }

        if (startIdx > 0) {
            // existing values wrap over the array
            // need to rearrange the values from [startIdx, cap-1]
            for (size_t from = cap - 1, to = newCap - 1; from >= startIdx; --from, --to) {
                auto [movTag, movVal] = array->swapAt(from, value::TypeTags::Null, 0);
                array->setAt(to, movTag, movVal);
            }
            startIdx += extend;
        }
        cap = newCap;
    }

    auto endIdx = (startIdx + queueSize) % cap;
    guard.reset();
    array->setAt(endIdx, tag, val);
    updateArrayQueueState(arrayQueue, startIdx, queueSize + 1);
}

/* Pops an element {tag, value} from the queue and returns it */
std::pair<value::TypeTags, value::Value> arrayQueuePop(value::Array* arrayQueue) {
    auto [array, startIdx, queueSize] = getArrayQueueState(arrayQueue);
    if (queueSize == 0) {
        return {value::TypeTags::Nothing, 0};
    }
    auto cap = array->size();
    auto pair = array->swapAt(startIdx, value::TypeTags::Null, 0);

    startIdx = (startIdx + 1) % cap;
    updateArrayQueueState(arrayQueue, startIdx, queueSize - 1);
    return pair;
}

std::pair<value::TypeTags, value::Value> arrayQueueFront(value::Array* arrayQueue) {
    auto [array, startIdx, queueSize] = getArrayQueueState(arrayQueue);
    if (queueSize == 0) {
        return {value::TypeTags::Nothing, 0};
    }
    return array->getAt(startIdx);
}

std::pair<value::TypeTags, value::Value> arrayQueueBack(value::Array* arrayQueue) {
    auto [array, startIdx, queueSize] = getArrayQueueState(arrayQueue);
    if (queueSize == 0) {
        return {value::TypeTags::Nothing, 0};
    }
    auto cap = array->size();
    auto endIdx = (startIdx + queueSize - 1) % cap;
    return array->getAt(endIdx);
}

// Returns a value::Array containing N elements at the front of the queue.
// If the queue contains less than N elements, returns all the elements
std::pair<value::TypeTags, value::Value> arrayQueueFrontN(value::Array* arrayQueue, size_t n) {
    auto [array, startIdx, queueSize] = getArrayQueueState(arrayQueue);

    auto [resultArrayTag, resultArrayVal] = value::makeNewArray();
    value::ValueGuard guard{resultArrayTag, resultArrayVal};
    auto resultArray = value::getArrayView(resultArrayVal);
    auto countElem = std::min(n, queueSize);
    resultArray->reserve(countElem);

    auto cap = array->size();
    for (size_t i = 0; i < countElem; ++i) {
        auto idx = (startIdx + i) % cap;

        auto [tag, val] = array->getAt(idx);
        auto [copyTag, copyVal] = value::copyValue(tag, val);
        resultArray->push_back(copyTag, copyVal);
    }

    guard.reset();
    return {resultArrayTag, resultArrayVal};
}

// Returns a value::Array containing N elements at the back of the queue.
// If the queue contains less than N elements, returns all the elements
std::pair<value::TypeTags, value::Value> arrayQueueBackN(value::Array* arrayQueue, size_t n) {
    auto [array, startIdx, queueSize] = getArrayQueueState(arrayQueue);

    auto [arrTag, arrVal] = value::makeNewArray();
    value::ValueGuard guard{arrTag, arrVal};
    auto arr = value::getArrayView(arrVal);
    arr->reserve(std::min(n, queueSize));

    auto cap = array->size();
    auto skip = queueSize > n ? queueSize - n : 0;
    auto elemCount = queueSize > n ? n : queueSize;
    startIdx = (startIdx + skip) % cap;

    for (size_t i = 0; i < elemCount; ++i) {
        auto idx = (startIdx + i) % cap;

        auto [tag, val] = array->getAt(idx);
        auto [copyTag, copyVal] = value::copyValue(tag, val);
        arr->push_back(copyTag, copyVal);
    }

    guard.reset();
    return {arrTag, arrVal};
}
}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggIntegralAdd(ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    auto [inputTag, inputVal] = moveOwnedFromStack(1);
    auto [sortByTag, sortByVal] = moveOwnedFromStack(2);

    value::ValueGuard stateGuard{stateTag, stateVal};
    value::ValueGuard inputGuard{inputTag, inputVal};
    value::ValueGuard sortByGuard{sortByTag, sortByVal};

    auto [state, inputQueue, sortByQueue, integral, nanCount, unitMillis, isNonRemovable] =
        getIntegralState(stateTag, stateVal);

    assertTypesForIntegeral(inputTag, sortByTag, unitMillis);

    if (value::isNaN(inputTag, inputVal) || value::isNaN(sortByTag, sortByVal)) {
        nanCount++;
        updateNaNCount(state, nanCount);
    }

    auto queueSize = arrayQueueSize(inputQueue);
    uassert(7821119, "Queue sizes should match", queueSize == arrayQueueSize(sortByQueue));
    if (queueSize > 0) {
        auto inputBack = arrayQueueBack(inputQueue);
        auto sortByBack = arrayQueueBack(sortByQueue);

        auto [integralDeltaOwned, integralDeltaTag, integralDeltaVal] =
            integralOfTwoPointsByTrapezoidalRule(
                inputBack, sortByBack, {inputTag, inputVal}, {sortByTag, sortByVal});
        value::ValueGuard integralDeltaGuard{
            integralDeltaOwned, integralDeltaTag, integralDeltaVal};
        aggRemovableSumImpl<1>(integral, integralDeltaTag, integralDeltaVal);
    }

    if (isNonRemovable) {
        auto [tag, val] = arrayQueuePop(inputQueue);
        value::releaseValue(tag, val);
        std::tie(tag, val) = arrayQueuePop(sortByQueue);
        value::releaseValue(tag, val);
    }

    inputGuard.reset();
    arrayQueuePush(inputQueue, inputTag, inputVal);

    sortByGuard.reset();
    arrayQueuePush(sortByQueue, sortByTag, sortByVal);

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggIntegralRemove(ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    auto [inputOwned, inputTag, inputVal] = getFromStack(1);
    auto [sortByOwned, sortByTag, sortByVal] = getFromStack(2);

    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [state, inputQueue, sortByQueue, integral, nanCount, unitMillis, isNonRemovable] =
        getIntegralState(stateTag, stateVal);
    uassert(7996801, "Expected integral window to be removable", !isNonRemovable);

    assertTypesForIntegeral(inputTag, sortByTag, unitMillis);

    // verify that the input and sortby value to be removed are the first elements of the queues
    auto [frontInputTag, frontInputVal] = arrayQueuePop(inputQueue);
    value::ValueGuard frontInputGuard{frontInputTag, frontInputVal};
    auto [cmpTag, cmpVal] = value::compareValue(frontInputTag, frontInputVal, inputTag, inputVal);
    uassert(7821113,
            "Attempted to remove unexpected input value",
            cmpTag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(cmpVal) == 0);

    auto [frontSortByTag, frontSortByVal] = arrayQueuePop(sortByQueue);
    value::ValueGuard frontSortByGuard{frontSortByTag, frontSortByVal};
    std::tie(cmpTag, cmpVal) =
        value::compareValue(frontSortByTag, frontSortByVal, sortByTag, sortByVal);
    uassert(7821117,
            "Attempted to remove unexpected sortby value",
            cmpTag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(cmpVal) == 0);

    if (value::isNaN(inputTag, inputVal) || value::isNaN(sortByTag, sortByVal)) {
        nanCount--;
        updateNaNCount(state, nanCount);
    }

    auto queueSize = arrayQueueSize(inputQueue);
    uassert(7821120, "Queue sizes should match", queueSize == arrayQueueSize(sortByQueue));
    if (queueSize > 0) {
        auto inputPair = arrayQueueFront(inputQueue);
        auto sortByPair = arrayQueueFront(sortByQueue);

        auto [integralDeltaOwned, integralDeltaTag, integralDeltaVal] =
            integralOfTwoPointsByTrapezoidalRule(
                {inputTag, inputVal}, {sortByTag, sortByVal}, inputPair, sortByPair);
        value::ValueGuard integralDeltaGuard{
            integralDeltaOwned, integralDeltaTag, integralDeltaVal};
        aggRemovableSumImpl<-1>(integral, integralDeltaTag, integralDeltaVal);
    }

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggIntegralFinalize(
    ArityType arity) {
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);

    auto [state, inputQueue, sortByQueue, integral, nanCount, unitMillis, isNonRemovable] =
        getIntegralState(stateTag, stateVal);

    auto queueSize = arrayQueueSize(inputQueue);
    uassert(7821118, "Queue sizes should match", queueSize == arrayQueueSize(sortByQueue));
    if (queueSize == 0) {
        return {false, value::TypeTags::Null, 0};
    }

    if (nanCount > 0) {
        return {false,
                value::TypeTags::NumberDouble,
                value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())};
    }

    auto [resultOwned, resultTag, resultVal] = aggRemovableSumFinalizeImpl(integral);
    value::ValueGuard resultGuard{resultOwned, resultTag, resultVal};
    if (unitMillis) {
        auto [divResultOwned, divResultTag, divResultVal] =
            genericDiv(resultTag,
                       resultVal,
                       value::TypeTags::NumberInt64,
                       value::bitcastFrom<int64_t>(*unitMillis));
        return {divResultOwned, divResultTag, divResultVal};
    } else {
        resultGuard.reset();
        return {resultOwned, resultTag, resultVal};
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggDerivativeFinalize(
    ArityType arity) {
    auto [unitMillisOwned, unitMillisTag, unitMillisVal] = getFromStack(0);
    auto [inputFirstOwned, inputFirstTag, inputFirstVal] = getFromStack(1);
    auto [sortByFirstOwned, sortByFirstTag, sortByFirstVal] = getFromStack(2);
    auto [inputLastOwned, inputLastTag, inputLastVal] = getFromStack(3);
    auto [sortByLastOwned, sortByLastTag, sortByLastVal] = getFromStack(4);

    if (sortByFirstTag == value::TypeTags::Nothing || sortByLastTag == value::TypeTags::Nothing) {
        return {false, value::TypeTags::Null, 0};
    }

    boost::optional<int64_t> unitMillis;
    if (unitMillisTag != value::TypeTags::Null) {
        uassert(7993408,
                "unitMillis should be of type NumberInt64",
                unitMillisTag == value::TypeTags::NumberInt64);
        unitMillis = value::bitcastTo<int64_t>(unitMillisVal);
    }

    if (unitMillis) {
        uassert(7993409,
                "Unexpected type for sortBy value",
                sortByFirstTag == value::TypeTags::Date && sortByLastTag == value::TypeTags::Date);
    } else {
        uassert(7993410,
                "Unexpected type for sortBy value",
                value::isNumber(sortByFirstTag) && value::isNumber(sortByLastTag));
    }

    auto [runOwned, runTag, runVal] =
        genericSub(sortByLastTag, sortByLastVal, sortByFirstTag, sortByFirstVal);
    value::ValueGuard runGuard{runOwned, runTag, runVal};

    auto [riseOwned, riseTag, riseVal] =
        genericSub(inputLastTag, inputLastVal, inputFirstTag, inputFirstVal);
    value::ValueGuard riseGuard{riseOwned, riseTag, riseVal};

    uassert(7821012, "Input delta should be numeric", value::isNumber(riseTag));

    // Return null if the sortBy delta is zero
    if (runTag == value::TypeTags::NumberDecimal) {
        if (numericCast<Decimal128>(runTag, runVal).isZero()) {
            return {false, value::TypeTags::Null, 0};
        }
    } else {
        if (numericCast<double>(runTag, runVal) == 0) {
            return {false, value::TypeTags::Null, 0};
        }
    }

    auto [divOwned, divTag, divVal] = genericDiv(riseTag, riseVal, runTag, runVal);
    value::ValueGuard divGuard{divOwned, divTag, divVal};

    if (unitMillis) {
        auto [mulOwned, mulTag, mulVal] = genericMul(
            divTag, divVal, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(*unitMillis));
        return {mulOwned, mulTag, mulVal};
    } else {
        divGuard.reset();
        return {divOwned, divTag, divVal};
    }
}

namespace {
std::tuple<value::Array*, value::Array*, value::Array*, value::Array*, int64_t> covarianceState(
    value::TypeTags stateTag, value::Value stateVal) {
    tassert(
        7820800, "The accumulator state should be an array", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);

    tassert(7820801,
            "The accumulator state should have correct number of elements",
            state->size() == static_cast<size_t>(AggCovarianceElems::kSizeOfArray));

    auto [sumXTag, sumXVal] = state->getAt(static_cast<size_t>(AggCovarianceElems::kSumX));
    tassert(7820802, "SumX component should be an array", sumXTag == value::TypeTags::Array);
    auto sumX = value::getArrayView(sumXVal);

    auto [sumYTag, sumYVal] = state->getAt(static_cast<size_t>(AggCovarianceElems::kSumY));
    tassert(7820803, "SumY component should be an array", sumYTag == value::TypeTags::Array);
    auto sumY = value::getArrayView(sumYVal);

    auto [cXYTag, cXYVal] = state->getAt(static_cast<size_t>(AggCovarianceElems::kCXY));
    tassert(7820804, "CXY component should be an array", cXYTag == value::TypeTags::Array);
    auto cXY = value::getArrayView(cXYVal);

    auto [countTag, countVal] = state->getAt(static_cast<size_t>(AggCovarianceElems::kCount));
    tassert(7820805,
            "Count component should be a 64-bit integer",
            countTag == value::TypeTags::NumberInt64);
    auto count = value::bitcastTo<int64_t>(countVal);

    return {state, sumX, sumY, cXY, count};
}

FastTuple<bool, value::TypeTags, value::Value> covarianceCheckNonFinite(value::TypeTags xTag,
                                                                        value::Value xVal,
                                                                        value::TypeTags yTag,
                                                                        value::Value yVal) {
    int nanCnt = 0;
    int posCnt = 0;
    int negCnt = 0;
    bool isDecimal = false;
    auto checkValue = [&](value::TypeTags tag, value::Value val) {
        if (value::isNaN(tag, val)) {
            nanCnt++;
        } else if (tag == value::TypeTags::NumberDecimal) {
            if (value::isInfinity(tag, val)) {
                if (value::bitcastTo<Decimal128>(val).isNegative()) {
                    negCnt++;
                } else {
                    posCnt++;
                }
            }
            isDecimal = true;
        } else {
            auto [doubleOwned, doubleTag, doubleVal] =
                genericNumConvert(tag, val, value::TypeTags::NumberDouble);
            auto value = value::bitcastTo<double>(doubleVal);
            if (value == std::numeric_limits<double>::infinity()) {
                posCnt++;
            } else if (value == -std::numeric_limits<double>::infinity()) {
                negCnt++;
            }
        }
    };
    checkValue(xTag, xVal);
    checkValue(yTag, yVal);

    if (nanCnt == 0 && posCnt == 0 && negCnt == 0) {
        return {false, value::TypeTags::Nothing, 0};
    }
    if (nanCnt > 0 || posCnt * negCnt > 0) {
        if (isDecimal) {
            auto [decimalTag, decimalVal] = value::makeCopyDecimal(Decimal128::kPositiveNaN);
            return {true, decimalTag, decimalVal};
        } else {
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())};
        }
    }
    if (isDecimal) {
        if (posCnt > 0) {
            auto [decimalTag, decimalVal] = value::makeCopyDecimal(Decimal128::kPositiveInfinity);
            return {true, decimalTag, decimalVal};
        } else {
            auto [decimalTag, decimalVal] = value::makeCopyDecimal(Decimal128::kNegativeInfinity);
            return {true, decimalTag, decimalVal};
        }
    } else {
        if (posCnt > 0) {
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(std::numeric_limits<double>::infinity())};
        } else {
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(-std::numeric_limits<double>::infinity())};
        }
    }
}  // covarianceCheckNonFinite
}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::aggRemovableAvgFinalizeImpl(
    value::Array* sumState, int64_t count) {
    if (count == 0) {
        return {false, sbe::value::TypeTags::Null, 0};
    }
    auto [sumOwned, sumTag, sumVal] = aggRemovableSumFinalizeImpl(sumState);

    if (sumTag == value::TypeTags::NumberInt32) {
        auto sum = static_cast<double>(value::bitcastTo<int>(sumVal));
        auto avg = sum / static_cast<double>(count);
        return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(avg)};
    } else if (sumTag == value::TypeTags::NumberInt64) {
        auto sum = static_cast<double>(value::bitcastTo<long long>(sumVal));
        auto avg = sum / static_cast<double>(count);
        return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(avg)};
    } else if (sumTag == value::TypeTags::NumberDouble) {
        auto sum = value::bitcastTo<double>(sumVal);
        if (std::isnan(sum) || std::isinf(sum)) {
            return {false, sumTag, sumVal};
        }
        auto avg = sum / static_cast<double>(count);
        return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(avg)};
    } else if (sumTag == value::TypeTags::NumberDecimal) {
        value::ValueGuard sumGuard{sumOwned, sumTag, sumVal};
        auto sum = value::bitcastTo<Decimal128>(sumVal);
        if (sum.isNaN() || sum.isInfinite()) {
            sumGuard.reset();
            return {sumOwned, sumTag, sumVal};
        }
        auto avg = sum.divide(Decimal128(count));
        auto [avgTag, avgVal] = value::makeCopyDecimal(avg);
        return {true, avgTag, avgVal};
    } else {
        MONGO_UNREACHABLE;
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggCovarianceAdd(ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    auto [xOwned, xTag, xVal] = getFromStack(1);
    auto [yOwned, yTag, yVal] = getFromStack(2);

    // Initialize the accumulator.
    if (stateTag == value::TypeTags::Nothing) {
        std::tie(stateTag, stateVal) = value::makeNewArray();
        value::ValueGuard newStateGuard{stateTag, stateVal};
        auto state = value::getArrayView(stateVal);
        state->reserve(static_cast<size_t>(AggCovarianceElems::kSizeOfArray));

        auto [sumXStateTag, sumXStateVal] = initializeRemovableSumState();
        state->push_back(sumXStateTag, sumXStateVal);  // kSumX
        auto [sumYStateTag, sumYStateVal] = initializeRemovableSumState();
        state->push_back(sumYStateTag, sumYStateVal);  // kSumY
        auto [cXYStateTag, cXYStateVal] = initializeRemovableSumState();
        state->push_back(cXYStateTag, cXYStateVal);                                      // kCXY
        state->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));  // kCount
        newStateGuard.reset();
    }
    value::ValueGuard stateGuard{stateTag, stateVal};

    if (!value::isNumber(xTag) || !value::isNumber(yTag)) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    auto [state, sumXState, sumYState, cXYState, count] = covarianceState(stateTag, stateVal);

    auto [nonFiniteOwned, nonFiniteTag, nonFiniteVal] =
        covarianceCheckNonFinite(xTag, xVal, yTag, yVal);
    if (nonFiniteTag != value::TypeTags::Nothing) {
        value::ValueGuard nonFiniteGuard{nonFiniteOwned, nonFiniteTag, nonFiniteVal};
        aggRemovableSumImpl<1>(cXYState, nonFiniteTag, nonFiniteVal);
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    auto [meanXOwned, meanXTag, meanXVal] = aggRemovableAvgFinalizeImpl(sumXState, count);
    value::ValueGuard meanXGuard{meanXOwned, meanXTag, meanXVal};
    auto [deltaXOwned, deltaXTag, deltaXVal] = genericSub(xTag, xVal, meanXTag, meanXVal);
    value::ValueGuard deltaXGuard{deltaXOwned, deltaXTag, deltaXVal};
    aggRemovableSumImpl<1>(sumXState, xTag, xVal);

    aggRemovableSumImpl<1>(sumYState, yTag, yVal);
    auto [meanYOwned, meanYTag, meanYVal] = aggRemovableAvgFinalizeImpl(sumYState, count + 1);
    value::ValueGuard meanYGuard{meanYOwned, meanYTag, meanYVal};
    auto [deltaYOwned, deltaYTag, deltaYVal] = genericSub(yTag, yVal, meanYTag, meanYVal);
    value::ValueGuard deltaYGuard{deltaYOwned, deltaYTag, deltaYVal};

    auto [deltaCXYOwned, deltaCXYTag, deltaCXYVal] =
        genericMul(deltaXTag, deltaXVal, deltaYTag, deltaYVal);
    value::ValueGuard deltaCXYGuard{deltaCXYOwned, deltaCXYTag, deltaCXYVal};
    aggRemovableSumImpl<1>(cXYState, deltaCXYTag, deltaCXYVal);

    state->setAt(static_cast<size_t>(AggCovarianceElems::kCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(count + 1));

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

namespace {
void updateRemovableSumState(value::Array* state,
                             int64_t nanCount,
                             int64_t posInfinityCount,
                             int64_t negInfinityCount,
                             int64_t doubleCount,
                             int64_t decimalCount) {
    state->setAt(static_cast<size_t>(AggRemovableSumElems::kNanCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(nanCount));
    state->setAt(static_cast<size_t>(AggRemovableSumElems::kPosInfinityCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(posInfinityCount));
    state->setAt(static_cast<size_t>(AggRemovableSumElems::kNegInfinityCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(negInfinityCount));
    state->setAt(static_cast<size_t>(AggRemovableSumElems::kDoubleCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(doubleCount));
    state->setAt(static_cast<size_t>(AggRemovableSumElems::kDecimalCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(decimalCount));
}

void aggRemovableSumReset(value::Array* state) {
    auto [sumAccTag, sumAccVal] = state->getAt(static_cast<size_t>(AggRemovableSumElems::kSumAcc));
    tassert(7820807,
            "sum accumulator elem should be of array type",
            sumAccTag == value::TypeTags::Array);
    auto sumAcc = value::getArrayView(sumAccVal);
    ByteCode::genericResetDoubleDoubleSumState(sumAcc);
    updateRemovableSumState(state, 0, 0, 0, 0, 0);
}
}  // namespace

template <class T, int sign>
void ByteCode::updateRemovableSumAccForIntegerType(value::Array* sumAcc,
                                                   value::TypeTags rhsTag,
                                                   value::Value rhsVal) {
    auto value = value::bitcastTo<T>(rhsVal);
    if (value == std::numeric_limits<T>::min() && sign == -1) {
        // Avoid overflow by processing in two parts.
        aggDoubleDoubleSumImpl(sumAcc, rhsTag, std::numeric_limits<T>::max());
        aggDoubleDoubleSumImpl(sumAcc, rhsTag, value::bitcastFrom<T>(1));
    } else {
        aggDoubleDoubleSumImpl(sumAcc, rhsTag, value::bitcastFrom<T>(value * sign));
    }
}

template <int sign>
void ByteCode::aggRemovableSumImpl(value::Array* state,
                                   value::TypeTags rhsTag,
                                   value::Value rhsVal) {
    static_assert(sign == 1 || sign == -1);
    if (!value::isNumber(rhsTag)) {
        return;
    }

    auto [sumAcc, nanCount, posInfinityCount, negInfinityCount, doubleCount, decimalCount] =
        genericRemovableSumState(state);

    if (rhsTag == value::TypeTags::NumberInt32) {
        updateRemovableSumAccForIntegerType<int32_t, sign>(sumAcc, rhsTag, rhsVal);
    } else if (rhsTag == value::TypeTags::NumberInt64) {
        updateRemovableSumAccForIntegerType<int64_t, sign>(sumAcc, rhsTag, rhsVal);
    } else if (rhsTag == value::TypeTags::NumberDouble) {
        doubleCount += sign;
        auto value = value::bitcastTo<double>(rhsVal);
        if (std::isnan(value)) {
            nanCount += sign;
        } else if (value == std::numeric_limits<double>::infinity()) {
            posInfinityCount += sign;
        } else if (value == -std::numeric_limits<double>::infinity()) {
            negInfinityCount += sign;
        } else {
            if constexpr (sign == -1) {
                value *= -1;
            }
            aggDoubleDoubleSumImpl(
                sumAcc, value::TypeTags::NumberDouble, value::bitcastFrom<double>(value));
        }
        updateRemovableSumState(
            state, nanCount, posInfinityCount, negInfinityCount, doubleCount, decimalCount);
    } else if (rhsTag == value::TypeTags::NumberDecimal) {
        decimalCount += sign;
        auto value = value::bitcastTo<Decimal128>(rhsVal);
        if (value.isNaN()) {
            nanCount += sign;
        } else if (value.isInfinite() && !value.isNegative()) {
            posInfinityCount += sign;
        } else if (value.isInfinite() && value.isNegative()) {
            negInfinityCount += sign;
        } else {
            if constexpr (sign == -1) {
                auto [negDecTag, negDecVal] = value::makeCopyDecimal(value.negate());
                aggDoubleDoubleSumImpl(sumAcc, negDecTag, negDecVal);
                value::releaseValue(negDecTag, negDecVal);
            } else {
                aggDoubleDoubleSumImpl(sumAcc, rhsTag, rhsVal);
            }
        }
        updateRemovableSumState(
            state, nanCount, posInfinityCount, negInfinityCount, doubleCount, decimalCount);
    } else {
        MONGO_UNREACHABLE;
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggCovarianceRemove(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    auto [xOwned, xTag, xVal] = getFromStack(1);
    auto [yOwned, yTag, yVal] = getFromStack(2);
    value::ValueGuard stateGuard{stateTag, stateVal};

    if (!value::isNumber(xTag) || !value::isNumber(yTag)) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    auto [state, sumXState, sumYState, cXYState, count] = covarianceState(stateTag, stateVal);

    auto [nonFiniteOwned, nonFiniteTag, nonFiniteVal] =
        covarianceCheckNonFinite(xTag, xVal, yTag, yVal);
    if (nonFiniteTag != value::TypeTags::Nothing) {
        value::ValueGuard nonFiniteGuard{nonFiniteOwned, nonFiniteTag, nonFiniteVal};
        aggRemovableSumImpl<-1>(cXYState, nonFiniteTag, nonFiniteVal);
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    tassert(7820806, "Can't remove from an empty covariance window", count > 0);
    if (count == 1) {
        state->setAt(static_cast<size_t>(AggCovarianceElems::kCount),
                     value::TypeTags::NumberInt64,
                     value::bitcastFrom<int64_t>(0));
        aggRemovableSumReset(sumXState);
        aggRemovableSumReset(sumYState);
        aggRemovableSumReset(cXYState);
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    aggRemovableSumImpl<-1>(sumXState, xTag, xVal);
    auto [meanXOwned, meanXTag, meanXVal] = aggRemovableAvgFinalizeImpl(sumXState, count - 1);
    value::ValueGuard meanXGuard{meanXOwned, meanXTag, meanXVal};
    auto [deltaXOwned, deltaXTag, deltaXVal] = genericSub(xTag, xVal, meanXTag, meanXVal);
    value::ValueGuard deltaXGuard{deltaXOwned, deltaXTag, deltaXVal};

    auto [meanYOwned, meanYTag, meanYVal] = aggRemovableAvgFinalizeImpl(sumYState, count);
    value::ValueGuard meanYGuard{meanYOwned, meanYTag, meanYVal};
    auto [deltaYOwned, deltaYTag, deltaYVal] = genericSub(yTag, yVal, meanYTag, meanYVal);
    value::ValueGuard deltaYGuard{deltaYOwned, deltaYTag, deltaYVal};
    aggRemovableSumImpl<-1>(sumYState, yTag, yVal);

    auto [deltaCXYOwned, deltaCXYTag, deltaCXYVal] =
        genericMul(deltaXTag, deltaXVal, deltaYTag, deltaYVal);
    value::ValueGuard deltaCXYGuard{deltaCXYOwned, deltaCXYTag, deltaCXYVal};
    aggRemovableSumImpl<-1>(cXYState, deltaCXYTag, deltaCXYVal);

    state->setAt(static_cast<size_t>(AggCovarianceElems::kCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(count - 1));

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggCovarianceFinalize(
    ArityType arity, bool isSamp) {
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);
    auto [state, sumXState, sumYState, cXYState, count] = covarianceState(stateTag, stateVal);

    if (count == 1 && !isSamp) {
        return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0)};
    }

    double adjustedCount = (isSamp ? count - 1 : count);
    if (adjustedCount <= 0) {
        return {false, value::TypeTags::Null, 0};
    }

    auto [cXYOwned, cXYTag, cXYVal] = aggRemovableSumFinalizeImpl(cXYState);
    value::ValueGuard cXYGuard{cXYOwned, cXYTag, cXYVal};
    return genericDiv(
        cXYTag, cXYVal, value::TypeTags::NumberDouble, value::bitcastFrom<double>(adjustedCount));
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggCovarianceSampFinalize(
    ArityType arity) {
    return builtinAggCovarianceFinalize(arity, true /* isSamp */);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggCovariancePopFinalize(
    ArityType arity) {
    return builtinAggCovarianceFinalize(arity, false /* isSamp */);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovablePushAdd(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    if (stateTag == value::TypeTags::Nothing) {
        std::tie(stateTag, stateVal) = arrayQueueInit();
    }
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto [inputTag, inputVal] = moveOwnedFromStack(1);
    if (inputTag == value::TypeTags::Nothing) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }
    value::ValueGuard inputGuard{inputTag, inputVal};

    uassert(7993100, "State should be of array type", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);
    inputGuard.reset();
    arrayQueuePush(state, inputTag, inputVal);
    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovablePushRemove(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto [inputTag, inputVal] = moveOwnedFromStack(1);
    if (inputTag == value::TypeTags::Nothing) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }
    value::ValueGuard inputGuard{inputTag, inputVal};

    uassert(7993101, "State should be of array type", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);
    auto [tag, val] = arrayQueuePop(state);
    value::releaseValue(tag, val);
    stateGuard.reset();
    return {true, stateTag, stateVal};
}

namespace {
std::tuple<value::Array*, value::Array*, int32_t> concatArraysState(value::TypeTags stateTag,
                                                                    value::Value stateVal) {
    tassert(9476001, "state should be of type Array", stateTag == value::TypeTags::Array);
    auto stateArr = value::getArrayView(stateVal);
    tassert(9476002,
            str::stream() << "state array should have "
                          << static_cast<size_t>(AggArrayWithSize::kLast) << " elements",
            stateArr->size() == static_cast<size_t>(AggArrayWithSize::kLast));

    // Read the accumulator from the state.
    auto [accArrTag, accArrVal] = stateArr->getAt(static_cast<size_t>(AggArrayWithSize::kValues));
    tassert(9476003, "accumulator should be of type Array", accArrTag == value::TypeTags::Array);
    auto accArr = value::getArrayView(accArrVal);

    auto [accArrSizeTag, accArrSizeVal] =
        stateArr->getAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues));
    tassert(9476004,
            "accumulator size should be of type NumberInt32",
            accArrSizeTag == value::TypeTags::NumberInt32);

    return {stateArr, accArr, value::bitcastTo<int32_t>(accArrSizeVal)};
}

FastTuple<bool, value::TypeTags, value::Value> pushConcatArraysCommonFinalize(value::Array* state) {
    auto [queueBuffer, startIdx, queueSize] = getArrayQueueState(state);

    auto [resultTag, resultVal] = value::makeNewArray();
    value::ValueGuard resGuard{resultTag, resultVal};
    auto result = value::getArrayView(resultVal);
    result->reserve(queueSize);

    for (size_t i = 0; i < queueSize; ++i) {
        auto idx = startIdx + i;
        if (idx >= queueBuffer->size()) {
            idx -= queueBuffer->size();
        }
        auto [tag, val] = queueBuffer->getAt(idx);
        std::tie(tag, val) = value::copyValue(tag, val);
        result->push_back(tag, val);
    }
    resGuard.reset();
    return {true, resultTag, resultVal};
}
}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovablePushFinalize(
    ArityType arity) {
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);
    uassert(7993102, "State should be of array type", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);

    return pushConcatArraysCommonFinalize(state);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableConcatArraysInit(
    ArityType arity) {
    auto [stateTag, stateVal] = value::makeNewArray();
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto arr = value::getArrayView(stateVal);

    // This will be the structure where the accumulated values are stored.
    auto [accArrTag, accArrVal] = arrayQueueInit();

    // The order is important! The accumulated array should be at index
    // AggArrayWithSize::kValues, and the size (bytes) should be at index
    // AggArrayWithSize::kSizeOfValues.
    arr->push_back(accArrTag, accArrVal);
    arr->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableConcatArraysAdd(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto [newElTag, newElVal] = moveOwnedFromStack(1);
    value::ValueGuard newElGuard{newElTag, newElVal};

    // If the field resolves to Nothing (e.g. if it is missing in the document), then we want to
    // leave the current state as is.
    if (newElTag == value::TypeTags::Nothing) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    auto [sizeCapOwned, sizeCapTag, sizeCapVal] = getFromStack(2);
    tassert(9476000,
            "The size cap must be of type NumberInt32",
            sizeCapTag == value::TypeTags::NumberInt32);
    auto capSize = value::bitcastTo<int32_t>(sizeCapVal);
    auto [stateArr, accArr, accArrSize] = concatArraysState(stateTag, stateVal);

    // Note the importance of templating 'arrayForEach' on 'true' here. The input to $concatArrays
    // is an array. In order to avoid leaking the memory associated with each element of the array,
    // we create copies of each element to store in the accumulator (via templating on 'true'). An
    // example where we might otherwise leak memory is if we get the input off the stack as type
    // 'bsonArray'. Iterating over a 'bsonArray' results in pointers into the underlying BSON. Thus,
    // (without passing 'true') calling 'arrayQueuePush' below would insert elements that are
    // pointers to memory that will be destroyed with 'newElGuard' above, which is the source of a
    // memory leak.
    value::arrayForEach<true>(
        newElTag,
        newElVal,
        // We do an initialization capture because clang fails when trying to capture a structured
        // binding in a lambda expression.
        [&accArr = accArr, &accArrSize = accArrSize, capSize](value::TypeTags elemTag,
                                                              value::Value elemVal) {
            // Check that the size of the accumulator will not exceed the cap.
            auto elemSize = value::getApproximateSize(elemTag, elemVal);
            if (accArrSize + elemSize >= capSize) {
                uasserted(ErrorCodes::ExceededMemoryLimit,
                          str::stream() << "Used too much memory for the $concatArrays operator in "
                                           "$setWindowFields. Memory limit: "
                                        << capSize << " bytes. The window contains "
                                        << accArr->size() << " elements and is of size "
                                        << accArrSize << " bytes. The element being added has size "
                                        << elemSize << " bytes.");
            }
            // Update the state
            arrayQueuePush(accArr, elemTag, elemVal);
            accArrSize += elemSize;
        });
    // Update the window field with the new total size.
    stateArr->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom<int32_t>(accArrSize));
    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableConcatArraysRemove(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto [elTag, elVal] = moveOwnedFromStack(1);
    value::ValueGuard elGuard{elTag, elVal};
    auto [stateArr, accArr, accArrSize] = concatArraysState(stateTag, stateVal);

    // If the field resolves to Nothing (e.g. if it is missing in the document), then we want to
    // leave the current state as is.
    if (elTag == value::TypeTags::Nothing) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    // Note the importance of templating 'arrayForEach' on 'true' here. We followed the same pattern
    // in 'builtinAggRemovableConcatArraysAdd' (see comment there for details), which means we made
    // copies of each element to insert into the accumulator. This is important for some types
    // because while the underlying data stays the same, making a copy can return a value of a
    // different SBE type. For example, if the input to $concatArrays was the bsonArray ["Beauty"],
    // the string "Beauty" would be of type 'bsonString'. When we make a copy to insert it into the
    // accumulator, the new value is of type 'StringSmall'. These two representations of the same
    // string take up different amounts of memory. This is important here because we are tracking
    // accumulator memory usage and need to ensure that the value we subtract from the memory
    // tracker for each element is the same as what we added to the memory tracker in
    // 'builtinAggRemovableConcatArraysAdd'.
    value::arrayForEach<true>(
        elTag,
        elVal,
        // We do an initialization capture because clang fails when trying to
        // capture a structured binding in a lambda expression.
        [&accArr = accArr, &accArrSize = accArrSize](value::TypeTags elemBeingRemovedTag,
                                                     value::Value elemBeingRemovedVal) {
            value::ValueGuard removedGuard{elemBeingRemovedTag, elemBeingRemovedVal};
            auto elemSize = value::getApproximateSize(elemBeingRemovedTag, elemBeingRemovedVal);
            invariant(elemSize <= accArrSize);

            // Ensure that there is a value to remove from the window.
            tassert(9476005, "Trying to remove from an empty window", accArr->size() > 0);

            if (kDebugBuild) {
                // Ensure the value we will remove is in fact the value we have been told to remove.
                // This check is expensive on workloads with a lot of removals, and becomes even
                // more expensive with arbitrarily long arrays.
                auto [frontElemTag, frontElemVal] = arrayQueueFront(accArr);
                auto [cmpTag, cmpVal] = value::compareValue(
                    frontElemTag, frontElemVal, elemBeingRemovedTag, elemBeingRemovedVal);
                invariant(cmpTag == value::TypeTags::NumberInt32 &&
                              value::bitcastTo<int32_t>(cmpVal) == 0,
                          "Can't remove a value that is not at the front of the window");
            }

            // Remove the value.
            auto [removedTag, removedVal] = arrayQueuePop(accArr);
            value::releaseValue(removedTag, removedVal);

            accArrSize -= elemSize;
        });

    // Update the window field with the new total size.
    stateArr->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom<int32_t>(accArrSize));
    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableConcatArraysFinalize(
    ArityType arity) {
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);
    uassert(9476007, "State should be of array type", stateTag == value::TypeTags::Array);
    auto [stateArr, accArr, _] = concatArraysState(stateTag, stateVal);

    return pushConcatArraysCommonFinalize(accArr);
}

namespace {
std::tuple<value::Array*, value::Array*, value::Array*, int64_t, int64_t> removableStdDevState(
    value::TypeTags stateTag, value::Value stateVal) {
    uassert(8019600, "state should be of array type", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);

    uassert(8019601,
            "incorrect size of state array",
            state->size() == static_cast<size_t>(AggRemovableStdDevElems::kSizeOfArray));

    auto [sumTag, sumVal] = state->getAt(static_cast<size_t>(AggRemovableStdDevElems::kSum));
    uassert(8019602, "sum elem should be of array type", sumTag == value::TypeTags::Array);
    auto sum = value::getArrayView(sumVal);

    auto [m2Tag, m2Val] = state->getAt(static_cast<size_t>(AggRemovableStdDevElems::kM2));
    uassert(8019603, "m2 elem should be of array type", m2Tag == value::TypeTags::Array);
    auto m2 = value::getArrayView(m2Val);

    auto [countTag, countVal] = state->getAt(static_cast<size_t>(AggRemovableStdDevElems::kCount));
    uassert(
        8019604, "count elem should be of int64 type", countTag == value::TypeTags::NumberInt64);
    auto count = value::bitcastTo<int64_t>(countVal);

    auto [nonFiniteCountTag, nonFiniteCountVal] =
        state->getAt(static_cast<size_t>(AggRemovableStdDevElems::kNonFiniteCount));
    uassert(8019605,
            "non finite count elem should be of int64 type",
            nonFiniteCountTag == value::TypeTags::NumberInt64);
    auto nonFiniteCount = value::bitcastTo<int64_t>(nonFiniteCountVal);

    return {state, sum, m2, count, nonFiniteCount};
}

void updateRemovableStdDevState(value::Array* state, int64_t count, int64_t nonFiniteCount) {
    state->setAt(static_cast<size_t>(AggRemovableStdDevElems::kCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(count));
    state->setAt(static_cast<size_t>(AggRemovableStdDevElems::kNonFiniteCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(nonFiniteCount));
}
}  // namespace

template <int quantity>
void ByteCode::aggRemovableStdDevImpl(value::TypeTags stateTag,
                                      value::Value stateVal,
                                      value::TypeTags inputTag,
                                      value::Value inputVal) {
    static_assert(quantity == 1 || quantity == -1);
    auto [state, sumState, m2State, count, nonFiniteCount] =
        removableStdDevState(stateTag, stateVal);
    if (!value::isNumber(inputTag)) {
        return;
    }
    if ((inputTag == value::TypeTags::NumberDouble &&
         !std::isfinite(value::bitcastTo<double>(inputVal))) ||
        (inputTag == value::TypeTags::NumberDecimal &&
         !value::bitcastTo<Decimal128>(inputVal).isFinite())) {
        nonFiniteCount += quantity;
        updateRemovableStdDevState(state, count, nonFiniteCount);
        return;
    }

    if (count == 0) {
        // Assuming we are adding value if count == 0.
        aggDoubleDoubleSumImpl(sumState, inputTag, inputVal);
        updateRemovableStdDevState(state, ++count, nonFiniteCount);
        return;
    } else if (count + quantity == 0) {
        genericResetDoubleDoubleSumState(sumState);
        genericResetDoubleDoubleSumState(m2State);
        updateRemovableStdDevState(state, 0, 0);
        return;
    }

    auto inputDouble = value::bitcastTo<double>(value::coerceToDouble(inputTag, inputVal).second);
    auto [sumOwned, sumTag, sumVal] = aggDoubleDoubleSumFinalizeImpl(sumState);
    value::ValueGuard sumGuard{sumOwned, sumTag, sumVal};
    double x = count * inputDouble -
        value::bitcastTo<double>(value::coerceToDouble(sumTag, sumVal).second);
    count += quantity;
    aggDoubleDoubleSumImpl(sumState,
                           value::TypeTags::NumberDouble,
                           value::bitcastFrom<double>(inputDouble * quantity));
    aggDoubleDoubleSumImpl(
        m2State,
        value::TypeTags::NumberDouble,
        value::bitcastFrom<double>(x * x * quantity / (count * (count - quantity))));
    updateRemovableStdDevState(state, count, nonFiniteCount);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableStdDevAdd(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    auto [inputOwned, inputTag, inputVal] = getFromStack(1);
    // Initialize the accumulator.
    if (stateTag == value::TypeTags::Nothing) {
        std::tie(stateTag, stateVal) = value::makeNewArray();
        value::ValueGuard newStateGuard{stateTag, stateVal};
        auto state = value::getArrayView(stateVal);
        state->reserve(static_cast<size_t>(AggRemovableStdDevElems::kSizeOfArray));

        auto [sumStateTag, sumStateVal] = genericInitializeDoubleDoubleSumState();
        state->push_back(sumStateTag, sumStateVal);  // kSum
        auto [m2StateTag, m2StateVal] = genericInitializeDoubleDoubleSumState();
        state->push_back(m2StateTag, m2StateVal);                                        // kM2
        state->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));  // kCount
        state->push_back(value::TypeTags::NumberInt64,
                         value::bitcastFrom<int64_t>(0));  // kNonFiniteCount
        newStateGuard.reset();
    }
    value::ValueGuard stateGuard{stateTag, stateVal};

    aggRemovableStdDevImpl<1>(stateTag, stateVal, inputTag, inputVal);

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableStdDevRemove(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    auto [inputOwned, inputTag, inputVal] = getFromStack(1);
    value::ValueGuard stateGuard{stateTag, stateVal};

    aggRemovableStdDevImpl<-1>(stateTag, stateVal, inputTag, inputVal);

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableStdDevFinalize(
    ArityType arity, bool isSamp) {
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);
    auto [state, sumState, m2State, count, nonFiniteCount] =
        removableStdDevState(stateTag, stateVal);
    if (nonFiniteCount > 0) {
        return {false, value::TypeTags::Null, 0};
    }
    const long long adjustedCount = isSamp ? count - 1 : count;
    if (adjustedCount <= 0) {
        return {false, value::TypeTags::Null, 0};
    }
    auto [m2Owned, m2Tag, m2Val] = aggDoubleDoubleSumFinalizeImpl(m2State);
    value::ValueGuard m2Guard{m2Owned, m2Tag, m2Val};
    auto squaredDifferences = value::bitcastTo<double>(value::coerceToDouble(m2Tag, m2Val).second);
    if (squaredDifferences < 0 || (!isSamp && count == 1)) {
        // m2 is the sum of squared differences from the mean, so it should always be
        // nonnegative. It may take on a small negative value due to floating point error, which
        // breaks the sqrt calculation. In this case, the closest valid value for _m2 is 0, so
        // we reset _m2 and return 0 for the standard deviation.
        // If we're doing a population std dev of one element, it is also correct to return 0.
        genericResetDoubleDoubleSumState(m2State);
        return {false, value::TypeTags::NumberInt32, 0};
    }
    return {false,
            value::TypeTags::NumberDouble,
            value::bitcastFrom<double>(sqrt(squaredDifferences / adjustedCount))};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableStdDevSampFinalize(
    ArityType arity) {
    return builtinAggRemovableStdDevFinalize(arity, true /* isSamp */);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableStdDevPopFinalize(
    ArityType arity) {
    return builtinAggRemovableStdDevFinalize(arity, false /* isSamp */);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableAvgFinalize(
    ArityType arity) {
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);
    auto [countOwned, countTag, countVal] = getFromStack(1);

    tassert(7965901,
            "The avg accumulator state should be an array",
            stateTag == value::TypeTags::Array);

    return aggRemovableAvgFinalizeImpl(value::getArrayView(stateVal), countVal);
}

/**
 * $linearFill implementation
 */
namespace {
std::tuple<value::Array*,
           std::pair<value::TypeTags, value::Value>,
           std::pair<value::TypeTags, value::Value>,
           std::pair<value::TypeTags, value::Value>,
           std::pair<value::TypeTags, value::Value>,
           std::pair<value::TypeTags, value::Value>,
           int64_t>
linearFillState(value::TypeTags stateTag, value::Value stateVal) {
    tassert(
        7971200, "The accumulator state should be an array", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);

    tassert(7971201,
            "The accumulator state should have correct number of elements",
            state->size() == static_cast<size_t>(AggLinearFillElems::kSizeOfArray));

    auto x1 = state->getAt(static_cast<size_t>(AggLinearFillElems::kX1));
    auto y1 = state->getAt(static_cast<size_t>(AggLinearFillElems::kY1));
    auto x2 = state->getAt(static_cast<size_t>(AggLinearFillElems::kX2));
    auto y2 = state->getAt(static_cast<size_t>(AggLinearFillElems::kY2));
    auto prevX = state->getAt(static_cast<size_t>(AggLinearFillElems::kPrevX));
    auto [countTag, countVal] = state->getAt(static_cast<size_t>(AggLinearFillElems::kCount));
    tassert(7971202,
            "Expected count element to be of int64 type",
            countTag == value::TypeTags::NumberInt64);
    auto count = value::bitcastTo<int64_t>(countVal);

    return {state, x1, y1, x2, y2, prevX, count};
}
}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggLinearFillCanAdd(
    ArityType arity) {
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);
    auto [state, x1, y1, x2, y2, prevX, count] = linearFillState(stateTag, stateVal);

    // if y2 is non-null it means we have found a valid upper window bound. in that case if count is
    // positive it means there are still more finalize calls to be made. when count == 0 we have
    // exhausted this window.
    if (y2.first != value::TypeTags::Null) {
        return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(count == 0)};
    }

    // if y2 is null it means we have not yet found the upper window bound so keep on adding input
    // values
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(true)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggLinearFillAdd(ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [inputTag, inputVal] = moveOwnedFromStack(1);
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [sortByTag, sortByVal] = moveOwnedFromStack(2);
    value::ValueGuard sortByGuard{sortByTag, sortByVal};

    // Validate the types of the values
    uassert(7971203,
            "Expected input value type to be numeric or null",
            value::isNumber(inputTag) || inputTag == value::TypeTags::Null);
    uassert(7971204,
            "Expected sortBy value type to be numeric or date",
            value::isNumber(sortByTag) || coercibleToDate(sortByTag));

    auto [state, x1, y1, x2, y2, prevX, count] = linearFillState(stateTag, stateVal);

    // Valdiate the current sortBy value with the previous one and update prevX
    auto [cmpTag, cmpVal] = value::compareValue(sortByTag, sortByVal, prevX.first, prevX.second);
    uassert(7971205,
            "There can be no repeated values in the sort field",
            cmpTag == value::TypeTags::NumberInt32 && cmpVal != 0);

    if (prevX.first != value::TypeTags::Null) {
        uassert(7971206,
                "Conflicting sort value types, previous and current types don't match",
                (coercibleToDate(sortByTag) && coercibleToDate(prevX.first)) ||
                    (value::isNumber(sortByTag) && value::isNumber(prevX.first)));
    }

    auto [copyXTag, copyXVal] = value::copyValue(sortByTag, sortByVal);
    state->setAt(static_cast<size_t>(AggLinearFillElems::kPrevX), copyXTag, copyXVal);

    // Update x2/y2 to the current sortby/input values
    sortByGuard.reset();
    auto [oldX2Tag, oldX2Val] =
        state->swapAt(static_cast<size_t>(AggLinearFillElems::kX2), sortByTag, sortByVal);
    value::ValueGuard oldX2Guard{oldX2Tag, oldX2Val};

    inputGuard.reset();
    auto [oldY2Tag, oldY2Val] =
        state->swapAt(static_cast<size_t>(AggLinearFillElems::kY2), inputTag, inputVal);
    value::ValueGuard oldY2Guard{oldY2Tag, oldY2Val};

    // If (old) y2 is non-null, it means we need to look for new end-points (x1, y1), (x2, y2)
    // and the segment spanned be previous endpoints is exhausted. Count should be zero at
    // this point. Update (x1, y1) to the previous (x2, y2)
    if (oldY2Tag != value::TypeTags::Null) {
        tassert(7971207, "count value should be zero", count == 0);
        oldX2Guard.reset();
        state->setAt(static_cast<size_t>(AggLinearFillElems::kX1), oldX2Tag, oldX2Val);
        oldY2Guard.reset();
        state->setAt(static_cast<size_t>(AggLinearFillElems::kY1), oldY2Tag, oldY2Val);
    }

    state->setAt(static_cast<size_t>(AggLinearFillElems::kCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(++count));

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

// Given two known points (x1, y1) and (x2, y2) and a value x that lies between those two
// points, we solve (or fill) for y with the following formula: y = y1 + (x - x1) * ((y2 -
// y1)/(x2 - x1))
FastTuple<bool, value::TypeTags, value::Value> ByteCode::linearFillInterpolate(
    std::pair<value::TypeTags, value::Value> x1,
    std::pair<value::TypeTags, value::Value> y1,
    std::pair<value::TypeTags, value::Value> x2,
    std::pair<value::TypeTags, value::Value> y2,
    std::pair<value::TypeTags, value::Value> x) {
    // (y2 - y1)
    auto [delYOwned, delYTag, delYVal] = genericSub(y2.first, y2.second, y1.first, y1.second);
    value::ValueGuard delYGuard{delYOwned, delYTag, delYVal};

    // (x2 - x1)
    auto [delXOwned, delXTag, delXVal] = genericSub(x2.first, x2.second, x1.first, x1.second);
    value::ValueGuard delXGuard{delXOwned, delXTag, delXVal};

    // (y2 - y1) / (x2 - x1)
    auto [divOwned, divTag, divVal] = genericDiv(delYTag, delYVal, delXTag, delXVal);
    value::ValueGuard divGuard{divOwned, divTag, divVal};

    // (x - x1)
    auto [subOwned, subTag, subVal] = genericSub(x.first, x.second, x1.first, x1.second);
    value::ValueGuard subGuard{subOwned, subTag, subVal};

    // (x - x1) * ((y2 - y1) / (x2 - x1))
    auto [mulOwned, mulTag, mulVal] = genericMul(subTag, subVal, divTag, divVal);
    value::ValueGuard mulGuard{mulOwned, mulTag, mulVal};

    // y1 + (x - x1) * ((y2 - y1) / (x2 - x1))
    return genericAdd(y1.first, y1.second, mulTag, mulVal);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggLinearFillFinalize(
    ArityType arity) {
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);
    auto [xOwned, sortByTag, sortByVal] = getFromStack(1);
    auto [state, x1, y1, x2, y2, prevX, count] = linearFillState(stateTag, stateVal);

    tassert(7971208, "count should be positive", count > 0);
    state->setAt(static_cast<size_t>(AggLinearFillElems::kCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(--count));

    // if y2 is null it means the current window is the last window frame in the partition
    if (y2.first == value::TypeTags::Null) {
        return {false, value::TypeTags::Null, 0};
    }

    // If count == 0, we are currently handling the last docoument in the window frame (x2/y2)
    // so we can return y2 directly. Note that the document represented by y1 was returned as
    // part of previous window (when it was y2)
    if (count == 0) {
        auto [y2Tag, y2Val] = value::copyValue(y2.first, y2.second);
        return {true, y2Tag, y2Val};
    }

    // If y1 is null it means the current window is the first window frame in the partition
    if (y1.first == value::TypeTags::Null) {
        return {false, value::TypeTags::Null, 0};
    }
    return linearFillInterpolate(x1, y1, x2, y2, {sortByTag, sortByVal});
}

/**
 * Implementation for $firstN/$lastN removable window function
 */
namespace {
std::tuple<value::Array*, size_t> firstLastNState(value::TypeTags stateTag, value::Value stateVal) {
    uassert(8070600, "state should be of array type", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);

    uassert(8070601,
            "incorrect size of state array",
            state->size() == static_cast<size_t>(AggFirstLastNElems::kSizeOfArray));

    auto [queueTag, queueVal] = state->getAt(static_cast<size_t>(AggFirstLastNElems::kQueue));
    uassert(8070602, "Queue should be of array type", queueTag == value::TypeTags::Array);
    auto queue = value::getArrayView(queueVal);

    auto [nTag, nVal] = state->getAt(static_cast<size_t>(AggFirstLastNElems::kN));
    uassert(8070603, "'n' elem should be of int64 type", nTag == value::TypeTags::NumberInt64);
    auto n = value::bitcastTo<int64_t>(nVal);

    return {queue, static_cast<size_t>(n)};
}
}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggFirstLastNInit(ArityType arity) {
    auto [fieldOwned, fieldTag, fieldVal] = getFromStack(0);

    auto [nOwned, nTag, nVal] = genericNumConvert(fieldTag, fieldVal, value::TypeTags::NumberInt64);
    uassert(8070607, "Failed to convert to 64-bit integer", nTag == value::TypeTags::NumberInt64);

    auto n = value::bitcastTo<int64_t>(nVal);
    uassert(8070608, "Expected 'n' to be positive", n > 0);

    auto [queueTag, queueVal] = arrayQueueInit();

    auto [stateTag, stateVal] = value::makeNewArray();
    auto state = value::getArrayView(stateVal);
    state->push_back(queueTag, queueVal);
    state->push_back(nTag, nVal);
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggFirstLastNAdd(ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [fieldTag, fieldVal] = moveOwnedFromStack(1);
    value::ValueGuard fieldGuard{fieldTag, fieldVal};

    auto [queue, n] = firstLastNState(stateTag, stateVal);

    fieldGuard.reset();
    arrayQueuePush(queue, fieldTag, fieldVal);

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggFirstLastNRemove(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [fieldTag, fieldVal] = moveOwnedFromStack(1);
    value::ValueGuard fieldGuard{fieldTag, fieldVal};

    auto [queue, n] = firstLastNState(stateTag, stateVal);

    auto [popTag, popVal] = arrayQueuePop(queue);
    value::ValueGuard popValueGuard{popTag, popVal};

    auto [cmpTag, cmpVal] = value::compareValue(popTag, popVal, fieldTag, fieldVal);
    tassert(8070604,
            "Encountered unexpected value",
            cmpTag == value::TypeTags::NumberInt32 && cmpVal == 0);

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

template <AccumulatorFirstLastN::Sense S>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggFirstLastNFinalize(
    ArityType arity) {
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);
    auto [queue, n] = firstLastNState(stateTag, stateVal);

    if constexpr (S == AccumulatorFirstLastN::Sense::kFirst) {
        auto [arrTag, arrVal] = arrayQueueFrontN(queue, n);
        return {true, arrTag, arrVal};
    } else {
        auto [arrTag, arrVal] = arrayQueueBackN(queue, n);
        return {true, arrTag, arrVal};
    }
}
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggFirstLastNFinalize<(AccumulatorFirstLastN::Sense)-1>(ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggFirstLastNFinalize<(AccumulatorFirstLastN::Sense)1>(ArityType arity);

namespace {
std::tuple<value::Array*, value::ArrayMultiSet*, int32_t> setOperatorCommonState(
    value::TypeTags stateTag, value::Value stateVal) {
    tassert(8124900, "state should be of type Array", stateTag == value::TypeTags::Array);
    auto stateArr = value::getArrayView(stateVal);
    tassert(8124901,
            str::stream() << "state array should have "
                          << static_cast<size_t>(AggArrayWithSize::kLast) << " elements",
            stateArr->size() == static_cast<size_t>(AggArrayWithSize::kLast));

    // Read the accumulator from the state.
    auto [accMultiSetTag, accMultiSetVal] =
        stateArr->getAt(static_cast<size_t>(AggArrayWithSize::kValues));
    tassert(8124902,
            "accumulator should be of type MultiSet",
            accMultiSetTag == value::TypeTags::ArrayMultiSet);
    auto accMultiSet = value::getArrayMultiSetView(accMultiSetVal);

    auto [accMultiSetSizeTag, accMultiSetSizeVal] =
        stateArr->getAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues));
    tassert(8124903,
            "accumulator size should be of type NumberInt32",
            accMultiSetSizeTag == value::TypeTags::NumberInt32);

    return {stateArr, accMultiSet, value::bitcastTo<int32_t>(accMultiSetSizeVal)};
}

FastTuple<bool, value::TypeTags, value::Value> aggRemovableSetCommonInitImpl(
    CollatorInterface* collator) {
    auto [stateTag, stateVal] = value::makeNewArray();
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto stateArr = value::getArrayView(stateVal);

    auto [mSetTag, mSetVal] = value::makeNewArrayMultiSet(collator);

    // the order is important!!!
    stateArr->push_back(mSetTag, mSetVal);  // the multiset with the values
    stateArr->push_back(value::TypeTags::NumberInt32,
                        value::bitcastFrom<int32_t>(0));  // the size in bytes of the multiset
    stateGuard.reset();
    return {true, stateTag, stateVal};
}
}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableSetCommonInit(
    ArityType arity) {
    return aggRemovableSetCommonInitImpl(nullptr /* collator */);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableSetCommonCollInit(
    ArityType arity) {
    auto [collatorOwned, collatorTag, collatorVal] = getFromStack(0);
    tassert(8124904, "expected value of type 'collator'", collatorTag == value::TypeTags::collator);

    return aggRemovableSetCommonInitImpl(value::getCollatorView(collatorVal));
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableAddToSetAdd(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto [newElTag, newElVal] = moveOwnedFromStack(1);
    value::ValueGuard newElGuard{newElTag, newElVal};
    auto [sizeCapOwned, sizeCapTag, sizeCapVal] = getFromStack(2);
    tassert(8124905,
            "The size cap must be of type NumberInt32",
            sizeCapTag == value::TypeTags::NumberInt32);
    auto capSize = value::bitcastTo<int32_t>(sizeCapVal);

    auto [stateArr, accMultiSet, accMultiSetSize] = setOperatorCommonState(stateTag, stateVal);

    // Check the size of the accumulator will not exceed the cap.
    int32_t newElSize = value::getApproximateSize(newElTag, newElVal);
    if (accMultiSetSize + newElSize >= capSize) {
        auto elsNum = accMultiSet->size();
        auto setTotalSize = accMultiSetSize;
        uasserted(ErrorCodes::ExceededMemoryLimit,
                  str::stream() << "Used too much memory for a single set. Memory limit: "
                                << capSize << " bytes. The set contains " << elsNum
                                << " elements and is of size " << setTotalSize
                                << " bytes. The element being added has size " << newElSize
                                << " bytes.");
    }

    // Update the state.
    stateArr->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom<int32_t>(accMultiSetSize + newElSize));
    accMultiSet->push_back(newElTag, newElVal);
    newElGuard.reset();
    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableAddToSetRemove(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto [elTag, elVal] = moveOwnedFromStack(1);
    value::ValueGuard elGuard{elTag, elVal};

    auto [stateArr, accMultiSet, accMultiSetSize] = setOperatorCommonState(stateTag, stateVal);

    int32_t elSize = value::getApproximateSize(elTag, elVal);
    invariant(elSize <= accMultiSetSize);
    stateArr->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom<int32_t>(accMultiSetSize - elSize));

    accMultiSet->remove(elTag, elVal);
    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableSetCommonFinalize(
    ArityType arity) {
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);

    auto [stateArr, accMultiSet, _] = setOperatorCommonState(stateTag, stateVal);

    // Convert the multiSet to Set.
    auto [accSetTag, accSetVal] = value::makeNewArraySet(accMultiSet->getCollator());
    value::ValueGuard resGuard{accSetTag, accSetVal};
    auto accSet = value::getArraySetView(accSetVal);
    for (const auto& p : accMultiSet->values()) {
        auto [cTag, cVal] = copyValue(p.first, p.second);
        accSet->push_back(cTag, cVal);
    }
    resGuard.reset();
    return {true, accSetTag, accSetVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableSetUnionAdd(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto [newElTag, newElVal] = moveOwnedFromStack(1);
    value::ValueGuard newElGuard{newElTag, newElVal};
    auto [sizeCapOwned, sizeCapTag, sizeCapVal] = getFromStack(2);
    tassert(9475901,
            "The size cap must be of type NumberInt32",
            sizeCapTag == value::TypeTags::NumberInt32);
    auto capSize = value::bitcastTo<int32_t>(sizeCapVal);
    auto [stateArr, accMultiSet, accMultiSetSize] = setOperatorCommonState(stateTag, stateVal);

    // If the field resolves to Nothing (e.g. if it is missing in the document), then we want to
    // leave the current state as is.
    if (newElTag == value::TypeTags::Nothing) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    // Note the importance of templating 'arrayForEach' on 'true' here. The input to $setUnion
    // is an array. In order to avoid leaking the memory associated with each element of the array,
    // we create copies of each element to store in the accumulator (via templating on 'true'). An
    // example where we might otherwise leak memory is if we get the input off the stack as type
    // 'bsonArray'. Iterating over a 'bsonArray' results in pointers into the underlying BSON. Thus,
    // (without passing 'true') calling 'arrayQueuePush' below would insert elements that are
    // pointers to memory that will be destroyed with 'newElGuard' above, which is the source of a
    // memory leak.
    value::arrayForEach<true>(
        newElTag,
        newElVal,
        // We do an initialization capture because clang fails when trying to capture a structured
        // binding in a lambda expression.
        [&accMultiSet = accMultiSet, &accMultiSetSize = accMultiSetSize, capSize](
            value::TypeTags elemTag, value::Value elemVal) {
            // Check that the size of the accumulator will not exceed the cap.
            auto elemSize = value::getApproximateSize(elemTag, elemVal);
            if (accMultiSetSize + elemSize >= capSize) {
                uasserted(ErrorCodes::ExceededMemoryLimit,
                          str::stream()
                              << "Used too much memory for the $setUnion operator in "
                                 "$setWindowFields. Memory limit: "
                              << capSize << " bytes. The set contains " << accMultiSet->size()
                              << " elements and is of size " << accMultiSetSize
                              << " bytes. The element being added has size " << elemSize
                              << " bytes.");
            }

            // Update the state
            accMultiSet->push_back(elemTag, elemVal);
            accMultiSetSize += elemSize;
        });

    // Update the window field with the new total size.
    stateArr->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom<int32_t>(accMultiSetSize));

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableSetUnionRemove(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto [elTag, elVal] = moveOwnedFromStack(1);
    value::ValueGuard elGuard{elTag, elVal};
    auto [stateArr, accMultiSet, accMultiSetSize] = setOperatorCommonState(stateTag, stateVal);

    // If the field resolves to Nothing (e.g. if it is missing in the document), then we want to
    // leave the current state as is.
    if (elTag == value::TypeTags::Nothing) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    // Note the importance of templating 'arrayForEach' on 'true' here. We followed the same pattern
    // in 'builtinAggRemovableSetUnionAdd' (see comment there for details), which means we made
    // copies of each element to insert into the accumulator. This is important for some types
    // because while the underlying data stays the same, making a copy can return a value of a
    // different SBE type. For example, if the input to $setUnion was the bsonArray ["Beauty"], the
    // string "Beauty" would be of type 'bsonString'. When we make a copy to insert it into the
    // accumulator, the new value is of type 'StringSmall'. These two representations of the same
    // string take up different amounts of memory. This is important here because we are tracking
    // accumulator memory usage and need to ensure that the value we subtract from the memory
    // tracker for each element is the same as what we added to the memory tracker in
    // 'builtinAggRemovableSetUnionAdd'.
    value::arrayForEach<true>(
        elTag,
        elVal,
        // We do an initialization capture because clang fails when trying to
        // capture a structured binding in a lambda expression.
        [&accMultiSet = accMultiSet, &accMultiSetSize = accMultiSetSize](
            value::TypeTags elemBeingRemovedTag, value::Value elemBeingRemovedVal) {
            value::ValueGuard removedGuard{elemBeingRemovedTag, elemBeingRemovedVal};
            auto elemSize = value::getApproximateSize(elemBeingRemovedTag, elemBeingRemovedVal);
            invariant(elemSize <= accMultiSetSize);
            tassert(9475902,
                    "Can't remove a value that is not contained in the window",
                    accMultiSet->remove(elemBeingRemovedTag, elemBeingRemovedVal));
            accMultiSetSize -= elemSize;
        });

    // Update the window field with the new total size.
    stateArr->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom<int32_t>(accMultiSetSize));

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

namespace {
static std::tuple<value::Array*, value::TypeTags, value::Value, size_t, int32_t, int32_t>
accumulatorNState(value::TypeTags stateTag, value::Value stateVal) {
    tassert(
        8178100, "The accumulator state should be an array", stateTag == value::TypeTags::Array);
    auto stateArr = value::getArrayView(stateVal);

    tassert(8178101,
            str::stream() << "state array should have "
                          << static_cast<size_t>(AggAccumulatorNElems::kSizeOfArray)
                          << " elements but found " << stateArr->size(),
            stateArr->size() == static_cast<size_t>(AggAccumulatorNElems::kSizeOfArray));

    // Read the accumulator from the state.
    auto [accumulatorTag, accumulatorVal] =
        stateArr->getAt(static_cast<size_t>(AggAccumulatorNElems::kValues));

    // Read N from the state
    auto [nTag, nVal] = stateArr->getAt(static_cast<size_t>(AggAccumulatorNElems::kN));
    tassert(8178103, "N should be of type NumberInt64", nTag == value::TypeTags::NumberInt64);

    // Read memory usage information from state
    auto [memUsageTag, memUsage] =
        stateArr->getAt(static_cast<size_t>(AggAccumulatorNElems::kMemUsage));
    tassert(8178104,
            "MemUsage component should be of type NumberInt32",
            memUsageTag == value::TypeTags::NumberInt32);

    auto [memLimitTag, memLimit] =
        stateArr->getAt(static_cast<size_t>(AggAccumulatorNElems::kMemLimit));
    tassert(8178105,
            "MemLimit component should be of type NumberInt32",
            memLimitTag == value::TypeTags::NumberInt32);

    return {stateArr,
            accumulatorTag,
            accumulatorVal,
            value::bitcastTo<size_t>(nVal),
            value::bitcastTo<int32_t>(memUsage),
            value::bitcastTo<int32_t>(memLimit)};
}
}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::aggRemovableMinMaxNInitImpl(
    CollatorInterface* collator) {
    auto [sizeOwned, sizeTag, sizeVal] = getFromStack(0);

    auto [nOwned, nTag, nVal] = genericNumConvert(sizeTag, sizeVal, value::TypeTags::NumberInt64);
    uassert(8178107, "Failed to convert to 64-bit integer", nTag == value::TypeTags::NumberInt64);

    auto n = value::bitcastTo<int64_t>(nVal);
    uassert(8178108, "Expected 'n' to be positive", n > 0);

    auto [sizeCapOwned, sizeCapTag, sizeCapVal] = getFromStack(1);
    uassert(8178109,
            "The size cap must be of type NumberInt32",
            sizeCapTag == value::TypeTags::NumberInt32);

    // Initialize the state
    auto [stateTag, stateVal] = value::makeNewArray();
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto stateArr = value::getArrayView(stateVal);

    // the order is important!!!
    auto [mSetTag, mSetVal] = value::makeNewArrayMultiSet(collator);
    stateArr->push_back(mSetTag, mSetVal);  // The multiset with the values.
    stateArr->push_back(nTag, nVal);        // The maximum number of elements in the multiset.
    stateArr->push_back(value::TypeTags::NumberInt32,
                        value::bitcastFrom<int32_t>(0));  // The size of the multiset in bytes.
    stateArr->push_back(sizeCapTag,
                        sizeCapVal);  // The maximum possible size of the multiset in bytes.

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableMinMaxNCollInit(
    ArityType arity) {
    auto [collatorOwned, collatorTag, collatorVal] = getFromStack(2);
    tassert(8178111, "expected value of type 'collator'", collatorTag == value::TypeTags::collator);
    return aggRemovableMinMaxNInitImpl(value::getCollatorView(collatorVal));
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableMinMaxNInit(
    ArityType arity) {
    return aggRemovableMinMaxNInitImpl(nullptr);
}


FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableMinMaxNAdd(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto [newElTag, newElVal] = moveOwnedFromStack(1);
    value::ValueGuard newElGuard{newElTag, newElVal};

    if (value::isNullish(newElTag)) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    auto [stateArr, accMultiSetTag, accMultiSetVal, n, memUsage, memLimit] =
        accumulatorNState(stateTag, stateVal);
    tassert(8178102,
            "accumulator should be of type MultiSet",
            accMultiSetTag == value::TypeTags::ArrayMultiSet);
    auto accMultiSet = value::getArrayMultiSetView(accMultiSetVal);

    int32_t newElSize = value::getApproximateSize(newElTag, newElVal);

    updateAndCheckMemUsage(stateArr,
                           memUsage,
                           newElSize,
                           memLimit,
                           static_cast<size_t>(AggAccumulatorNElems::kMemUsage));

    newElGuard.reset();
    accMultiSet->push_back(newElTag, newElVal);

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableMinMaxNRemove(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto [_, elTag, elVal] = getFromStack(1);

    if (value::isNullish(elTag)) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    auto [stateArr, accMultiSetTag, accMultiSetVal, n, memUsage, memLimit] =
        accumulatorNState(stateTag, stateVal);
    tassert(8155723,
            "accumulator should be of type MultiSet",
            accMultiSetTag == value::TypeTags::ArrayMultiSet);
    auto accMultiSet = value::getArrayMultiSetView(accMultiSetVal);

    int32_t elSize = value::getApproximateSize(elTag, elVal);
    invariant(elSize <= memUsage);

    // remove element
    stateArr->setAt(static_cast<size_t>(AggAccumulatorNElems::kMemUsage),
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom<int32_t>(memUsage - elSize));
    tassert(8178116, "Element was not removed", accMultiSet->remove(elTag, elVal));

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

template <AccumulatorMinMaxN::MinMaxSense S>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableMinMaxNFinalize(
    ArityType arity) {
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);

    auto [stateArr, accMultiSetTag, accMultiSetVal, n, memUsage, memLimit] =
        accumulatorNState(stateTag, stateVal);
    tassert(8155724,
            "accumulator should be of type MultiSet",
            accMultiSetTag == value::TypeTags::ArrayMultiSet);
    auto accMultiSet = value::getArrayMultiSetView(accMultiSetVal);

    // Create an empty array to fill with the results
    auto [resultArrayTag, resultArrayVal] = value::makeNewArray();
    value::ValueGuard resultGuard{resultArrayTag, resultArrayVal};
    auto resultArray = value::getArrayView(resultArrayVal);
    resultArray->reserve(n);

    if constexpr (S == AccumulatorMinMaxN::MinMaxSense::kMin) {
        for (auto it = accMultiSet->values().cbegin();
             it != accMultiSet->values().cend() && resultArray->size() < n;
             ++it) {
            resultArray->push_back(value::copyValue(it->first, it->second));
        }
    } else {
        for (auto it = accMultiSet->values().crbegin();
             it != accMultiSet->values().crend() && resultArray->size() < n;
             ++it) {
            resultArray->push_back(value::copyValue(it->first, it->second));
        }
    }

    resultGuard.reset();
    return {true, resultArrayTag, resultArrayVal};
}
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggRemovableMinMaxNFinalize<(AccumulatorMinMaxN::MinMaxSense)-1>(ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggRemovableMinMaxNFinalize<(AccumulatorMinMaxN::MinMaxSense)1>(ArityType arity);

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableTopBottomNInit(
    ArityType arity) {
    auto [maxSizeOwned, maxSizeTag, maxSizeVal] = getFromStack(0);
    auto [memLimitOwned, memLimitTag, memLimitVal] = getFromStack(1);

    auto [nOwned, nTag, nVal] =
        genericNumConvert(maxSizeTag, maxSizeVal, value::TypeTags::NumberInt64);
    uassert(8155711, "Failed to convert to 64-bit integer", nTag == value::TypeTags::NumberInt64);

    auto n = value::bitcastTo<int64_t>(nVal);
    uassert(8155708, "Expected 'n' to be positive", n > 0);

    tassert(8155709,
            "memLimit should be of type NumberInt32",
            memLimitTag == value::TypeTags::NumberInt32);

    auto [stateTag, stateVal] = value::makeNewArray();
    value::ValueGuard stateGuard{stateTag, stateVal};
    auto stateArr = value::getArrayView(stateVal);

    auto [multiMapTag, multiMapVal] = value::makeNewMultiMap();
    stateArr->push_back(multiMapTag, multiMapVal);

    stateArr->push_back(nTag, nVal);
    stateArr->push_back(value::TypeTags::NumberInt32, 0);
    stateArr->push_back(memLimitTag, memLimitVal);

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableTopBottomNAdd(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [state, multiMapTag, multiMapVal, n, memSize, memLimit] =
        accumulatorNState(stateTag, stateVal);
    tassert(8155702, "value should be of type MultiMap", multiMapTag == value::TypeTags::MultiMap);
    auto multiMap = value::getMultiMapView(multiMapVal);

    auto key = moveOwnedFromStack(1);
    auto value = moveOwnedFromStack(2);

    multiMap->insert(key, value);

    auto kvSize = value::getApproximateSize(key.first, key.second) +
        value::getApproximateSize(value.first, value.second);
    updateAndCheckMemUsage(
        state, memSize, kvSize, memLimit, static_cast<size_t>(AggAccumulatorNElems::kMemUsage));

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableTopBottomNRemove(
    ArityType arity) {
    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto [state, multiMapTag, multiMapVal, n, memSize, memLimit] =
        accumulatorNState(stateTag, stateVal);
    tassert(8155726, "value should be of type MultiMap", multiMapTag == value::TypeTags::MultiMap);
    auto multiMap = value::getMultiMapView(multiMapVal);

    auto [keyOwned, keyTag, keyVal] = getFromStack(1);
    auto [outputOwned, outputTag, outputVal] = getFromStack(2);

    auto removed = multiMap->remove({keyTag, keyVal});
    tassert(8155707, "Failed to remove element from map", removed);

    auto elemSize =
        value::getApproximateSize(keyTag, keyVal) + value::getApproximateSize(outputTag, outputVal);
    memSize -= elemSize;
    state->setAt(static_cast<size_t>(AggAccumulatorNElems::kMemUsage),
                 value::TypeTags::NumberInt32,
                 value::bitcastFrom<int32_t>(memSize));

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

template <TopBottomSense sense>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAggRemovableTopBottomNFinalize(
    ArityType arity) {
    auto [stateOwned, stateTag, stateVal] = getFromStack(0);

    auto [state, multiMapTag, multiMapVal, n, memSize, memLimit] =
        accumulatorNState(stateTag, stateVal);
    tassert(8155727, "value should be of type MultiMap", multiMapTag == value::TypeTags::MultiMap);
    auto multiMap = value::getMultiMapView(multiMapVal);

    auto& values = multiMap->values();
    auto begin = values.begin();
    auto end = values.end();

    if constexpr (sense == TopBottomSense::kBottom) {
        // If this accumulator is removable there may be more than n elements in the map, so we must
        // skip elements that shouldn't be in the result.
        if (static_cast<size_t>(values.size()) > n) {
            std::advance(begin, values.size() - n);
        }
    }

    auto [resTag, resVal] = value::makeNewArray();
    value::ValueGuard resGuard{resTag, resVal};
    auto resArr = value::getArrayView(resVal);

    auto it = begin;
    for (size_t inserted = 0; inserted < n && it != end; ++inserted, ++it) {
        const auto& keyOutPair = *it;
        auto output = keyOutPair.second;
        auto [copyTag, copyVal] = value::copyValue(output.first, output.second);
        resArr->push_back(copyTag, copyVal);
    };

    resGuard.reset();
    return {true, resTag, resVal};
}
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggRemovableTopBottomNFinalize<(TopBottomSense)0>(ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinAggRemovableTopBottomNFinalize<(TopBottomSense)1>(ArityType arity);

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
