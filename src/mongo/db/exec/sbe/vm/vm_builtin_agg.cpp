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
value::TagValueMaybeOwned ByteCode::builtinAggDoubleDoubleSum(ArityType arity) {
    auto [_, fieldTag, fieldValue] = getFromStack(1);
    value::TagValueView field(fieldTag, fieldValue);

    // Move the incoming accumulator state from the stack. Given that we are now the owner of the
    // state we are free to do any in-place update as we see fit.
    auto accTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    // Initialize the accumulator.
    if (accTagVal.tag() == value::TypeTags::Nothing) {
        accTagVal = value::TagValueOwned::fromRaw(genericInitializeDoubleDoubleSumState());
    }

    tassert(
        5755317, "The result slot must be Array-typed", accTagVal.tag() == value::TypeTags::Array);
    value::Array* accumulator = value::getArrayView(accTagVal.value());

    if constexpr (merging) {
        aggMergeDoubleDoubleSumsImpl(accumulator, field.tag, field.value);
    } else {
        aggDoubleDoubleSumImpl(accumulator, field.tag, field.value);
    }

    // Transfer ownership to return value
    return accTagVal;
}
template value::TagValueMaybeOwned ByteCode::builtinAggDoubleDoubleSum<false>(ArityType arity);
template value::TagValueMaybeOwned ByteCode::builtinAggDoubleDoubleSum<true>(ArityType arity);

template <bool merging>
value::TagValueMaybeOwned ByteCode::builtinAggStdDev(ArityType arity) {
    auto [_, fieldTag, fieldValue] = getFromStack(1);
    auto field = value::TagValueView{fieldTag, fieldValue};

    // Initialize the accumulator.
    auto acc = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    // Initialize the accumulator if needed.
    if (acc.tag() == value::TypeTags::Nothing) {
        acc = value::TagValueOwned::fromRaw(value::makeNewArray());
        auto arr = value::getArrayView(acc.value());
        arr->reserve(AggStdDevValueElems::kSizeOfArray);

        // The order of the following three elements should match to 'AggStdDevValueElems'.
        arr->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
        arr->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0));
        arr->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0));
    }

    tassert(5755210, "The result slot must be Array-typed", acc.tag() == value::TypeTags::Array);
    auto accumulator = value::getArrayView(acc.value());

    if constexpr (merging) {
        aggMergeStdDevsImpl(accumulator, field.tag, field.value);
    } else {
        aggStdDevImpl(accumulator, field.tag, field.value);
    }

    // Transfer ownership to return value
    return acc;
}
template value::TagValueMaybeOwned ByteCode::builtinAggStdDev<true>(ArityType arity);
template value::TagValueMaybeOwned ByteCode::builtinAggStdDev<false>(ArityType arity);

value::TagValueMaybeOwned ByteCode::concatArraysAccumImpl(
    value::TagValueOwned accumulatorStateTagVal,
    value::TagValueOwned newArrayElements,
    int64_t newArrayElementsSize,
    int32_t sizeCap) {
    // The capped push accumulator holds a value of Nothing at first and gets initialized on
    // demand when the first value gets added. Once initialized, the state is a two-element array
    // containing the array and its size in bytes, which is necessary to enforce the memory cap.
    if (accumulatorStateTagVal.tag() == value::TypeTags::Nothing) {
        accumulatorStateTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());
        auto accumulatorState = value::getArrayView(accumulatorStateTagVal.value());

        // The order is important! The accumulated array should be at index
        // AggArrayWithSize::kValues, and the size should be at index
        // AggArrayWithSize::kSizeOfValues.
        accumulatorState->push_back(value::makeNewArray());
        accumulatorState->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    }
    tassert(7039514,
            "Expected array for set accumulator state",
            accumulatorStateTagVal.tag() == value::TypeTags::Array);

    auto accumulatorState = value::getArrayView(accumulatorStateTagVal.value());
    tassert(7039515,
            "Array accumulator with invalid length",
            accumulatorState->size() == static_cast<size_t>(AggArrayWithSize::kLast));

    auto accArrayTagVal = accumulatorState->getAt(static_cast<size_t>(AggArrayWithSize::kValues));
    tassert(7039518,
            "Expected array in accumulator state",
            accArrayTagVal.tag == value::TypeTags::Array);
    auto accArray = value::getArrayView(accArrayTagVal.value);

    auto accSize = accumulatorState->getAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues));
    tassert(7039516, "expected 64-bit int", accSize.tag == value::TypeTags::NumberInt64);
    const int64_t currentSize = value::bitcastTo<int64_t>(accSize.value);
    const int64_t updatedSize = currentSize + newArrayElementsSize;

    uassert(ErrorCodes::ExceededMemoryLimit,
            str::stream() << "Used too much memory for a single array. Memory limit: " << sizeCap
                          << ". Concatenating array of " << accArray->size() << " elements and "
                          << currentSize << " bytes with array of "
                          << (newArrayElements.tag() != value::TypeTags::Nothing
                                  ? value::getArraySize(newArrayElements.tag(),
                                                        newArrayElements.value())
                                  : 0)
                          << " elements and " << newArrayElementsSize << " bytes.",
            updatedSize < sizeCap);

    // We are still under the size limit. Set the new total size in the accumulator.
    accumulatorState->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
                            value::TypeTags::NumberInt64,
                            value::bitcastFrom<int64_t>(updatedSize));

    // Move each element from the 'newArrayElements' array to the accumulator array.
    if (newArrayElements.tag() != value::TypeTags::Nothing) {
        value::arrayForEach<true>(newArrayElements.tag(),
                                  newArrayElements.value(),
                                  [&](value::TypeTags tagNewElem, value::Value valNewElem) {
                                      accArray->push_back(tagNewElem, valNewElem);
                                  });
    }

    return accumulatorStateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggConcatArraysCapped(ArityType arity) {
    auto lhsAccumulatorStateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto rhsAccumulatorStateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));

    auto [_, tagSizeCap, valSizeCap] = getFromStack(2);
    auto sizeCap = value::TagValueView{tagSizeCap, valSizeCap};
    tassert(7039508,
            "'cap' parameter must be a 32-bit int",
            sizeCap.tag == value::TypeTags::NumberInt32);

    // Each accumulator should be a two-element array with the array value and the array value's
    // size as its elements. We pass the full LHS accumulator to 'concatArraysAccumImpl' as is, but
    // we need to destructure the RHS accumulator.
    tassert(7039512,
            "expected value of type 'Array'",
            rhsAccumulatorStateTagVal.tag() == value::TypeTags::Array);
    auto rhsAccumulatorState = value::getArrayView(rhsAccumulatorStateTagVal.value());

    tassert(7039527,
            "Capped array concatenation accumulator with invalid length",
            rhsAccumulatorState->size() == static_cast<size_t>(AggArrayWithSize::kLast));

    // Move ownership of the RHS array from the RHS accumulator to the local scope.
    auto newArrayElements = rhsAccumulatorState->swapAt(
        static_cast<size_t>(AggArrayWithSize::kValues), value::TypeTags::Null, 0);
    tassert(7039519,
            "expected value of type 'Array'",
            newArrayElements.tag() == value::TypeTags::Array);

    auto newArrayElementsSize =
        rhsAccumulatorState->getAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues));
    tassert(
        7039517, "expected 64-bit int", newArrayElementsSize.tag == value::TypeTags::NumberInt64);

    return concatArraysAccumImpl(std::move(lhsAccumulatorStateTagVal),
                                 std::move(newArrayElements),
                                 value::bitcastTo<int64_t>(newArrayElementsSize.value),
                                 value::bitcastTo<int32_t>(sizeCap.value));
}

value::TagValueMaybeOwned ByteCode::builtinAggSetUnion(ArityType arity) {
    auto accTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));

    if (accTagVal.tag() == value::TypeTags::Nothing) {
        // Initialize the accumulator.
        auto [tagAcc, valAcc] = value::makeNewArraySet();
        accTagVal = value::TagValueMaybeOwned::fromRaw(true, tagAcc, valAcc);
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }

    tassert(7039552, "accumulator must be owned", accTagVal.owned());
    tassert(7039553,
            "accumulator must be of type ArraySet",
            accTagVal.tag() == value::TypeTags::ArraySet);
    auto acc = value::getArraySetView(accTagVal.value());

    auto newSet = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));
    if (!value::isArray(newSet.tag())) {
        return {false, value::TypeTags::Nothing, 0};
    }

    value::arrayForEach(
        newSet.tag(), newSet.value(), [&](value::TypeTags elTag, value::Value elVal) {
            auto [copyTag, copyVal] = value::copyValue(elTag, elVal);
            acc->push_back(copyTag, copyVal);
        });

    return accTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggCollSetUnion(ArityType arity) {
    auto accTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));

    if (accTagVal.tag() == value::TypeTags::Nothing) {
        auto [_, collatorTag, collatorVal] = getFromStack(1);
        auto collatorTagVal = value::TagValueView{collatorTag, collatorVal};
        tassert(7690402,
                "Expected value of type 'collator'",
                collatorTagVal.tag == value::TypeTags::collator);
        CollatorInterface* collator = value::getCollatorView(collatorTagVal.value);

        // Initialize the accumulator.
        auto [tagAcc, valAcc] = value::makeNewArraySet(collator);
        accTagVal = value::TagValueMaybeOwned(true, tagAcc, valAcc);
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }

    tassert(7690403, "Accumulator must be owned", accTagVal.owned());
    tassert(7690404,
            "Accumulator must be of type ArraySet",
            accTagVal.tag() == value::TypeTags::ArraySet);
    auto acc = value::getArraySetView(accTagVal.value());

    auto newSet = value::TagValueOwned::fromRaw(moveOwnedFromStack(2));
    if (!value::isArray(newSet.tag())) {
        return {false, value::TypeTags::Nothing, 0};
    }

    value::arrayForEach(
        newSet.tag(), newSet.value(), [&](value::TypeTags elTag, value::Value elVal) {
            auto [copyTag, copyVal] = value::copyValue(elTag, elVal);
            acc->push_back(copyTag, copyVal);
        });

    return accTagVal;
}

namespace {
value::TagValueMaybeOwned builtinAggSetUnionCappedImpl(
    value::TagValueOwned lhsAccumulatorStateTagVal,
    value::TagValueOwned rhsAccumulatorStateTagVal,
    int32_t sizeCap,
    CollatorInterface* collator) {

    tassert(7039526,
            "Expected array for capped set union operand",
            rhsAccumulatorStateTagVal.tag() == value::TypeTags::Array);

    auto rhsAccumulatorState = value::getArrayView(rhsAccumulatorStateTagVal.value());
    tassert(7039528,
            "Capped set union operand with invalid length",
            rhsAccumulatorState->size() == static_cast<size_t>(AggArrayWithSize::kLast));

    value::TagValueOwned newSetMembers = rhsAccumulatorState->swapAt(
        static_cast<size_t>(AggArrayWithSize::kValues), value::TypeTags::Null, 0);
    tassert(7039525,
            "Expected ArraySet in capped set union operand",
            newSetMembers.tag() == value::TypeTags::ArraySet);

    return ByteCode::setUnionAccumImpl(
        std::move(lhsAccumulatorStateTagVal), std::move(newSetMembers), sizeCap, collator);
}
}  // namespace

value::TagValueMaybeOwned ByteCode::builtinAggSetUnionCapped(ArityType arity) {
    auto lhsAccumulatorState = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto rhsAccumulatorState = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));

    auto [_, tagSizeCap, valSizeCap] = getFromStack(2);
    value::TagValueView sizeCap(tagSizeCap, valSizeCap);
    tassert(7039509,
            "'cap' parameter must be a 32-bit int",
            sizeCap.tag == value::TypeTags::NumberInt32);

    return builtinAggSetUnionCappedImpl(std::move(lhsAccumulatorState),
                                        std::move(rhsAccumulatorState),
                                        value::bitcastTo<int32_t>(sizeCap.value),
                                        nullptr /*collator*/);
}

value::TagValueMaybeOwned ByteCode::builtinAggCollSetUnionCapped(ArityType arity) {
    auto lhsAccumulatorState = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto [_1, tagColl, valColl] = getFromStack(1);
    auto coll = value::TagValueView{tagColl, valColl};
    tassert(7039510, "expected value of type 'collator'", coll.tag == value::TypeTags::collator);

    auto rhsAccumulatorState = value::TagValueOwned::fromRaw(moveOwnedFromStack(2));

    auto [_2, tagSizeCap, valSizeCap] = getFromStack(3);
    auto sizeCap = value::TagValueView{tagSizeCap, valSizeCap};
    tassert(7039511,
            "'cap' parameter must be a 32-bit int",
            sizeCap.tag == value::TypeTags::NumberInt32);

    return builtinAggSetUnionCappedImpl(std::move(lhsAccumulatorState),
                                        std::move(rhsAccumulatorState),
                                        value::bitcastTo<int32_t>(sizeCap.value),
                                        value::getCollatorView(coll.value));
}

value::TagValueMaybeOwned ByteCode::builtinAggFirstNNeedsMoreInput(ArityType arity) {
    auto stateTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));
    uassert(7695200, "Unexpected accumulator state ownership", !stateTagVal.owned());

    auto state = value::getArrayView(stateTagVal.value());
    uassert(7695201,
            "The accumulator state should be an array",
            stateTagVal.tag() == value::TypeTags::Array);

    auto arrayTagVal = state->getAt(static_cast<size_t>(AggMultiElems::kInternalArr));
    uassert(7695202,
            "Internal array component is not of correct type",
            arrayTagVal.tag == value::TypeTags::Array);
    auto array = value::getArrayView(arrayTagVal.value);

    auto maxSize = state->getAt(static_cast<size_t>(AggMultiElems::kMaxSize));
    uassert(7695203,
            "MaxSize component should be a 64-bit integer",
            maxSize.tag == value::TypeTags::NumberInt64);

    bool needMoreInput = (array->size() < maxSize.value);
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(needMoreInput)};
}

namespace {
int32_t aggFirstN(value::Array* state,
                  value::Array* array,
                  size_t maxSize,
                  int32_t memUsage,
                  int32_t memLimit,
                  value::TagValueOwned field) {
    if (array->size() < maxSize) {
        memUsage = updateAndCheckMemUsage(
            state, memUsage, value::getApproximateSize(field.tag(), field.value()), memLimit);

        // add to array
        array->push_back(std::move(field));
    }
    return memUsage;
}
}  // namespace

value::TagValueMaybeOwned ByteCode::builtinAggFirstN(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.tag(), stateTagVal.value());

    auto [fieldTag, fieldVal] = moveOwnedFromStack(1);
    aggFirstN(state, array, maxSize, memUsage, memLimit, {fieldTag, fieldVal});

    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggFirstNMerge(ArityType arity) {
    auto mergeStateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));

    auto [mergeState,
          mergeArray,
          mergeStartIdx,
          mergeMaxSize,
          mergeMemUsage,
          mergeMemLimit,
          mergeIsGroupAccum] = getMultiAccState(mergeStateTagVal.tag(), mergeStateTagVal.value());
    auto [state, array, accStartIdx, accMaxSize, accMemUsage, accMemLimit, accIsGroupAccum] =
        getMultiAccState(stateTagVal.tag(), stateTagVal.value());
    uassert(7548604,
            "Two arrays to merge should have the same MaxSize component",
            accMaxSize == mergeMaxSize);

    for (size_t i = 0; i < array->size(); ++i) {
        if (mergeArray->size() == mergeMaxSize) {
            break;
        }

        value::TagValueOwned field = array->swapAt(i, value::TypeTags::Null, 0);
        mergeMemUsage = aggFirstN(
            mergeState, mergeArray, mergeMaxSize, mergeMemUsage, mergeMemLimit, std::move(field));
    }

    return mergeStateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggFirstNFinalize(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    uassert(7548605, "expected an array", stateTagVal.tag() == value::TypeTags::Array);
    auto state = value::getArrayView(stateTagVal.value());

    auto isGroupAccTagVal = state->getAt(static_cast<size_t>(AggMultiElems::kIsGroupAccum));
    auto isGroupAcc = value::bitcastTo<bool>(isGroupAccTagVal.value);

    if (isGroupAcc) {
        auto output = state->swapAt(
            static_cast<size_t>(AggMultiElems::kInternalArr), value::TypeTags::Null, 0);
        return output;
    } else {
        auto arrTagVal = state->getAt(static_cast<size_t>(AggMultiElems::kInternalArr));
        auto [outputTag, outputVal] = value::copyValue(arrTagVal.tag, arrTagVal.value);
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
                                    value::TagValueOwned field) {
    auto fieldView = field.view();
    if (array->size() < maxSize) {
        invariant(startIdx == 0);
        array->push_back(std::move(field));
    } else {
        invariant(array->size() == maxSize);
        auto oldField = array->swapAt(startIdx, std::move(field));
        memUsage -= value::getApproximateSize(oldField.tag(), oldField.value());
        startIdx = updateStartIdx(state, startIdx, maxSize);
    }
    memUsage = updateAndCheckMemUsage(
        state, memUsage, value::getApproximateSize(fieldView.tag, fieldView.value), memLimit);
    return {startIdx, memUsage};
}
}  // namespace

value::TagValueMaybeOwned ByteCode::builtinAggLastN(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.tag(), stateTagVal.value());

    auto fieldTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));
    aggLastN(state, array, startIdx, maxSize, memUsage, memLimit, std::move(fieldTagVal));

    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggLastNMerge(ArityType arity) {
    auto mergeStateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));

    auto [mergeState,
          mergeArray,
          mergeStartIdx,
          mergeMaxSize,
          mergeMemUsage,
          mergeMemLimit,
          mergeIsGroupAccum] = getMultiAccState(mergeStateTagVal.tag(), mergeStateTagVal.value());
    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.tag(), stateTagVal.value());
    uassert(7548703,
            "Two arrays to merge should have the same MaxSize component",
            maxSize == mergeMaxSize);
    tassert(11093706, "Array size cannot be greater than maxSize", array->size() <= maxSize);

    if (array->size() < maxSize) {
        // add values from accArr to mergeArray
        for (size_t i = 0; i < array->size(); ++i) {
            value::TagValueOwned field = array->swapAt(i, value::TypeTags::Null, 0);
            std::tie(mergeStartIdx, mergeMemUsage) = aggLastN(mergeState,
                                                              mergeArray,
                                                              mergeStartIdx,
                                                              mergeMaxSize,
                                                              mergeMemUsage,
                                                              mergeMemLimit,
                                                              std::move(field));
        }
        return mergeStateTagVal;
    } else {
        // return accArray since it contains last n values
        return stateTagVal;
    }
}

value::TagValueMaybeOwned ByteCode::builtinAggLastNFinalize(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto [state, arr, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.tag(), stateTagVal.value());
    if (startIdx == 0) {
        if (isGroupAccum) {
            auto out = state->swapAt(0, value::TypeTags::Null, 0);
            return out;
        } else {
            auto arrView = state->getAt(0);
            auto [outTag, outVal] = value::copyValue(arrView.tag, arrView.value);
            return {true, outTag, outVal};
        }
    }

    tassert(11093707, "Array size must be equal to maxSize", arr->size() == maxSize);
    auto outArrayTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto outArray = value::getArrayView(outArrayTagVal.value());
    outArray->reserve(maxSize);

    if (isGroupAccum) {
        for (size_t i = 0; i < maxSize; ++i) {
            auto srcIdx = (i + startIdx) % maxSize;
            auto elem = arr->swapAt(srcIdx, value::TypeTags::Null, 0);
            outArray->push_back(std::move(elem));
        }
    } else {
        for (size_t i = 0; i < maxSize; ++i) {
            auto srcIdx = (i + startIdx) % maxSize;
            auto elem = arr->getAt(srcIdx);
            auto [copyTag, copyVal] = value::copyValue(elem.tag, elem.value);
            outArray->push_back(copyTag, copyVal);
        }
    }
    return outArrayTagVal;
}

class ByteCode::TopBottomArgsDirect final : public ByteCode::TopBottomArgs {
public:
    TopBottomArgsDirect(TopBottomSense sense,
                        SortSpec* sortSpec,
                        value::TagValueMaybeOwned key,
                        value::TagValueMaybeOwned value)
        : TopBottomArgs(sense, sortSpec, false, false) {
        setDirectKeyArg(std::move(key));
        setDirectValueArg(std::move(value));
    }

    ~TopBottomArgsDirect() final = default;

    bool keySortsBeforeImpl(value::TagValueView item) final {
        MONGO_UNREACHABLE_TASSERT(8448721);
    }
    value::TagValueOwned getOwnedKeyImpl() final {
        MONGO_UNREACHABLE_TASSERT(8448722);
    }
    value::TagValueOwned getOwnedValueImpl() final {
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
            setDirectKeyArg(
                value::TagValueMaybeOwned::fromRaw(_bytecode->moveFromStack(_keysStartOffset)));
        }
        if (!_decomposedValue) {
            setDirectValueArg(
                value::TagValueMaybeOwned::fromRaw(_bytecode->moveFromStack(_valuesStartOffset)));
        }
    }

    ~TopBottomArgsFromStack() final = default;

protected:
    bool keySortsBeforeImpl(value::TagValueView item) final {
        tassert(8448700, "Expected item to be an Array", item.tag == value::TypeTags::Array);

        const SortPattern& sortPattern = _sortSpec->getSortPattern();
        tassert(8448701,
                "Expected numKeys to be equal to number of sort pattern parts",
                _numKeys == sortPattern.size());

        auto itemArray = value::getArrayView(item.value);
        tassert(8448702,
                "Expected size of item array to be equal to number of sort pattern parts",
                sortPattern.size() == itemArray->size());

        if (_sense == TopBottomSense::kTop) {
            for (size_t i = 0; i < sortPattern.size(); i++) {
                auto [_, keyTag, keyVal] = _bytecode->getFromStack(_keysStartOffset + i);
                auto keyTagVal = value::TagValueView{keyTag, keyVal};
                auto itemTagVal = itemArray->getAt(i);
                int32_t cmp = compare<TopBottomSense::kTop>(
                    keyTagVal.tag, keyTagVal.value, itemTagVal.tag, itemTagVal.value);

                if (cmp != 0) {
                    return sortPattern[i].isAscending ? cmp < 0 : cmp > 0;
                }
            }
        } else {
            for (size_t i = 0; i < sortPattern.size(); i++) {
                auto [_, keyTag, keyVal] = _bytecode->getFromStack(_keysStartOffset + i);
                auto keyTagVal = value::TagValueView{keyTag, keyVal};
                auto itemTagVal = itemArray->getAt(i);
                int32_t cmp = compare<TopBottomSense::kBottom>(
                    keyTagVal.tag, keyTagVal.value, itemTagVal.tag, itemTagVal.value);

                if (cmp != 0) {
                    return sortPattern[i].isAscending ? cmp < 0 : cmp > 0;
                }
            }
        }

        return false;
    }

    value::TagValueOwned getOwnedKeyImpl() final {
        auto keys = value::TagValueOwned::fromRaw(value::makeNewArray());
        auto keysArr = value::getArrayView(keys.value());

        for (size_t i = 0; i < _numKeys; ++i) {
            auto [keyTag, keyVal] = _bytecode->moveOwnedFromStack(_keysStartOffset + i);
            keysArr->push_back(keyTag, keyVal);
        }

        return keys;
    }

    value::TagValueOwned getOwnedValueImpl() final {
        auto values = value::TagValueOwned::fromRaw(value::makeNewArray());
        auto valuesArr = value::getArrayView(values.value());

        for (size_t i = 0; i < _numValues; ++i) {
            auto [valueTag, valueVal] = _bytecode->moveOwnedFromStack(_valuesStartOffset + i);
            valuesArr->push_back(valueTag, valueVal);
        }

        return values;
    }

private:
    ByteCode* _bytecode;
    size_t _keysStartOffset;
    size_t _numKeys;
    size_t _valuesStartOffset;
    size_t _numValues;
};  // class ByteCode::TopBottomArgsFromStack

template <TopBottomSense Sense, bool ValueIsDecomposedArray>
value::TagValueMaybeOwned ByteCode::builtinAggTopBottomNImpl(ArityType arity) {
    using Less =
        std::conditional_t<Sense == TopBottomSense::kTop, SortPatternLess, SortPatternGreater>;

    auto sortSpec = value::TagValueMaybeOwned::fromRaw(getFromStack(1));
    tassert(
        8448703, "Argument must be of sortSpec type", sortSpec.tag() == value::TypeTags::sortSpec);
    auto ss = value::getSortSpecView(sortSpec.value());

    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.tag(), stateTagVal.value());

    size_t numKeys = 1;
    bool keyIsDecomposed = false;
    auto [_, numKeysTag, numKeysVal] = getFromStack(2);
    auto numKeysTagVal = value::TagValueView{numKeysTag, numKeysVal};
    if (numKeysTagVal.tag == value::TypeTags::NumberInt32) {
        numKeys = static_cast<size_t>(value::bitcastTo<int32_t>(numKeysTagVal.value));
        keyIsDecomposed = true;
    } else {
        tassert(8448704,
                "Expected numKeys to be Null or Int32",
                numKeysTagVal.tag == value::TypeTags::Null);
    }

    constexpr size_t keysStartOffset = 3;
    const size_t valuesStartOffset = keysStartOffset + numKeys;
    const size_t numValues = ValueIsDecomposedArray ? arity - valuesStartOffset : 1;

    if (!keyIsDecomposed && !ValueIsDecomposedArray) {
        auto [keyOwned, keyTag, keyVal] = moveFromStack(keysStartOffset);
        auto [valueOwned, valueTag, valueVal] = moveFromStack(valuesStartOffset);

        TopBottomArgsDirect topBottomArgs{
            Sense, ss, {keyOwned, keyTag, keyVal}, {valueOwned, valueTag, valueVal}};

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

    return stateTagVal;
}

template <TopBottomSense Sense>
value::TagValueMaybeOwned ByteCode::builtinAggTopBottomN(ArityType arity) {
    return builtinAggTopBottomNImpl<Sense, false>(arity);
}
template value::TagValueMaybeOwned ByteCode::builtinAggTopBottomN<(TopBottomSense)0>(
    ArityType arity);
template value::TagValueMaybeOwned ByteCode::builtinAggTopBottomN<(TopBottomSense)1>(
    ArityType arity);

template <TopBottomSense Sense>
value::TagValueMaybeOwned ByteCode::builtinAggTopBottomNArray(ArityType arity) {
    return builtinAggTopBottomNImpl<Sense, true>(arity);
}
template value::TagValueMaybeOwned ByteCode::builtinAggTopBottomNArray<(TopBottomSense)0>(
    ArityType arity);
template value::TagValueMaybeOwned ByteCode::builtinAggTopBottomNArray<(TopBottomSense)1>(
    ArityType arity);

template <TopBottomSense Sense>
value::TagValueMaybeOwned ByteCode::builtinAggTopBottomNMerge(ArityType arity) {
    using OwnedTagValTuple = value::TagValueMaybeOwned;

    auto sortSpecTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(2));
    tassert(5807025,
            "Argument must be of sortSpec type",
            sortSpecTagVal.tag() == value::TypeTags::sortSpec);
    auto sortSpec = value::getSortSpecView(sortSpecTagVal.value());

    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));
    auto mergeStateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto [mergeState,
          mergeArray,
          mergeStartIx,
          mergeMaxSize,
          mergeMemUsage,
          mergeMemLimit,
          mergeIsGroupAccum] = getMultiAccState(mergeStateTagVal.tag(), mergeStateTagVal.value());
    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.tag(), stateTagVal.value());
    tassert(5807008,
            "Two arrays to merge should have the same MaxSize component",
            maxSize == mergeMaxSize);

    for (auto [pairTag, pairVal] : array->values()) {
        auto pair = value::getArrayView(pairVal);
        auto key = pair->swapAt(0, value::TypeTags::Null, 0);
        auto value = pair->swapAt(1, value::TypeTags::Null, 0);

        TopBottomArgsDirect topBottomArgs{Sense, sortSpec, std::move(key), std::move(value)};

        mergeMemUsage = aggTopBottomNAdd<Sense>(
            mergeState, mergeArray, mergeMaxSize, mergeMemUsage, mergeMemLimit, topBottomArgs);
    }

    return mergeStateTagVal;
}
template value::TagValueMaybeOwned ByteCode::builtinAggTopBottomNMerge<(TopBottomSense)0>(
    ArityType arity);
template value::TagValueMaybeOwned ByteCode::builtinAggTopBottomNMerge<(TopBottomSense)1>(
    ArityType arity);

value::TagValueMaybeOwned ByteCode::builtinAggTopBottomNFinalize(ArityType arity) {
    auto sortSpecTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(1));
    tassert(5807026,
            "Argument must be of sortSpec type",
            sortSpecTagVal.tag() == value::TypeTags::sortSpec);
    auto sortSpec = value::getSortSpecView(sortSpecTagVal.value());

    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.tag(), stateTagVal.value());

    auto outputArrayTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto outputArray = value::getArrayView(outputArrayTagVal.value());
    outputArray->reserve(array->size());

    // We always output result in the order of sort pattern in according to MQL semantics.
    auto less = SortPatternLess(sortSpec);
    auto keyLess = PairKeyComp(less);
    std::sort(array->values().begin(), array->values().end(), keyLess);
    for (size_t i = 0; i < array->size(); ++i) {
        auto pair = value::getArrayView(array->getAt(i).value);
        if (isGroupAccum) {
            auto out = pair->swapAt(1, value::TypeTags::Null, 0);
            outputArray->push_back(std::move(out));
        } else {
            auto outTagVal = pair->getAt(1);
            auto [copyTag, copyVal] = value::copyValue(outTagVal.tag, outTagVal.value);
            outputArray->push_back(copyTag, copyVal);
        }
    }

    return outputArrayTagVal;
}

namespace {
template <AccumulatorMinMaxN::MinMaxSense S>
int32_t aggMinMaxN(value::Array* state,
                   value::Array* array,
                   size_t maxSize,
                   int32_t memUsage,
                   int32_t memLimit,
                   const CollatorInterface* collator,
                   value::TagValueOwned field) {
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
            state, memUsage, value::getApproximateSize(field.tag(), field.value()), memLimit);

        array->push_back(std::move(field));
        std::push_heap(heap.begin(), heap.end(), comp);
    } else {
        uassert(7548800,
                "Heap should contain same number of elements as MaxSize",
                array->size() == maxSize);

        auto heapRoot = heap.front();
        if (comp(field.raw(), heapRoot)) {
            memUsage =
                updateAndCheckMemUsage(state,
                                       memUsage,
                                       -value::getApproximateSize(heapRoot.first, heapRoot.second) +
                                           value::getApproximateSize(field.tag(), field.value()),
                                       memLimit);
            std::pop_heap(heap.begin(), heap.end(), comp);
            array->setAt(maxSize - 1, std::move(field));
            std::push_heap(heap.begin(), heap.end(), comp);
        }
    }

    return memUsage;
}
}  // namespace

template <AccumulatorMinMaxN::MinMaxSense S>
value::TagValueMaybeOwned ByteCode::builtinAggMinMaxN(ArityType arity) {
    tassert(11080087, "Unexpected arity value", arity == 2 || arity == 3);

    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto fieldTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));
    if (value::isNullish(fieldTagVal.tag())) {
        return stateTagVal;
    }

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.tag(), stateTagVal.value());

    CollatorInterface* collator = nullptr;
    if (arity == 3) {
        auto collTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(2));
        uassert(
            7548802, "expected a collator argument", collTagVal.tag() == value::TypeTags::collator);
        collator = value::getCollatorView(collTagVal.value());
    }
    aggMinMaxN<S>(state, array, maxSize, memUsage, memLimit, collator, fieldTagVal.releaseToRaw());

    return stateTagVal;
}
template value::TagValueMaybeOwned ByteCode::builtinAggMinMaxN<(AccumulatorMinMax::Sense)-1>(
    ArityType arity);
template value::TagValueMaybeOwned ByteCode::builtinAggMinMaxN<(AccumulatorMinMax::Sense)1>(
    ArityType arity);

template <AccumulatorMinMaxN::MinMaxSense S>
value::TagValueMaybeOwned ByteCode::builtinAggMinMaxNMerge(ArityType arity) {
    tassert(11080086, "Unexpected arity value", arity == 2 || arity == 3);

    auto mergeStateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));

    auto [mergeState,
          mergeArray,
          mergeStartIdx,
          mergeMaxSize,
          mergeMemUsage,
          mergeMemLimit,
          mergeIsGroupAccum] = getMultiAccState(mergeStateTagVal.tag(), mergeStateTagVal.value());
    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.tag(), stateTagVal.value());
    uassert(7548801,
            "Two arrays to merge should have the same MaxSize component",
            maxSize == mergeMaxSize);

    CollatorInterface* collator = nullptr;
    if (arity == 3) {
        auto collTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(2));
        uassert(
            7548803, "expected a collator argument", collTagVal.tag() == value::TypeTags::collator);
        collator = value::getCollatorView(collTagVal.value());
    }

    for (size_t i = 0; i < array->size(); ++i) {
        value::TagValueOwned field = array->swapAt(i, value::TypeTags::Null, 0);
        mergeMemUsage = aggMinMaxN<S>(mergeState,
                                      mergeArray,
                                      mergeMaxSize,
                                      mergeMemUsage,
                                      mergeMemLimit,
                                      collator,
                                      std::move(field));
    }

    return mergeStateTagVal;
}
template value::TagValueMaybeOwned ByteCode::builtinAggMinMaxNMerge<(AccumulatorMinMax::Sense)-1>(
    ArityType arity);
template value::TagValueMaybeOwned ByteCode::builtinAggMinMaxNMerge<(AccumulatorMinMax::Sense)1>(
    ArityType arity);

template <AccumulatorMinMaxN::MinMaxSense S>
value::TagValueMaybeOwned ByteCode::builtinAggMinMaxNFinalize(ArityType arity) {
    tassert(11080085, "Unexpected arity value", arity == 2 || arity == 1);
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.tag(), stateTagVal.value());

    CollatorInterface* collator = nullptr;
    if (arity == 2) {
        auto collTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(1));
        uassert(
            7548804, "expected a collator argument", collTagVal.tag() == value::TypeTags::collator);
        collator = value::getCollatorView(collTagVal.value());
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
        auto arrayVal = state->swapAt(
            static_cast<size_t>(AggMultiElems::kInternalArr), value::TypeTags::Null, 0);
        return arrayVal;
    } else {
        auto arrTagVal = state->getAt(0);
        auto [outTag, outVal] = value::copyValue(arrTagVal.tag, arrTagVal.value);
        return {true, outTag, outVal};
    }
}
template value::TagValueMaybeOwned
ByteCode::builtinAggMinMaxNFinalize<(AccumulatorMinMax::Sense)-1>(ArityType arity);
template value::TagValueMaybeOwned ByteCode::builtinAggMinMaxNFinalize<(AccumulatorMinMax::Sense)1>(
    ArityType arity);

namespace {
std::tuple<value::Array*, value::TagValueView, bool, int64_t, int64_t, SortSpec*> rankState(
    value::TypeTags stateTag, value::Value stateVal) {
    uassert(
        7795500, "The accumulator state should be an array", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);

    uassert(7795501,
            "The accumulator state should have correct number of elements",
            state->size() == AggRankElems::kRankArraySize);

    auto lastValue = state->getAt(AggRankElems::kLastValue);
    auto lastValueIsNothingTagVal = state->getAt(AggRankElems::kLastValueIsNothing);
    auto lastRankTagVal = state->getAt(AggRankElems::kLastRank);
    auto sameRankCountTagVal = state->getAt(AggRankElems::kSameRankCount);
    auto sortSpecTagVal = state->getAt(AggRankElems::kSortSpec);

    uassert(8188900,
            "Last rank is nothing component should be a boolean",
            lastValueIsNothingTagVal.tag == value::TypeTags::Boolean);
    auto lastValueIsNothing = value::bitcastTo<bool>(lastValueIsNothingTagVal.value);

    uassert(7795502,
            "Last rank component should be a 64-bit integer",
            lastRankTagVal.tag == value::TypeTags::NumberInt64);
    auto lastRank = value::bitcastTo<int64_t>(lastRankTagVal.value);

    uassert(7795503,
            "Same rank component should be a 64-bit integer",
            sameRankCountTagVal.tag == value::TypeTags::NumberInt64);
    auto sameRankCount = value::bitcastTo<int64_t>(sameRankCountTagVal.value);

    uassert(8216800,
            "Sort spec component should be a sort spec object",
            sortSpecTagVal.tag == value::TypeTags::sortSpec);
    auto sortSpec = value::getSortSpecView(sortSpecTagVal.value);

    return {state, lastValue, lastValueIsNothing, lastRank, sameRankCount, sortSpec};
}

value::TagValueMaybeOwned builtinAggRankImpl(value::TypeTags stateTag,
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
        auto newStateTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());

        auto newState = value::getArrayView(newStateTagVal.value());
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
        return newStateTagVal;
    }

    value::TagValueOwned stateTagVal(stateTag, stateVal);
    auto [state, lastValue, lastValueIsNothing, lastRank, sameRankCount, sortSpec] =
        rankState(stateTagVal.tag(), stateTagVal.value());
    // Update the last value to Nothing before comparison if the flag is set.
    if (lastValueIsNothing) {
        lastValue.tag = value::TypeTags::Nothing;
        lastValue.value = 0;
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

    if (isSameValue(
            sortSpec, std::make_pair(valueTag, valueVal), {lastValue.tag, lastValue.value})) {
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
    return stateTagVal;
}  // builtinAggRankImpl
}  // namespace

value::TagValueMaybeOwned ByteCode::builtinAggRankColl(ArityType arity) {
    tassert(11080084, "Unexpected arity value", arity == 4);
    auto collatorTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(3));
    auto isAscendingTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(2));
    auto valueTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(1));
    auto [stateTag, stateVal] = moveOwnedFromStack(0);

    tassert(8216804,
            "Incorrect value type passed to aggRankColl for 'isAscending' parameter.",
            isAscendingTagVal.tag() == value::TypeTags::Boolean);
    auto isAscending = value::bitcastTo<bool>(isAscendingTagVal.value());

    tassert(7795504,
            "Incorrect value type passed to aggRankColl for collator.",
            collatorTagVal.tag() == value::TypeTags::collator);
    auto collator = value::getCollatorView(collatorTagVal.value());

    return builtinAggRankImpl(stateTag,
                              stateVal,
                              valueTagVal.owned(),
                              valueTagVal.tag(),
                              valueTagVal.value(),
                              isAscending,
                              false /* dense */,
                              collator);
}

value::TagValueMaybeOwned ByteCode::builtinAggDenseRank(ArityType arity) {
    tassert(11080083, "Unexpected arity value", arity == 3);
    auto isAscendingTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(2));
    auto valueTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(1));
    auto [stateTag, stateVal] = moveOwnedFromStack(0);

    tassert(8216805,
            "Incorrect value type passed to aggDenseRank for 'isAscending' parameter.",
            isAscendingTagVal.tag() == value::TypeTags::Boolean);
    auto isAscending = value::bitcastTo<bool>(isAscendingTagVal.value());

    return builtinAggRankImpl(stateTag,
                              stateVal,
                              valueTagVal.owned(),
                              valueTagVal.tag(),
                              valueTagVal.value(),
                              isAscending,
                              true /* dense */);
}

value::TagValueMaybeOwned ByteCode::builtinAggRank(ArityType arity) {
    tassert(11080082, "Unexpected arity value", arity == 3);
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

value::TagValueMaybeOwned ByteCode::builtinAggDenseRankColl(ArityType arity) {
    tassert(11080081, "Unexpected arity value", arity == 4);
    auto collatorTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(3));
    auto isAscendingTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(2));
    auto valueTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(1));
    auto [stateTag, stateVal] = moveOwnedFromStack(0);

    tassert(8216806,
            "Incorrect value type passed to aggDenseRankColl for 'isAscending' parameter.",
            isAscendingTagVal.tag() == value::TypeTags::Boolean);
    auto isAscending = value::bitcastTo<bool>(isAscendingTagVal.value());

    tassert(7795505,
            "Incorrect value type passed to aggDenseRankColl for collator.",
            collatorTagVal.tag() == value::TypeTags::collator);
    auto collator = value::getCollatorView(collatorTagVal.value());

    return builtinAggRankImpl(stateTag,
                              stateVal,
                              valueTagVal.owned(),
                              valueTagVal.tag(),
                              valueTagVal.value(),
                              isAscending,
                              true /* dense */,
                              collator);
}

value::TagValueMaybeOwned ByteCode::builtinAggRankFinalize(ArityType arity) {
    tassert(11080080, "Unexpected arity value", arity == 1);
    auto stateTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));
    auto [state, lastValue, lastValueIsNothing, lastRank, sameRankCount, sortSpec] =
        rankState(stateTagVal.tag(), stateTagVal.value());
    if (static_cast<int32_t>(lastRank) == lastRank) {
        return {true, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(lastRank)};
    }
    return {true, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(lastRank)};
}

value::TagValueMaybeOwned ByteCode::builtinAggExpMovingAvg(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto [fieldOwned, fieldTag, fieldVal] = getFromStack(1);
    if (!value::isNumber(fieldTag)) {
        return stateTagVal;
    }

    uassert(7821200, "State should be of array type", stateTagVal.tag() == value::TypeTags::Array);
    auto state = value::getArrayView(stateTagVal.value());
    uassert(7821201,
            "Unexpected state array size",
            state->size() == static_cast<size_t>(AggExpMovingAvgElems::kSizeOfArray));

    auto alphaTagVal = state->getAt(static_cast<size_t>(AggExpMovingAvgElems::kAlpha));
    uassert(
        7821202, "alpha is not of decimal type", alphaTagVal.tag == value::TypeTags::NumberDecimal);
    auto alpha = value::bitcastTo<Decimal128>(alphaTagVal.value);

    auto currentResultTagVal = state->getAt(static_cast<size_t>(AggExpMovingAvgElems::kResult));

    auto decimalVal = value::numericCast<Decimal128>(fieldTag, fieldVal);
    auto result = [&]() {
        if (currentResultTagVal.tag == value::TypeTags::Null) {
            // Accumulator result has not been yet initialised. We will now
            // set it to decimalVal
            return decimalVal;
        } else {
            uassert(7821203,
                    "currentResultTag is not of decimal type",
                    currentResultTagVal.tag == value::TypeTags::NumberDecimal);
            auto currentResult = value::bitcastTo<Decimal128>(currentResultTagVal.value);
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

    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggExpMovingAvgFinalize(ArityType arity) {
    auto stateTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));

    uassert(7821204, "State should be of array type", stateTagVal.tag() == value::TypeTags::Array);
    auto state = value::getArrayView(stateTagVal.value());

    auto [resultTag, resultVal] = state->getAt(static_cast<size_t>(AggExpMovingAvgElems::kResult));
    if (resultTag == value::TypeTags::Null) {
        return {false, value::TypeTags::Null, 0};
    }
    uassert(7821205, "Unexpected result type", resultTag == value::TypeTags::NumberDecimal);

    auto isDecimalTagVal = state->getAt(static_cast<size_t>(AggExpMovingAvgElems::kIsDecimal));
    uassert(7821206, "Unexpected isDecimal type", isDecimalTagVal.tag == value::TypeTags::Boolean);

    if (value::bitcastTo<bool>(isDecimalTagVal.value)) {
        std::tie(resultTag, resultVal) = value::copyValue(resultTag, resultVal);
        return {true, resultTag, resultVal};
    } else {
        auto result = value::bitcastTo<Decimal128>(resultVal).toDouble();
        return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
    }
}

namespace {
value::TagValueOwned initializeRemovableSumState() {
    auto stateTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto state = value::getArrayView(stateTagVal.value());
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
    auto [stateTag, stateVal] = stateTagVal.releaseToRaw();
    return {stateTag, stateVal};
}
}  // namespace

template <int sign>
value::TagValueMaybeOwned ByteCode::builtinAggRemovableSum(ArityType arity) {
    auto [_, fieldTag, fieldVal] = getFromStack(1);
    value::TagValueView field(fieldTag, fieldVal);
    auto state = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    // Initialize the accumulator.
    if (state.tag() == value::TypeTags::Nothing) {
        state = initializeRemovableSumState();
    }

    uassert(7795108, "state should be of array type", state.tag() == value::TypeTags::Array);
    auto stateArray = value::getArrayView(state.value());

    aggRemovableSumImpl<sign>(stateArray, field.tag, field.value);

    auto [tag, val] = state.releaseToRaw();
    return value::TagValueMaybeOwned(true, tag, val);
}
template value::TagValueMaybeOwned ByteCode::builtinAggRemovableSum<-1>(ArityType arity);
template value::TagValueMaybeOwned ByteCode::builtinAggRemovableSum<1>(ArityType arity);

value::TagValueMaybeOwned ByteCode::builtinAggRemovableSumFinalize(ArityType arity) {
    auto [_, stateTag, stateVal] = getFromStack(0);
    auto stateTagVal = value::TagValueView{stateTag, stateVal};

    uassert(7795109, "state should be of array type", stateTagVal.tag == value::TypeTags::Array);
    auto state = value::getArrayView(stateTagVal.value);
    return value::TagValueMaybeOwned::fromRaw(aggRemovableSumFinalizeImpl(state));
}

namespace {
// Initialize an array queue
std::pair<value::TypeTags, value::Value> arrayQueueInit() {
    auto arrayQueueTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto arrayQueue = value::getArrayView(arrayQueueTagVal.value());
    arrayQueue->reserve(static_cast<size_t>(ArrayQueueElems::kSizeOfArray));

    auto bufferTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());

    // Make the buffer has at least 1 capacity so that the start index will always be valid.
    auto buffer = value::getArrayView(bufferTagVal.value());
    buffer->push_back(value::TypeTags::Null, 0);

    auto [bufferTag, bufferVal] = bufferTagVal.releaseToRaw();
    arrayQueue->push_back(bufferTag, bufferVal);
    arrayQueue->push_back(value::TypeTags::NumberInt64, 0);  // kStartIdx
    arrayQueue->push_back(value::TypeTags::NumberInt64, 0);  // kQueueSize
    return arrayQueueTagVal.releaseToRaw();
}
}  // namespace

value::TagValueMaybeOwned ByteCode::builtinAggIntegralInit(ArityType arity) {
    auto unitTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));
    auto isNonRemovableTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(1));

    tassert(7996820,
            "Invalid unit type",
            unitTagVal.tag() == value::TypeTags::Null ||
                unitTagVal.tag() == value::TypeTags::NumberInt64);
    tassert(7996821,
            "Invalid isNonRemovable type",
            isNonRemovableTagVal.tag() == value::TypeTags::Boolean);

    auto stateTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());

    auto state = value::getArrayView(stateTagVal.value());
    state->reserve(static_cast<size_t>(AggIntegralElems::kMaxSizeOfArray));

    // AggIntegralElems::kInputQueue
    auto [inputQueueTag, inputQueueVal] = arrayQueueInit();
    state->push_back(inputQueueTag, inputQueueVal);

    // AggIntegralElems::kSortByQueue
    auto [sortByQueueTag, sortByQueueVal] = arrayQueueInit();
    state->push_back(sortByQueueTag, sortByQueueVal);

    // AggIntegralElems::kIntegral
    auto [integralTag, integralVal] = initializeRemovableSumState().releaseToRaw();
    state->push_back(integralTag, integralVal);

    // AggIntegralElems::kNanCount
    state->push_back(value::TypeTags::NumberInt64, 0);

    // AggIntegralElems::kUnitMillis
    state->push_back(unitTagVal.tag(), unitTagVal.value());

    // AggIntegralElems::kIsNonRemovable
    state->push_back(isNonRemovableTagVal.tag(), isNonRemovableTagVal.value());

    return stateTagVal;
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

    auto inputQueueTagVal = state->getAt(static_cast<size_t>(AggIntegralElems::kInputQueue));
    uassert(7821105,
            "InputQueue should be of array type",
            inputQueueTagVal.tag == value::TypeTags::Array);
    auto inputQueue = value::getArrayView(inputQueueTagVal.value);

    auto sortByQueueTagVal = state->getAt(static_cast<size_t>(AggIntegralElems::kSortByQueue));
    uassert(7821121,
            "SortByQueue should be of array type",
            sortByQueueTagVal.tag == value::TypeTags::Array);
    auto sortByQueue = value::getArrayView(sortByQueueTagVal.value);

    auto integralTagVal = state->getAt(static_cast<size_t>(AggIntegralElems::kIntegral));
    uassert(
        7821106, "Integral should be of array type", integralTagVal.tag == value::TypeTags::Array);
    auto integral = value::getArrayView(integralTagVal.value);

    auto nanCountTagVal = state->getAt(static_cast<size_t>(AggIntegralElems::kNanCount));
    uassert(7821107,
            "nanCount should be of NumberInt64 type",
            nanCountTagVal.tag == value::TypeTags::NumberInt64);
    auto nanCount = value::bitcastTo<int64_t>(nanCountTagVal.value);

    boost::optional<int64_t> unitMillis;
    auto unitMillisTagVal = state->getAt(static_cast<size_t>(AggIntegralElems::kUnitMillis));
    if (unitMillisTagVal.tag != value::TypeTags::Null) {
        uassert(7821108,
                "unitMillis should be of type NumberInt64",
                unitMillisTagVal.tag == value::TypeTags::NumberInt64);
        unitMillis = value::bitcastTo<int64_t>(unitMillisTagVal.value);
    }

    auto isNonRemovableTagVal =
        state->getAt(static_cast<size_t>(AggIntegralElems::kIsNonRemovable));
    uassert(7996800,
            "isNonRemovable should be of boolean type",
            isNonRemovableTagVal.tag == value::TypeTags::Boolean);
    auto isNonRemovable = value::bitcastTo<bool>(isNonRemovableTagVal.value);

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

value::TagValueMaybeOwned ByteCode::integralOfTwoPointsByTrapezoidalRule(
    value::TagValueView prevInput,
    value::TagValueView prevSortByVal,
    value::TagValueView newInput,
    value::TagValueView newSortByVal) {
    if (value::isNaN(prevInput.tag, prevInput.value) ||
        value::isNaN(prevSortByVal.tag, prevSortByVal.value) ||
        value::isNaN(newInput.tag, newInput.value) ||
        value::isNaN(newSortByVal.tag, newSortByVal.value)) {
        return {false, value::TypeTags::NumberInt64, 0};
    }

    if ((prevSortByVal.tag == value::TypeTags::Date && newSortByVal.tag == value::TypeTags::Date) ||
        (value::isNumber(prevSortByVal.tag) && value::isNumber(newSortByVal.tag))) {
        auto deltaTagVal = value::TagValueMaybeOwned::fromRaw(genericSub(
            newSortByVal.tag, newSortByVal.value, prevSortByVal.tag, prevSortByVal.value));

        auto sumYTagVal = value::TagValueMaybeOwned::fromRaw(
            genericAdd(newInput.tag, newInput.value, prevInput.tag, prevInput.value));

        auto integralTagVal = value::TagValueMaybeOwned::fromRaw(genericMul(
            sumYTagVal.tag(), sumYTagVal.value(), deltaTagVal.tag(), deltaTagVal.value()));

        auto result = genericDiv(integralTagVal.tag(),
                                 integralTagVal.value(),
                                 value::TypeTags::NumberInt64,
                                 value::bitcastFrom<int32_t>(2));
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
    auto arrayTagVal = arrayQueue->getAt(static_cast<size_t>(ArrayQueueElems::kArray));
    uassert(7821100, "Expected an array", arrayTagVal.tag == value::TypeTags::Array);
    auto array = value::getArrayView(arrayTagVal.value);
    auto size = array->size();
    uassert(7821116, "Expected non-empty array", size > 0);

    auto startIdxTagVal = arrayQueue->getAt(static_cast<size_t>(ArrayQueueElems::kStartIdx));
    uassert(
        7821101, "Expected NumberInt64 type", startIdxTagVal.tag == value::TypeTags::NumberInt64);
    auto startIdx = value::bitcastTo<size_t>(startIdxTagVal.value);
    uassert(7821114,
            str::stream() << "Invalid startIdx " << startIdx << " with array size " << size,
            startIdx < size);

    auto queueSizeTagVal = arrayQueue->getAt(static_cast<size_t>(ArrayQueueElems::kQueueSize));
    uassert(
        7821102, "Expected NumberInt64 type", queueSizeTagVal.tag == value::TypeTags::NumberInt64);
    auto queueSize = value::bitcastTo<size_t>(queueSizeTagVal.value);
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
    value::TagValueOwned tagVal(tag, val);
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
                value::TagValueOwned mov = array->swapAt(from, value::TypeTags::Null, 0);
                array->setAt(to, std::move(mov));
            }
            startIdx += extend;
        }
        cap = newCap;
    }

    auto endIdx = (startIdx + queueSize) % cap;
    auto [tagFinal, valFinal] = tagVal.releaseToRaw();
    array->setAt(endIdx, tagFinal, valFinal);
    updateArrayQueueState(arrayQueue, startIdx, queueSize + 1);
}

/* Pops an element {tag, value} from the queue and returns it */
value::TagValueOwned arrayQueuePop(value::Array* arrayQueue) {
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

value::TagValueView arrayQueueFront(value::Array* arrayQueue) {
    auto [array, startIdx, queueSize] = getArrayQueueState(arrayQueue);
    if (queueSize == 0) {
        return {value::TypeTags::Nothing, 0};
    }
    return array->getAt(startIdx);
}

value::TagValueView arrayQueueBack(value::Array* arrayQueue) {
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
value::TagValueOwned arrayQueueFrontN(value::Array* arrayQueue, size_t n) {
    auto [array, startIdx, queueSize] = getArrayQueueState(arrayQueue);

    auto resultArrayTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto resultArray = value::getArrayView(resultArrayTagVal.value());
    auto countElem = std::min(n, queueSize);
    resultArray->reserve(countElem);

    auto cap = array->size();
    for (size_t i = 0; i < countElem; ++i) {
        auto idx = (startIdx + i) % cap;

        auto tagVal = array->getAt(idx);
        auto [copyTag, copyVal] = value::copyValue(tagVal.tag, tagVal.value);
        resultArray->push_back(copyTag, copyVal);
    }

    return resultArrayTagVal;
}

// Returns a value::Array containing N elements at the back of the queue.
// If the queue contains less than N elements, returns all the elements
value::TagValueOwned arrayQueueBackN(value::Array* arrayQueue, size_t n) {
    auto [array, startIdx, queueSize] = getArrayQueueState(arrayQueue);

    auto arrTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto arr = value::getArrayView(arrTagVal.value());
    arr->reserve(std::min(n, queueSize));

    auto cap = array->size();
    auto skip = queueSize > n ? queueSize - n : 0;
    auto elemCount = queueSize > n ? n : queueSize;
    startIdx = (startIdx + skip) % cap;

    for (size_t i = 0; i < elemCount; ++i) {
        auto idx = (startIdx + i) % cap;

        auto tagVal = array->getAt(idx);
        auto [copyTag, copyVal] = value::copyValue(tagVal.tag, tagVal.value);
        arr->push_back(copyTag, copyVal);
    }

    return arrTagVal;
}
}  // namespace

value::TagValueMaybeOwned ByteCode::builtinAggIntegralAdd(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto inputTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));
    auto sortByTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(2));

    auto [state, inputQueue, sortByQueue, integral, nanCount, unitMillis, isNonRemovable] =
        getIntegralState(stateTagVal.tag(), stateTagVal.value());

    assertTypesForIntegeral(inputTagVal.tag(), sortByTagVal.tag(), unitMillis);

    if (value::isNaN(inputTagVal.tag(), inputTagVal.value()) ||
        value::isNaN(sortByTagVal.tag(), sortByTagVal.value())) {
        nanCount++;
        updateNaNCount(state, nanCount);
    }

    auto queueSize = arrayQueueSize(inputQueue);
    uassert(7821119, "Queue sizes should match", queueSize == arrayQueueSize(sortByQueue));
    if (queueSize > 0) {
        auto inputBack = arrayQueueBack(inputQueue);
        auto sortByBack = arrayQueueBack(sortByQueue);

        auto integralDelta =
            integralOfTwoPointsByTrapezoidalRule(inputBack,
                                                 sortByBack,
                                                 {inputTagVal.tag(), inputTagVal.value()},
                                                 {sortByTagVal.tag(), sortByTagVal.value()});
        aggRemovableSumImpl<1>(integral, integralDelta.tag(), integralDelta.value());
    }

    if (isNonRemovable) {
        arrayQueuePop(inputQueue);
        arrayQueuePop(sortByQueue);
    }

    auto [inputTag, inputVal] = inputTagVal.releaseToRaw();
    arrayQueuePush(inputQueue, inputTag, inputVal);

    auto [sortByTag, sortByVal] = sortByTagVal.releaseToRaw();
    arrayQueuePush(sortByQueue, sortByTag, sortByVal);

    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggIntegralRemove(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto inputTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));
    auto sortByTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(2));

    auto [state, inputQueue, sortByQueue, integral, nanCount, unitMillis, isNonRemovable] =
        getIntegralState(stateTagVal.tag(), stateTagVal.value());
    uassert(7996801, "Expected integral window to be removable", !isNonRemovable);

    assertTypesForIntegeral(inputTagVal.tag(), sortByTagVal.tag(), unitMillis);

    // verify that the input and sortby value to be removed are the first elements of the queues
    auto frontInput = arrayQueuePop(inputQueue);
    auto [cmpTag, cmpVal] = value::compareValue(
        frontInput.tag(), frontInput.value(), inputTagVal.tag(), inputTagVal.value());
    uassert(7821113,
            "Attempted to remove unexpected input value",
            cmpTag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(cmpVal) == 0);

    value::TagValueOwned frontSortBy = arrayQueuePop(sortByQueue);
    std::tie(cmpTag, cmpVal) = value::compareValue(
        frontSortBy.tag(), frontSortBy.value(), sortByTagVal.tag(), sortByTagVal.value());
    uassert(7821117,
            "Attempted to remove unexpected sortby value",
            cmpTag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(cmpVal) == 0);

    if (value::isNaN(inputTagVal.tag(), inputTagVal.value()) ||
        value::isNaN(sortByTagVal.tag(), sortByTagVal.value())) {
        nanCount--;
        updateNaNCount(state, nanCount);
    }

    auto queueSize = arrayQueueSize(inputQueue);
    uassert(7821120, "Queue sizes should match", queueSize == arrayQueueSize(sortByQueue));
    if (queueSize > 0) {
        auto inputPair = arrayQueueFront(inputQueue);
        auto sortByPair = arrayQueueFront(sortByQueue);

        auto integralDelta =
            integralOfTwoPointsByTrapezoidalRule({inputTagVal.tag(), inputTagVal.value()},
                                                 {sortByTagVal.tag(), sortByTagVal.value()},
                                                 inputPair,
                                                 sortByPair);
        aggRemovableSumImpl<-1>(integral, integralDelta.tag(), integralDelta.value());
    }

    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggIntegralFinalize(ArityType arity) {
    auto stateTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));

    auto [state, inputQueue, sortByQueue, integral, nanCount, unitMillis, isNonRemovable] =
        getIntegralState(stateTagVal.tag(), stateTagVal.value());

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

    auto resultTagVal = value::TagValueMaybeOwned::fromRaw(aggRemovableSumFinalizeImpl(integral));
    if (unitMillis) {
        auto [divResultOwned, divResultTag, divResultVal] =
            genericDiv(resultTagVal.tag(),
                       resultTagVal.value(),
                       value::TypeTags::NumberInt64,
                       value::bitcastFrom<int64_t>(*unitMillis))
                .releaseToRaw();
        return {divResultOwned, divResultTag, divResultVal};
    } else {
        return resultTagVal;
    }
}

value::TagValueMaybeOwned ByteCode::builtinAggDerivativeFinalize(ArityType arity) {
    auto unitMillisTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));
    auto inputFirstTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(1));
    auto sortByFirstTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(2));
    auto inputLastTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(3));
    auto sortByLastTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(4));

    if (sortByFirstTagVal.tag() == value::TypeTags::Nothing ||
        sortByLastTagVal.tag() == value::TypeTags::Nothing) {
        return {false, value::TypeTags::Null, 0};
    }

    boost::optional<int64_t> unitMillis;
    if (unitMillisTagVal.tag() != value::TypeTags::Null) {
        uassert(7993408,
                "unitMillis should be of type NumberInt64",
                unitMillisTagVal.tag() == value::TypeTags::NumberInt64);
        unitMillis = value::bitcastTo<int64_t>(unitMillisTagVal.value());
    }

    if (unitMillis) {
        uassert(7993409,
                "Unexpected type for sortBy value",
                sortByFirstTagVal.tag() == value::TypeTags::Date &&
                    sortByLastTagVal.tag() == value::TypeTags::Date);
    } else {
        uassert(7993410,
                "Unexpected type for sortBy value",
                value::isNumber(sortByFirstTagVal.tag()) &&
                    value::isNumber(sortByLastTagVal.tag()));
    }

    auto runTagVal = value::TagValueMaybeOwned::fromRaw(genericSub(sortByLastTagVal.tag(),
                                                                   sortByLastTagVal.value(),
                                                                   sortByFirstTagVal.tag(),
                                                                   sortByFirstTagVal.value()));

    auto riseTagVal = value::TagValueMaybeOwned::fromRaw(genericSub(inputLastTagVal.tag(),
                                                                    inputLastTagVal.value(),
                                                                    inputFirstTagVal.tag(),
                                                                    inputFirstTagVal.value()));

    uassert(7821012, "Input delta should be numeric", value::isNumber(riseTagVal.tag()));

    // Return null if the sortBy delta is zero
    if (runTagVal.tag() == value::TypeTags::NumberDecimal) {
        if (numericCast<Decimal128>(runTagVal.tag(), runTagVal.value()).isZero()) {
            return {false, value::TypeTags::Null, 0};
        }
    } else {
        if (numericCast<double>(runTagVal.tag(), runTagVal.value()) == 0) {
            return {false, value::TypeTags::Null, 0};
        }
    }

    auto divTagVal =
        genericDiv(riseTagVal.tag(), riseTagVal.value(), runTagVal.tag(), runTagVal.value());

    if (unitMillis) {
        auto [mulOwned, mulTag, mulVal] = genericMul(divTagVal.tag(),
                                                     divTagVal.value(),
                                                     value::TypeTags::NumberInt64,
                                                     value::bitcastFrom<int64_t>(*unitMillis));
        return {mulOwned, mulTag, mulVal};
    } else {
        return divTagVal;
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

    auto sumXTagVal = state->getAt(static_cast<size_t>(AggCovarianceElems::kSumX));
    tassert(7820802, "SumX component should be an array", sumXTagVal.tag == value::TypeTags::Array);
    auto sumX = value::getArrayView(sumXTagVal.value);

    auto sumYTagVal = state->getAt(static_cast<size_t>(AggCovarianceElems::kSumY));
    tassert(7820803, "SumY component should be an array", sumYTagVal.tag == value::TypeTags::Array);
    auto sumY = value::getArrayView(sumYTagVal.value);

    auto cXYTagVal = state->getAt(static_cast<size_t>(AggCovarianceElems::kCXY));
    tassert(7820804, "CXY component should be an array", cXYTagVal.tag == value::TypeTags::Array);
    auto cXY = value::getArrayView(cXYTagVal.value);

    auto countTagVal = state->getAt(static_cast<size_t>(AggCovarianceElems::kCount));
    tassert(7820805,
            "Count component should be a 64-bit integer",
            countTagVal.tag == value::TypeTags::NumberInt64);
    auto count = value::bitcastTo<int64_t>(countTagVal.value);

    return {state, sumX, sumY, cXY, count};
}

value::TagValueMaybeOwned covarianceCheckNonFinite(value::TypeTags xTag,
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
            auto doubleTagVal = value::TagValueMaybeOwned::fromRaw(
                genericNumConvert(tag, val, value::TypeTags::NumberDouble));
            auto value = value::bitcastTo<double>(doubleTagVal.value());
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

value::TagValueMaybeOwned ByteCode::aggRemovableAvgFinalizeImpl(value::Array* sumState,
                                                                int64_t count) {
    if (count == 0) {
        return {false, sbe::value::TypeTags::Null, 0};
    }
    auto sumTagVal = value::TagValueMaybeOwned::fromRaw(aggRemovableSumFinalizeImpl(sumState));

    if (sumTagVal.tag() == value::TypeTags::NumberInt32) {
        auto sum = static_cast<double>(value::bitcastTo<int>(sumTagVal.value()));
        auto avg = sum / static_cast<double>(count);
        return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(avg)};
    } else if (sumTagVal.tag() == value::TypeTags::NumberInt64) {
        auto sum = static_cast<double>(value::bitcastTo<long long>(sumTagVal.value()));
        auto avg = sum / static_cast<double>(count);
        return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(avg)};
    } else if (sumTagVal.tag() == value::TypeTags::NumberDouble) {
        auto sum = value::bitcastTo<double>(sumTagVal.value());
        if (std::isnan(sum) || std::isinf(sum)) {
            return {false, sumTagVal.tag(), sumTagVal.value()};
        }
        auto avg = sum / static_cast<double>(count);
        return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(avg)};
    } else if (sumTagVal.tag() == value::TypeTags::NumberDecimal) {
        auto sum = value::bitcastTo<Decimal128>(sumTagVal.value());
        if (sum.isNaN() || sum.isInfinite()) {
            return sumTagVal;
        }
        auto avg = sum.divide(Decimal128(count));
        auto [avgTag, avgVal] = value::makeCopyDecimal(avg);
        return {true, avgTag, avgVal};
    } else {
        MONGO_UNREACHABLE_TASSERT(11122938);
    }
}

value::TagValueMaybeOwned ByteCode::builtinAggCovarianceAdd(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto xTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(1));
    auto yTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(2));

    // Initialize the accumulator.
    if (stateTagVal.tag() == value::TypeTags::Nothing) {
        stateTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());
        auto state = value::getArrayView(stateTagVal.value());
        state->reserve(static_cast<size_t>(AggCovarianceElems::kSizeOfArray));

        auto [sumXStateTag, sumXStateVal] = initializeRemovableSumState().releaseToRaw();
        state->push_back(sumXStateTag, sumXStateVal);  // kSumX
        auto [sumYStateTag, sumYStateVal] = initializeRemovableSumState().releaseToRaw();
        state->push_back(sumYStateTag, sumYStateVal);  // kSumY
        auto [cXYStateTag, cXYStateVal] = initializeRemovableSumState().releaseToRaw();
        state->push_back(cXYStateTag, cXYStateVal);                                      // kCXY
        state->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));  // kCount
    }

    if (!value::isNumber(xTagVal.tag()) || !value::isNumber(yTagVal.tag())) {
        return stateTagVal;
    }

    auto [state, sumXState, sumYState, cXYState, count] =
        covarianceState(stateTagVal.tag(), stateTagVal.value());

    auto [nonFiniteOwned, nonFiniteTag, nonFiniteVal] =
        covarianceCheckNonFinite(xTagVal.tag(), xTagVal.value(), yTagVal.tag(), yTagVal.value())
            .releaseToRaw();
    if (nonFiniteTag != value::TypeTags::Nothing) {
        value::ValueGuard nonFiniteGuard{nonFiniteOwned, nonFiniteTag, nonFiniteVal};
        aggRemovableSumImpl<1>(cXYState, nonFiniteTag, nonFiniteVal);
        return stateTagVal;
    }

    auto meanXTagVal = aggRemovableAvgFinalizeImpl(sumXState, count);
    auto deltaXTagVal = value::TagValueMaybeOwned::fromRaw(
        genericSub(xTagVal.tag(), xTagVal.value(), meanXTagVal.tag(), meanXTagVal.value()));
    aggRemovableSumImpl<1>(sumXState, xTagVal.tag(), xTagVal.value());

    aggRemovableSumImpl<1>(sumYState, yTagVal.tag(), yTagVal.value());
    auto meanYTagVal = aggRemovableAvgFinalizeImpl(sumYState, count + 1);
    auto deltaYTagVal = value::TagValueMaybeOwned::fromRaw(
        genericSub(yTagVal.tag(), yTagVal.value(), meanYTagVal.tag(), meanYTagVal.value()));

    auto deltaCXYTagVal = value::TagValueMaybeOwned::fromRaw(genericMul(
        deltaXTagVal.tag(), deltaXTagVal.value(), deltaYTagVal.tag(), deltaYTagVal.value()));
    aggRemovableSumImpl<1>(cXYState, deltaCXYTagVal.tag(), deltaCXYTagVal.value());

    state->setAt(static_cast<size_t>(AggCovarianceElems::kCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(count + 1));

    return stateTagVal;
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
        MONGO_UNREACHABLE_TASSERT(11122939);
    }
}

value::TagValueMaybeOwned ByteCode::builtinAggCovarianceRemove(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto xTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(1));
    auto yTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(2));

    if (!value::isNumber(xTagVal.tag()) || !value::isNumber(yTagVal.tag())) {
        return stateTagVal;
    }

    auto [state, sumXState, sumYState, cXYState, count] =
        covarianceState(stateTagVal.tag(), stateTagVal.value());

    auto nonFiniteTagVal =
        covarianceCheckNonFinite(xTagVal.tag(), xTagVal.value(), yTagVal.tag(), yTagVal.value());
    if (nonFiniteTagVal.tag() != value::TypeTags::Nothing) {
        aggRemovableSumImpl<-1>(cXYState, nonFiniteTagVal.tag(), nonFiniteTagVal.value());
        return stateTagVal;
    }

    tassert(7820806, "Can't remove from an empty covariance window", count > 0);
    if (count == 1) {
        state->setAt(static_cast<size_t>(AggCovarianceElems::kCount),
                     value::TypeTags::NumberInt64,
                     value::bitcastFrom<int64_t>(0));
        aggRemovableSumReset(sumXState);
        aggRemovableSumReset(sumYState);
        aggRemovableSumReset(cXYState);
        return stateTagVal;
    }

    aggRemovableSumImpl<-1>(sumXState, xTagVal.tag(), xTagVal.value());
    auto meanXTagVal = aggRemovableAvgFinalizeImpl(sumXState, count - 1);
    auto deltaXTagVal = value::TagValueMaybeOwned::fromRaw(
        genericSub(xTagVal.tag(), xTagVal.value(), meanXTagVal.tag(), meanXTagVal.value()));

    auto meanYTagVal = value::TagValueMaybeOwned::fromRaw(
        aggRemovableAvgFinalizeImpl(sumYState, count).releaseToRaw());
    auto deltaYTagVal = value::TagValueMaybeOwned::fromRaw(
        genericSub(yTagVal.tag(), yTagVal.value(), meanYTagVal.tag(), meanYTagVal.value()));
    aggRemovableSumImpl<-1>(sumYState, yTagVal.tag(), yTagVal.value());

    auto deltaCXYTagVal = value::TagValueMaybeOwned::fromRaw(genericMul(
        deltaXTagVal.tag(), deltaXTagVal.value(), deltaYTagVal.tag(), deltaYTagVal.value()));
    aggRemovableSumImpl<-1>(cXYState, deltaCXYTagVal.tag(), deltaCXYTagVal.value());

    state->setAt(static_cast<size_t>(AggCovarianceElems::kCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(count - 1));

    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggCovarianceFinalize(ArityType arity, bool isSamp) {
    auto stateTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));
    auto [state, sumXState, sumYState, cXYState, count] =
        covarianceState(stateTagVal.tag(), stateTagVal.value());

    if (count == 1 && !isSamp) {
        return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0)};
    }

    double adjustedCount = (isSamp ? count - 1 : count);
    if (adjustedCount <= 0) {
        return {false, value::TypeTags::Null, 0};
    }

    auto cXYTagVal = value::TagValueMaybeOwned::fromRaw(aggRemovableSumFinalizeImpl(cXYState));
    return genericDiv(cXYTagVal.tag(),
                      cXYTagVal.value(),
                      value::TypeTags::NumberDouble,
                      value::bitcastFrom<double>(adjustedCount));
}

value::TagValueMaybeOwned ByteCode::builtinAggCovarianceSampFinalize(ArityType arity) {
    return builtinAggCovarianceFinalize(arity, true /* isSamp */);
}

value::TagValueMaybeOwned ByteCode::builtinAggCovariancePopFinalize(ArityType arity) {
    return builtinAggCovarianceFinalize(arity, false /* isSamp */);
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovablePushAdd(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    if (stateTagVal.tag() == value::TypeTags::Nothing) {
        stateTagVal = value::TagValueOwned::fromRaw(arrayQueueInit());
    }

    auto inputTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));
    if (inputTagVal.tag() == value::TypeTags::Nothing) {
        return stateTagVal;
    }

    uassert(7993100, "State should be of array type", stateTagVal.tag() == value::TypeTags::Array);
    auto state = value::getArrayView(stateTagVal.value());

    auto [inputTag, inputVal] = inputTagVal.releaseToRaw();  // Release ownership first!
    arrayQueuePush(state, inputTag, inputVal);

    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovablePushRemove(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto inputTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));
    if (inputTagVal.tag() == value::TypeTags::Nothing) {
        return stateTagVal;
    }

    uassert(7993101, "State should be of array type", stateTagVal.tag() == value::TypeTags::Array);
    auto state = value::getArrayView(stateTagVal.value());
    auto poppedVal = arrayQueuePop(state);
    return stateTagVal;
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
    auto accArrTagVal = stateArr->getAt(static_cast<size_t>(AggArrayWithSize::kValues));
    tassert(
        9476003, "accumulator should be of type Array", accArrTagVal.tag == value::TypeTags::Array);
    auto accArr = value::getArrayView(accArrTagVal.value);

    auto accArrSizeTagVal = stateArr->getAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues));
    tassert(9476004,
            "accumulator size should be of type NumberInt32",
            accArrSizeTagVal.tag == value::TypeTags::NumberInt32);

    return {stateArr, accArr, value::bitcastTo<int32_t>(accArrSizeTagVal.value)};
}

value::TagValueMaybeOwned pushConcatArraysCommonFinalize(value::Array* state) {
    auto [queueBuffer, startIdx, queueSize] = getArrayQueueState(state);

    auto resultTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto result = value::getArrayView(resultTagVal.value());
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
    return resultTagVal;
}
}  // namespace

value::TagValueMaybeOwned ByteCode::builtinAggRemovablePushFinalize(ArityType arity) {
    auto stateTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));
    uassert(7993102, "State should be of array type", stateTagVal.tag() == value::TypeTags::Array);
    auto state = value::getArrayView(stateTagVal.value());

    return pushConcatArraysCommonFinalize(state);
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableConcatArraysInit(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto arr = value::getArrayView(stateTagVal.value());

    // This will be the structure where the accumulated values are stored.
    auto [accArrTag, accArrVal] = arrayQueueInit();

    // The order is important! The accumulated array should be at index
    // AggArrayWithSize::kValues, and the size (bytes) should be at index
    // AggArrayWithSize::kSizeOfValues.
    arr->push_back(accArrTag, accArrVal);
    arr->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableConcatArraysAdd(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto newElTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));

    // If the field resolves to Nothing (e.g. if it is missing in the document), then we want to
    // leave the current state as is.
    if (newElTagVal.tag() == value::TypeTags::Nothing) {
        return stateTagVal;
    }

    auto sizeCapTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(2));
    tassert(9476000,
            "The size cap must be of type NumberInt32",
            sizeCapTagVal.tag() == value::TypeTags::NumberInt32);
    auto capSize = value::bitcastTo<int32_t>(sizeCapTagVal.value());
    auto [stateArr, accArr, accArrSize] = concatArraysState(stateTagVal.tag(), stateTagVal.value());

    // Note the importance of templating 'arrayForEach' on 'true' here. The input to $concatArrays
    // is an array. In order to avoid leaking the memory associated with each element of the array,
    // we create copies of each element to store in the accumulator (via templating on 'true'). An
    // example where we might otherwise leak memory is if we get the input off the stack as type
    // 'bsonArray'. Iterating over a 'bsonArray' results in pointers into the underlying BSON. Thus,
    // (without passing 'true') calling 'arrayQueuePush' below would insert elements that are
    // pointers to memory that will be destroyed with 'newElGuard' above, which is the source of a
    // memory leak.
    value::arrayForEach<true>(
        newElTagVal.tag(),
        newElTagVal.value(),
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
    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableConcatArraysRemove(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto elTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));
    auto [stateArr, accArr, accArrSize] = concatArraysState(stateTagVal.tag(), stateTagVal.value());

    // If the field resolves to Nothing (e.g. if it is missing in the document), then we want to
    // leave the current state as is.
    if (elTagVal.tag() == value::TypeTags::Nothing) {
        return stateTagVal;
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
        elTagVal.tag(),
        elTagVal.value(),
        // We do an initialization capture because clang fails when trying to
        // capture a structured binding in a lambda expression.
        [&accArr = accArr, &accArrSize = accArrSize](value::TypeTags elemBeingRemovedTag,
                                                     value::Value elemBeingRemovedVal) {
            value::TagValueOwned elemBeingRemoved(elemBeingRemovedTag, elemBeingRemovedVal);
            auto elemSize =
                value::getApproximateSize(elemBeingRemoved.tag(), elemBeingRemoved.value());
            tassert(11093708,
                    "Size of element is larger than size of accumulator array",
                    elemSize <= accArrSize);

            // Ensure that there is a value to remove from the window.
            tassert(9476005, "Trying to remove from an empty window", accArr->size() > 0);

            if (kDebugBuild) {
                // Ensure the value we will remove is in fact the value we have been told to remove.
                // This check is expensive on workloads with a lot of removals, and becomes even
                // more expensive with arbitrarily long arrays.
                auto frontElem = arrayQueueFront(accArr);
                auto [cmpTag, cmpVal] = value::compareValue(frontElem.tag,
                                                            frontElem.value,
                                                            elemBeingRemoved.tag(),
                                                            elemBeingRemoved.value());
                tassert(11093709,
                        "Can't remove a value that is not at the front of the window",
                        cmpTag == value::TypeTags::NumberInt32 &&
                            value::bitcastTo<int32_t>(cmpVal) == 0);
            }

            // Remove the value.
            auto removedVal = arrayQueuePop(accArr);

            accArrSize -= elemSize;
        });

    // Update the window field with the new total size.
    stateArr->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom<int32_t>(accArrSize));
    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableConcatArraysFinalize(ArityType arity) {
    auto stateTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));
    uassert(9476007, "State should be of array type", stateTagVal.tag() == value::TypeTags::Array);
    auto [stateArr, accArr, _] = concatArraysState(stateTagVal.tag(), stateTagVal.value());

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

    auto sumTagVal = state->getAt(static_cast<size_t>(AggRemovableStdDevElems::kSum));
    uassert(8019602, "sum elem should be of array type", sumTagVal.tag == value::TypeTags::Array);
    auto sum = value::getArrayView(sumTagVal.value);

    auto m2TagVal = state->getAt(static_cast<size_t>(AggRemovableStdDevElems::kM2));
    uassert(8019603, "m2 elem should be of array type", m2TagVal.tag == value::TypeTags::Array);
    auto m2 = value::getArrayView(m2TagVal.value);

    auto countTagVal = state->getAt(static_cast<size_t>(AggRemovableStdDevElems::kCount));
    uassert(8019604,
            "count elem should be of int64 type",
            countTagVal.tag == value::TypeTags::NumberInt64);
    auto count = value::bitcastTo<int64_t>(countTagVal.value);

    auto nonFiniteCountTagVal =
        state->getAt(static_cast<size_t>(AggRemovableStdDevElems::kNonFiniteCount));
    uassert(8019605,
            "non finite count elem should be of int64 type",
            nonFiniteCountTagVal.tag == value::TypeTags::NumberInt64);
    auto nonFiniteCount = value::bitcastTo<int64_t>(nonFiniteCountTagVal.value);

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
    auto sumTagVal = aggDoubleDoubleSumFinalizeImpl(sumState);
    double x = count * inputDouble -
        value::bitcastTo<double>(value::coerceToDouble(sumTagVal.tag(), sumTagVal.value()).second);
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

value::TagValueMaybeOwned ByteCode::builtinAggRemovableStdDevAdd(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto [inputOwned, inputTag, inputVal] = getFromStack(1);
    // Initialize the accumulator.
    if (stateTagVal.tag() == value::TypeTags::Nothing) {
        stateTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());
        auto state = value::getArrayView(stateTagVal.value());
        state->reserve(static_cast<size_t>(AggRemovableStdDevElems::kSizeOfArray));

        auto [sumStateTag, sumStateVal] = genericInitializeDoubleDoubleSumState();
        state->push_back(sumStateTag, sumStateVal);  // kSum
        auto [m2StateTag, m2StateVal] = genericInitializeDoubleDoubleSumState();
        state->push_back(m2StateTag, m2StateVal);                                        // kM2
        state->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));  // kCount
        state->push_back(value::TypeTags::NumberInt64,
                         value::bitcastFrom<int64_t>(0));  // kNonFiniteCount
    }

    aggRemovableStdDevImpl<1>(stateTagVal.tag(), stateTagVal.value(), inputTag, inputVal);

    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableStdDevRemove(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto [inputOwned, inputTag, inputVal] = getFromStack(1);

    aggRemovableStdDevImpl<-1>(stateTagVal.tag(), stateTagVal.value(), inputTag, inputVal);

    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableStdDevFinalize(ArityType arity,
                                                                      bool isSamp) {
    auto stateTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));
    auto [state, sumState, m2State, count, nonFiniteCount] =
        removableStdDevState(stateTagVal.tag(), stateTagVal.value());
    if (nonFiniteCount > 0) {
        return {false, value::TypeTags::Null, 0};
    }
    const long long adjustedCount = isSamp ? count - 1 : count;
    if (adjustedCount <= 0) {
        return {false, value::TypeTags::Null, 0};
    }
    auto m2 = aggDoubleDoubleSumFinalizeImpl(m2State);
    auto squaredDifferences =
        value::bitcastTo<double>(value::coerceToDouble(m2.tag(), m2.value()).second);
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

value::TagValueMaybeOwned ByteCode::builtinAggRemovableStdDevSampFinalize(ArityType arity) {
    return builtinAggRemovableStdDevFinalize(arity, true /* isSamp */);
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableStdDevPopFinalize(ArityType arity) {
    return builtinAggRemovableStdDevFinalize(arity, false /* isSamp */);
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableAvgFinalize(ArityType arity) {
    auto stateTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));
    auto countTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(1));

    tassert(7965901,
            "The avg accumulator state should be an array",
            stateTagVal.tag() == value::TypeTags::Array);

    return aggRemovableAvgFinalizeImpl(value::getArrayView(stateTagVal.value()),
                                       countTagVal.value());
}

/**
 * $linearFill implementation
 */
namespace {
std::tuple<value::Array*,
           value::TagValueView,
           value::TagValueView,
           value::TagValueView,
           value::TagValueView,
           value::TagValueView,
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

value::TagValueMaybeOwned ByteCode::builtinAggLinearFillCanAdd(ArityType arity) {
    auto stateTagValue = value::TagValueMaybeOwned::fromRaw(getFromStack(0));
    auto [state, x1, y1, x2, y2, prevX, count] =
        linearFillState(stateTagValue.tag(), stateTagValue.value());

    // if y2 is non-null it means we have found a valid upper window bound. in that case if count is
    // positive it means there are still more finalize calls to be made. when count == 0 we have
    // exhausted this window.
    if (y2.tag != value::TypeTags::Null) {
        return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(count == 0)};
    }

    // if y2 is null it means we have not yet found the upper window bound so keep on adding input
    // values
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(true)};
}

value::TagValueMaybeOwned ByteCode::builtinAggLinearFillAdd(ArityType arity) {
    auto stateTagValue = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto inputTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));

    auto sortByTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(2));

    // Validate the types of the values
    uassert(7971203,
            "Expected input value type to be numeric or null",
            value::isNumber(inputTagVal.tag()) || inputTagVal.tag() == value::TypeTags::Null);
    uassert(7971204,
            "Expected sortBy value type to be numeric or date",
            value::isNumber(sortByTagVal.tag()) || coercibleToDate(sortByTagVal.tag()));

    auto [state, x1, y1, x2, y2, prevX, count] =
        linearFillState(stateTagValue.tag(), stateTagValue.value());

    // Validate the current sortBy value with the previous one and update prevX
    auto [cmpTag, cmpVal] =
        value::compareValue(sortByTagVal.tag(), sortByTagVal.value(), prevX.tag, prevX.value);
    uassert(7971205,
            "There can be no repeated values in the sort field",
            cmpTag == value::TypeTags::NumberInt32 && cmpVal != 0);

    if (prevX.tag != value::TypeTags::Null) {
        uassert(7971206,
                "Conflicting sort value types, previous and current types don't match",
                (coercibleToDate(sortByTagVal.tag()) && coercibleToDate(prevX.tag)) ||
                    (value::isNumber(sortByTagVal.tag()) && value::isNumber(prevX.tag)));
    }

    auto [copyXTag, copyXVal] = value::copyValue(sortByTagVal.tag(), sortByTagVal.value());
    state->setAt(static_cast<size_t>(AggLinearFillElems::kPrevX), copyXTag, copyXVal);

    // Update x2/y2 to the current sortby/input values
    auto [sortByTag, sortByVal] = sortByTagVal.releaseToRaw();
    auto oldX2 = state->swapAt(static_cast<size_t>(AggLinearFillElems::kX2), sortByTag, sortByVal);

    auto [inputTag, inputVal] = inputTagVal.releaseToRaw();
    auto oldY2 = state->swapAt(static_cast<size_t>(AggLinearFillElems::kY2), inputTag, inputVal);

    // If (old) y2 is non-null, it means we need to look for new end-points (x1, y1), (x2, y2)
    // and the segment spanned be previous endpoints is exhausted. Count should be zero at
    // this point. Update (x1, y1) to the previous (x2, y2)
    if (oldY2.tag() != value::TypeTags::Null) {
        tassert(7971207, "count value should be zero", count == 0);
        state->setAt(static_cast<size_t>(AggLinearFillElems::kX1), std::move(oldX2));
        state->setAt(static_cast<size_t>(AggLinearFillElems::kY1), std::move(oldY2));
    }

    state->setAt(static_cast<size_t>(AggLinearFillElems::kCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(++count));

    return stateTagValue;
}

// Given two known points (x1, y1) and (x2, y2) and a value x that lies between those two
// points, we solve (or fill) for y with the following formula: y = y1 + (x - x1) * ((y2 -
// y1)/(x2 - x1))
value::TagValueMaybeOwned ByteCode::linearFillInterpolate(value::TagValueView x1,
                                                          value::TagValueView y1,
                                                          value::TagValueView x2,
                                                          value::TagValueView y2,
                                                          value::TagValueView x) {
    // (y2 - y1)
    auto delY = value::TagValueMaybeOwned::fromRaw(genericSub(y2.tag, y2.value, y1.tag, y1.value));

    // (x2 - x1)
    auto delX = value::TagValueMaybeOwned::fromRaw(genericSub(x2.tag, x2.value, x1.tag, x1.value));

    // (y2 - y1) / (x2 - x1)
    auto div = value::TagValueMaybeOwned::fromRaw(
        genericDiv(delY.tag(), delY.value(), delX.tag(), delX.value()).releaseToRaw());

    // (x - x1)
    auto sub = value::TagValueMaybeOwned::fromRaw(genericSub(x.tag, x.value, x1.tag, x1.value));

    // (x - x1) * ((y2 - y1) / (x2 - x1))
    auto mul = value::TagValueMaybeOwned::fromRaw(
        genericMul(sub.tag(), sub.value(), div.tag(), div.value()));

    // y1 + (x - x1) * ((y2 - y1) / (x2 - x1))
    return value::TagValueMaybeOwned::fromRaw(genericAdd(y1.tag, y1.value, mul.tag(), mul.value()));
}

value::TagValueMaybeOwned ByteCode::builtinAggLinearFillFinalize(ArityType arity) {
    auto stateTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));
    auto sortByTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(1));
    auto [state, x1, y1, x2, y2, prevX, count] =
        linearFillState(stateTagVal.tag(), stateTagVal.value());

    tassert(7971208, "count should be positive", count > 0);
    state->setAt(static_cast<size_t>(AggLinearFillElems::kCount),
                 value::TypeTags::NumberInt64,
                 value::bitcastFrom<int64_t>(--count));

    // if y2 is null it means the current window is the last window frame in the partition
    if (y2.tag == value::TypeTags::Null) {
        return {false, value::TypeTags::Null, 0};
    }

    // If count == 0, we are currently handling the last document in the window frame (x2/y2)
    // so we can return y2 directly. Note that the document represented by y1 was returned as
    // part of previous window (when it was y2)
    if (count == 0) {
        auto [y2Tag, y2Val] = value::copyValue(y2.tag, y2.value);
        return {true, y2Tag, y2Val};
    }

    // If y1 is null it means the current window is the first window frame in the partition
    if (y1.tag == value::TypeTags::Null) {
        return {false, value::TypeTags::Null, 0};
    }
    return linearFillInterpolate(x1, y1, x2, y2, {sortByTagVal.tag(), sortByTagVal.value()});
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

    auto queueTagVal = state->getAt(static_cast<size_t>(AggFirstLastNElems::kQueue));
    uassert(8070602, "Queue should be of array type", queueTagVal.tag == value::TypeTags::Array);
    auto queue = value::getArrayView(queueTagVal.value);

    auto nTagVal = state->getAt(static_cast<size_t>(AggFirstLastNElems::kN));
    uassert(
        8070603, "'n' elem should be of int64 type", nTagVal.tag == value::TypeTags::NumberInt64);
    auto n = value::bitcastTo<int64_t>(nTagVal.value);

    return {queue, static_cast<size_t>(n)};
}
}  // namespace

value::TagValueMaybeOwned ByteCode::builtinAggFirstLastNInit(ArityType arity) {
    auto [_, fieldTag, fieldVal] = getFromStack(0);
    auto fieldTagVal = value::rawToView({fieldTag, fieldVal});

    auto nTagVal = value::TagValueMaybeOwned::fromRaw(
        genericNumConvert(fieldTagVal.tag, fieldTagVal.value, value::TypeTags::NumberInt64));
    uassert(8070607,
            "Failed to convert to 64-bit integer",
            nTagVal.tag() == value::TypeTags::NumberInt64);

    auto n = value::bitcastTo<int64_t>(nTagVal.value());
    uassert(8070608, "Expected 'n' to be positive", n > 0);

    auto [queueTag, queueVal] = arrayQueueInit();

    auto [stateTag, stateVal] = value::makeNewArray();
    auto stateArr = value::getArrayView(stateVal);
    stateArr->push_back(queueTag, queueVal);
    stateArr->push_back(nTagVal.tag(), nTagVal.value());
    return {true, stateTag, stateVal};
}

value::TagValueMaybeOwned ByteCode::builtinAggFirstLastNAdd(ArityType arity) {
    auto state = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto field = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));

    auto [queue, n] = firstLastNState(state.tag(), state.value());

    auto [tag, val] = field.releaseToRaw();
    arrayQueuePush(queue, tag, val);

    auto [stateTag, stateVal] = state.releaseToRaw();
    return value::TagValueMaybeOwned(true, stateTag, stateVal);
}

value::TagValueMaybeOwned ByteCode::builtinAggFirstLastNRemove(ArityType arity) {
    auto state = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto field = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));

    auto [queue, n] = firstLastNState(state.tag(), state.value());

    value::TagValueOwned poppedVal = arrayQueuePop(queue);

    auto [cmpTag, cmpVal] =
        value::compareValue(poppedVal.tag(), poppedVal.value(), field.tag(), field.value());
    tassert(8070604,
            "Encountered unexpected value",
            cmpTag == value::TypeTags::NumberInt32 && cmpVal == 0);

    auto [stateTag, stateVal] = state.releaseToRaw();
    return value::TagValueMaybeOwned(true, stateTag, stateVal);
}

template <AccumulatorFirstLastN::Sense S>
value::TagValueMaybeOwned ByteCode::builtinAggFirstLastNFinalize(ArityType arity) {
    auto [_, stateTag, stateVal] = getFromStack(0);
    auto stateTagVal = value::rawToView({stateTag, stateVal});

    auto [queue, n] = firstLastNState(stateTagVal.tag, stateTagVal.value);

    if constexpr (S == AccumulatorFirstLastN::Sense::kFirst) {
        auto result = arrayQueueFrontN(queue, n);
        return result;
    } else {
        auto result = arrayQueueBackN(queue, n);
        return result;
    }
}
template value::TagValueMaybeOwned
ByteCode::builtinAggFirstLastNFinalize<(AccumulatorFirstLastN::Sense)-1>(ArityType arity);
template value::TagValueMaybeOwned
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
    auto accMultiSetTagVal = stateArr->getAt(static_cast<size_t>(AggArrayWithSize::kValues));
    tassert(8124902,
            "accumulator should be of type MultiSet",
            accMultiSetTagVal.tag == value::TypeTags::ArrayMultiSet);
    auto accMultiSet = value::getArrayMultiSetView(accMultiSetTagVal.value);

    auto accMultiSetSizeTagVal =
        stateArr->getAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues));
    tassert(8124903,
            "accumulator size should be of type NumberInt32",
            accMultiSetSizeTagVal.tag == value::TypeTags::NumberInt32);

    return {stateArr, accMultiSet, value::bitcastTo<int32_t>(accMultiSetSizeTagVal.value)};
}

value::TagValueMaybeOwned aggRemovableSetCommonInitImpl(CollatorInterface* collator) {
    auto state = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto stateArr = value::getArrayView(state.value());

    auto [mSetTag, mSetVal] = value::makeNewArrayMultiSet(collator);

    // the order is important!!!
    stateArr->push_back(mSetTag, mSetVal);  // the multiset with the values
    stateArr->push_back(value::TypeTags::NumberInt32,
                        value::bitcastFrom<int32_t>(0));  // the size in bytes of the multiset
    return state;
}
}  // namespace

value::TagValueMaybeOwned ByteCode::builtinAggRemovableSetCommonInit(ArityType arity) {
    return aggRemovableSetCommonInitImpl(nullptr /* collator */);
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableSetCommonCollInit(ArityType arity) {
    auto collator = value::TagValueMaybeOwned::fromRaw(getFromStack(0));
    tassert(
        8124904, "expected value of type 'collator'", collator.tag() == value::TypeTags::collator);

    return aggRemovableSetCommonInitImpl(value::getCollatorView(collator.value()));
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableAddToSetAdd(ArityType arity) {
    auto state = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto newEl = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));
    auto sizeCap = value::TagValueMaybeOwned::fromRaw(getFromStack(2));
    tassert(8124905,
            "The size cap must be of type NumberInt32",
            sizeCap.tag() == value::TypeTags::NumberInt32);
    auto capSize = value::bitcastTo<int32_t>(sizeCap.value());
    auto [stateArr, accMultiSet, accMultiSetSize] =
        setOperatorCommonState(state.tag(), state.value());

    // Check the size of the accumulator will not exceed the cap.
    int32_t newElSize = value::getApproximateSize(newEl.tag(), newEl.value());
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
    auto [newElTag, newElVal] = newEl.releaseToRaw();
    accMultiSet->push_back(newElTag, newElVal);
    return state;
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableAddToSetRemove(ArityType arity) {
    auto state = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto el = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));
    auto [stateArr, accMultiSet, accMultiSetSize] =
        setOperatorCommonState(state.tag(), state.value());

    int32_t elSize = value::getApproximateSize(el.tag(), el.value());
    tassert(11093710,
            "Size of element is larger than size of accumulator multiset",
            elSize <= accMultiSetSize);
    stateArr->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom<int32_t>(accMultiSetSize - elSize));

    accMultiSet->remove(el.tag(), el.value());
    return state;
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableSetCommonFinalize(ArityType arity) {
    auto state = value::TagValueMaybeOwned::fromRaw(getFromStack(0));

    auto [stateArr, accMultiSet, _] = setOperatorCommonState(state.tag(), state.value());

    // Convert the multiSet to Set.
    auto accSetTagValue =
        value::TagValueOwned::fromRaw(value::makeNewArraySet(accMultiSet->getCollator()));
    auto accSet = value::getArraySetView(accSetTagValue.value());
    for (const auto& p : accMultiSet->values()) {
        auto [cTag, cVal] = copyValue(p.first, p.second);
        accSet->push_back(cTag, cVal);
    }
    return accSetTagValue;
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableSetUnionAdd(ArityType arity) {
    auto state = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto newEl = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));
    auto sizeCapTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(2));
    tassert(9475901,
            "The size cap must be of type NumberInt32",
            sizeCapTagVal.tag() == value::TypeTags::NumberInt32);
    auto capSize = value::bitcastTo<int32_t>(sizeCapTagVal.value());
    auto [stateArr, accMultiSet, accMultiSetSize] =
        setOperatorCommonState(state.tag(), state.value());

    // If the field resolves to Nothing (e.g. if it is missing in the document), then we want to
    // leave the current state as is.
    if (newEl.tag() == value::TypeTags::Nothing) {
        return state;
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
        newEl.tag(),
        newEl.value(),
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

    return state;
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableSetUnionRemove(ArityType arity) {
    auto state = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto el = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));
    auto [stateArr, accMultiSet, accMultiSetSize] =
        setOperatorCommonState(state.tag(), state.value());

    // If the field resolves to Nothing (e.g. if it is missing in the document), then we want to
    // leave the current state as is.
    if (el.tag() == value::TypeTags::Nothing) {
        return state;
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
        el.tag(),
        el.value(),
        // We do an initialization capture because clang fails when trying to
        // capture a structured binding in a lambda expression.
        [&accMultiSet = accMultiSet, &accMultiSetSize = accMultiSetSize](
            value::TypeTags elemBeingRemovedTag, value::Value elemBeingRemovedVal) {
            value::ValueGuard removedGuard{elemBeingRemovedTag, elemBeingRemovedVal};
            auto elemSize = value::getApproximateSize(elemBeingRemovedTag, elemBeingRemovedVal);
            tassert(11093711,
                    "Size of element is larger than size of accumulator multiset",
                    elemSize <= accMultiSetSize);
            tassert(9475902,
                    "Can't remove a value that is not contained in the window",
                    accMultiSet->remove(elemBeingRemovedTag, elemBeingRemovedVal));
            accMultiSetSize -= elemSize;
        });

    // Update the window field with the new total size.
    stateArr->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom<int32_t>(accMultiSetSize));

    return state;
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
    auto accumulatorTagVal = stateArr->getAt(static_cast<size_t>(AggAccumulatorNElems::kValues));

    // Read N from the state
    auto nTagVal = stateArr->getAt(static_cast<size_t>(AggAccumulatorNElems::kN));
    tassert(
        8178103, "N should be of type NumberInt64", nTagVal.tag == value::TypeTags::NumberInt64);

    // Read memory usage information from state
    auto memUsageTagVal = stateArr->getAt(static_cast<size_t>(AggAccumulatorNElems::kMemUsage));
    tassert(8178104,
            "MemUsage component should be of type NumberInt32",
            memUsageTagVal.tag == value::TypeTags::NumberInt32);

    auto memLimitTagValue = stateArr->getAt(static_cast<size_t>(AggAccumulatorNElems::kMemLimit));
    tassert(8178105,
            "MemLimit component should be of type NumberInt32",
            memLimitTagValue.tag == value::TypeTags::NumberInt32);

    return {stateArr,
            accumulatorTagVal.tag,
            accumulatorTagVal.value,
            value::bitcastTo<size_t>(nTagVal.value),
            value::bitcastTo<int32_t>(memUsageTagVal.value),
            value::bitcastTo<int32_t>(memLimitTagValue.value)};
}
}  // namespace

value::TagValueMaybeOwned ByteCode::aggRemovableMinMaxNInitImpl(CollatorInterface* collator) {
    auto size = value::TagValueMaybeOwned::fromRaw(getFromStack(0));

    auto nTagVal = value::TagValueMaybeOwned::fromRaw(
        genericNumConvert(size.tag(), size.value(), value::TypeTags::NumberInt64));
    uassert(8178107,
            "Failed to convert to 64-bit integer",
            nTagVal.tag() == value::TypeTags::NumberInt64);

    auto n = value::bitcastTo<int64_t>(nTagVal.value());
    uassert(8178108, "Expected 'n' to be positive", n > 0);

    auto sizeCap = value::TagValueMaybeOwned::fromRaw(getFromStack(1));
    uassert(8178109,
            "The size cap must be of type NumberInt32",
            sizeCap.tag() == value::TypeTags::NumberInt32);

    // Initialize the state
    auto state = value::TagValueOwned::fromRaw(value::makeNewArray());

    auto stateArr = value::getArrayView(state.value());

    // the order is important!!!
    auto [mSetTag, mSetVal] = value::makeNewArrayMultiSet(collator);
    stateArr->push_back(mSetTag, mSetVal);
    stateArr->push_back(nTagVal.tag(),
                        nTagVal.value());  // The maximum number of elements in the multiset.
    stateArr->push_back(value::TypeTags::NumberInt32,
                        value::bitcastFrom<int32_t>(0));  // The size of the multiset in bytes.
    stateArr->push_back(sizeCap.tag(),
                        sizeCap.value());  // The maximum possible size of the multiset in bytes.
    return state;
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableMinMaxNCollInit(ArityType arity) {
    auto [collatorOwned, collatorTag, collatorVal] = getFromStack(2);
    tassert(8178111, "expected value of type 'collator'", collatorTag == value::TypeTags::collator);
    return aggRemovableMinMaxNInitImpl(value::getCollatorView(collatorVal));
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableMinMaxNInit(ArityType arity) {
    return aggRemovableMinMaxNInitImpl(nullptr);
}


value::TagValueMaybeOwned ByteCode::builtinAggRemovableMinMaxNAdd(ArityType arity) {
    auto state = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto newEl = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));

    if (value::isNullish(newEl.tag())) {
        return state;
    }

    auto [stateArr, accMultiSetTag, accMultiSetVal, n, memUsage, memLimit] =
        accumulatorNState(state.tag(), state.value());
    tassert(8178102,
            "accumulator should be of type MultiSet",
            accMultiSetTag == value::TypeTags::ArrayMultiSet);
    auto accMultiSet = value::getArrayMultiSetView(accMultiSetVal);

    int32_t newElSize = value::getApproximateSize(newEl.tag(), newEl.value());

    updateAndCheckMemUsage(stateArr,
                           memUsage,
                           newElSize,
                           memLimit,
                           static_cast<size_t>(AggAccumulatorNElems::kMemUsage));

    auto [newElTag, newElVal] = newEl.releaseToRaw();
    accMultiSet->push_back(newElTag, newElVal);

    return state;
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableMinMaxNRemove(ArityType arity) {
    auto state = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto [_, elTag, elVal] = getFromStack(1);
    auto el = value::TagValueView{elTag, elVal};

    if (value::isNullish(el.tag)) {
        return state;
    }

    auto [stateArr, accMultiSetTag, accMultiSetVal, n, memUsage, memLimit] =
        accumulatorNState(state.tag(), state.value());
    tassert(8155723,
            "accumulator should be of type MultiSet",
            accMultiSetTag == value::TypeTags::ArrayMultiSet);
    auto accMultiSet = value::getArrayMultiSetView(accMultiSetVal);

    int32_t elSize = value::getApproximateSize(el.tag, el.value);
    tassert(11093712, "Size of element is larger than used memory", elSize <= memUsage);

    // remove element
    stateArr->setAt(static_cast<size_t>(AggAccumulatorNElems::kMemUsage),
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom<int32_t>(memUsage - elSize));
    tassert(8178116, "Element was not removed", accMultiSet->remove(el.tag, el.value));

    return state;
}

template <AccumulatorMinMaxN::MinMaxSense S>
value::TagValueMaybeOwned ByteCode::builtinAggRemovableMinMaxNFinalize(ArityType arity) {
    auto state = value::TagValueMaybeOwned::fromRaw(getFromStack(0));

    auto [stateArr, accMultiSetTag, accMultiSetVal, n, memUsage, memLimit] =
        accumulatorNState(state.tag(), state.value());
    tassert(8155724,
            "accumulator should be of type MultiSet",
            accMultiSetTag == value::TypeTags::ArrayMultiSet);
    auto accMultiSet = value::getArrayMultiSetView(accMultiSetVal);

    // Create an empty array to fill with the results
    auto resultArrayTagVal = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto resultArray = value::getArrayView(resultArrayTagVal.value());
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

    return resultArrayTagVal;
}
template value::TagValueMaybeOwned
ByteCode::builtinAggRemovableMinMaxNFinalize<(AccumulatorMinMaxN::MinMaxSense)-1>(ArityType arity);
template value::TagValueMaybeOwned
ByteCode::builtinAggRemovableMinMaxNFinalize<(AccumulatorMinMaxN::MinMaxSense)1>(ArityType arity);

value::TagValueMaybeOwned ByteCode::builtinAggRemovableTopBottomNInit(ArityType arity) {
    auto maxSize = value::TagValueMaybeOwned::fromRaw(getFromStack(0));
    auto memLimit = value::TagValueMaybeOwned::fromRaw(getFromStack(1));

    auto nTagVal = value::TagValueMaybeOwned::fromRaw(
        genericNumConvert(maxSize.tag(), maxSize.value(), value::TypeTags::NumberInt64));
    uassert(8155711,
            "Failed to convert to 64-bit integer",
            nTagVal.tag() == value::TypeTags::NumberInt64);

    auto n = value::bitcastTo<int64_t>(nTagVal.value());
    uassert(8155708, "Expected 'n' to be positive", n > 0);

    tassert(8155709,
            "memLimit should be of type NumberInt32",
            memLimit.tag() == value::TypeTags::NumberInt32);

    auto state = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto stateArr = value::getArrayView(state.value());

    auto [multiMapTag, multiMapVal] = value::makeNewMultiMap();
    stateArr->push_back(multiMapTag, multiMapVal);

    stateArr->push_back(nTagVal.tag(), nTagVal.value());
    stateArr->push_back(value::TypeTags::NumberInt32, 0);
    stateArr->push_back(memLimit.tag(), memLimit.value());

    return state;
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableTopBottomNAdd(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto [state, multiMapTag, multiMapVal, n, memSize, memLimit] =
        accumulatorNState(stateTagVal.tag(), stateTagVal.value());
    tassert(8155702, "value should be of type MultiMap", multiMapTag == value::TypeTags::MultiMap);
    auto multiMap = value::getMultiMapView(multiMapVal);

    auto key = moveOwnedFromStack(1);
    auto value = moveOwnedFromStack(2);

    multiMap->insert(key, value);

    auto kvSize = value::getApproximateSize(key.first, key.second) +
        value::getApproximateSize(value.first, value.second);
    updateAndCheckMemUsage(
        state, memSize, kvSize, memLimit, static_cast<size_t>(AggAccumulatorNElems::kMemUsage));

    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggRemovableTopBottomNRemove(ArityType arity) {
    auto stateTagVal = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));

    auto [state, multiMapTag, multiMapVal, n, memSize, memLimit] =
        accumulatorNState(stateTagVal.tag(), stateTagVal.value());
    tassert(8155726, "value should be of type MultiMap", multiMapTag == value::TypeTags::MultiMap);
    auto multiMap = value::getMultiMapView(multiMapVal);

    auto key = value::TagValueMaybeOwned::fromRaw(getFromStack(1));
    auto output = value::TagValueMaybeOwned::fromRaw(getFromStack(2));

    auto removed = multiMap->remove({key.tag(), key.value()});
    tassert(8155707, "Failed to remove element from map", removed);

    auto elemSize = value::getApproximateSize(key.tag(), key.value()) +
        value::getApproximateSize(output.tag(), output.value());
    memSize -= elemSize;
    state->setAt(static_cast<size_t>(AggAccumulatorNElems::kMemUsage),
                 value::TypeTags::NumberInt32,
                 value::bitcastFrom<int32_t>(memSize));

    return stateTagVal;
}

template <TopBottomSense sense>
value::TagValueMaybeOwned ByteCode::builtinAggRemovableTopBottomNFinalize(ArityType arity) {
    auto stateTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));

    auto [state, multiMapTag, multiMapVal, n, memSize, memLimit] =
        accumulatorNState(stateTagVal.tag(), stateTagVal.value());
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

    auto res = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto resArr = value::getArrayView(res.value());

    auto it = begin;
    for (size_t inserted = 0; inserted < n && it != end; ++inserted, ++it) {
        const auto& keyOutPair = *it;
        auto output = keyOutPair.second;
        auto [copyTag, copyVal] = value::copyValue(output.first, output.second);
        resArr->push_back(copyTag, copyVal);
    };

    return res;
}
template value::TagValueMaybeOwned
ByteCode::builtinAggRemovableTopBottomNFinalize<(TopBottomSense)0>(ArityType arity);
template value::TagValueMaybeOwned
ByteCode::builtinAggRemovableTopBottomNFinalize<(TopBottomSense)1>(ArityType arity);

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
