// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
    auto field = viewFromStack(1);

    // Move the incoming accumulator state from the stack. Given that we are now the owner of the
    // state we are free to do any in-place update as we see fit.
    auto accTagVal = moveOwnedFromStack(0);

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
    auto field = viewFromStack(1);

    // Initialize the accumulator.
    auto acc = moveOwnedFromStack(0);

    // Initialize the accumulator if needed.
    if (acc.tag() == value::TypeTags::Nothing) {
        acc = value::TagValueOwned::fromRaw(value::makeNewArray());
        auto arr = value::getArrayView(acc.value());
        arr->reserve(AggStdDevValueElems::kSizeOfArray);

        // The order of the following three elements should match to 'AggStdDevValueElems'.
        arr->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
        arr->push_back_raw(value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0));
        arr->push_back_raw(value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0));
    }

    tassert(5755210, "The result slot must be Array-typed", acc.tag() == value::TypeTags::Array);
    auto accumulator = value::getArrayView(acc.value());

    if constexpr (merging) {
        aggMergeStdDevsImpl(accumulator, field.tag, field.value);
    } else {
        aggStdDevImpl(accumulator, field);
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
        accumulatorState->push_back_raw(value::makeNewArray());
        accumulatorState->push_back_raw(value::TypeTags::NumberInt64,
                                        value::bitcastFrom<int64_t>(0));
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
                                      accArray->push_back_raw(tagNewElem, valNewElem);
                                  });
    }

    return accumulatorStateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggConcatArraysCapped(ArityType arity) {
    auto lhsAccumulatorStateTagVal = moveOwnedFromStack(0);
    auto rhsAccumulatorStateTagVal = moveOwnedFromStack(1);

    auto sizeCap = viewFromStack(2);
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

    auto newSet = moveOwnedFromStack(1);
    if (!value::isArray(newSet.tag())) {
        return value::TagValueMaybeOwned::nothing();
    }

    value::arrayForEach(
        newSet.tag(), newSet.value(), [&](value::TypeTags elTag, value::Value elVal) {
            acc->push_back_clone(elTag, elVal);
        });

    return accTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggCollSetUnion(ArityType arity) {
    auto accTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(0));

    if (accTagVal.tag() == value::TypeTags::Nothing) {
        auto collatorView = viewFromStack(1);
        tassert(7690402,
                "Expected value of type 'collator'",
                collatorView.tag == value::TypeTags::collator);
        CollatorInterface* collator = value::getCollatorView(collatorView.value);

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

    auto newSet = moveOwnedFromStack(2);
    if (!value::isArray(newSet.tag())) {
        return value::TagValueMaybeOwned::nothing();
    }

    value::arrayForEach(
        newSet.tag(), newSet.value(), [&](value::TypeTags elTag, value::Value elVal) {
            acc->push_back_clone(elTag, elVal);
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
    auto lhsAccumulatorState = moveOwnedFromStack(0);
    auto rhsAccumulatorState = moveOwnedFromStack(1);

    auto sizeCap = viewFromStack(2);
    tassert(7039509,
            "'cap' parameter must be a 32-bit int",
            sizeCap.tag == value::TypeTags::NumberInt32);

    return builtinAggSetUnionCappedImpl(std::move(lhsAccumulatorState),
                                        std::move(rhsAccumulatorState),
                                        value::bitcastTo<int32_t>(sizeCap.value),
                                        /*collator=*/nullptr);
}

value::TagValueMaybeOwned ByteCode::builtinAggCollSetUnionCapped(ArityType arity) {
    auto lhsAccumulatorState = moveOwnedFromStack(0);

    auto coll = viewFromStack(1);
    tassert(7039510, "expected value of type 'collator'", coll.tag == value::TypeTags::collator);

    auto rhsAccumulatorState = moveOwnedFromStack(2);

    auto sizeCap = viewFromStack(3);
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
    return value::TagValueMaybeOwned::boolean(needMoreInput);
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
    auto stateTagVal = moveOwnedFromStack(0);

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.view());

    aggFirstN(state, array, maxSize, memUsage, memLimit, moveOwnedFromStack(1));

    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggFirstNMerge(ArityType arity) {
    auto mergeStateTagVal = moveOwnedFromStack(0);

    auto stateTagVal = moveOwnedFromStack(1);

    auto [mergeState,
          mergeArray,
          mergeStartIdx,
          mergeMaxSize,
          mergeMemUsage,
          mergeMemLimit,
          mergeIsGroupAccum] = getMultiAccState(mergeStateTagVal.view());
    auto [state, array, accStartIdx, accMaxSize, accMemUsage, accMemLimit, accIsGroupAccum] =
        getMultiAccState(stateTagVal.view());
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
    auto stateTagVal = moveOwnedFromStack(0);

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
    auto stateTagVal = moveOwnedFromStack(0);

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.view());

    auto fieldTagVal = moveOwnedFromStack(1);
    aggLastN(state, array, startIdx, maxSize, memUsage, memLimit, std::move(fieldTagVal));

    return stateTagVal;
}

value::TagValueMaybeOwned ByteCode::builtinAggLastNMerge(ArityType arity) {
    auto mergeStateTagVal = moveOwnedFromStack(0);

    auto stateTagVal = moveOwnedFromStack(1);

    auto [mergeState,
          mergeArray,
          mergeStartIdx,
          mergeMaxSize,
          mergeMemUsage,
          mergeMemLimit,
          mergeIsGroupAccum] = getMultiAccState(mergeStateTagVal.view());
    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.view());
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
    auto stateTagVal = moveOwnedFromStack(0);

    auto [state, arr, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.view());
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
            outArray->push_back_raw(copyTag, copyVal);
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
                int32_t cmp = compare<TopBottomSense::kTop>(
                    _bytecode->viewFromStack(_keysStartOffset + i), itemArray->getAt(i));

                if (cmp != 0) {
                    return sortPattern[i].isAscending ? cmp < 0 : cmp > 0;
                }
            }
        } else {
            for (size_t i = 0; i < sortPattern.size(); i++) {
                int32_t cmp = compare<TopBottomSense::kBottom>(
                    _bytecode->viewFromStack(_keysStartOffset + i), itemArray->getAt(i));

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
            keysArr->push_back(_bytecode->moveOwnedFromStack(_keysStartOffset + i));
        }

        return keys;
    }

    value::TagValueOwned getOwnedValueImpl() final {
        auto values = value::TagValueOwned::fromRaw(value::makeNewArray());
        auto valuesArr = value::getArrayView(values.value());

        for (size_t i = 0; i < _numValues; ++i) {
            valuesArr->push_back(_bytecode->moveOwnedFromStack(_valuesStartOffset + i));
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

    auto stateTagVal = moveOwnedFromStack(0);

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.view());

    size_t numKeys = 1;
    bool keyIsDecomposed = false;
    auto numKeysView = viewFromStack(2);
    if (numKeysView.tag == value::TypeTags::NumberInt32) {
        numKeys = static_cast<size_t>(value::bitcastTo<int32_t>(numKeysView.value));
        keyIsDecomposed = true;
    } else {
        tassert(8448704,
                "Expected numKeys to be Null or Int32",
                numKeysView.tag == value::TypeTags::Null);
    }

    constexpr size_t keysStartOffset = 3;
    const size_t valuesStartOffset = keysStartOffset + numKeys;
    const size_t numValues = ValueIsDecomposedArray ? arity - valuesStartOffset : 1;

    if (!keyIsDecomposed && !ValueIsDecomposedArray) {
        auto key = moveOwnedFromStack(keysStartOffset);
        auto val = moveOwnedFromStack(valuesStartOffset);

        TopBottomArgsDirect topBottomArgs{Sense, ss, std::move(key), std::move(val)};

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

    auto stateTagVal = moveOwnedFromStack(1);
    auto mergeStateTagVal = moveOwnedFromStack(0);
    auto [mergeState,
          mergeArray,
          mergeStartIx,
          mergeMaxSize,
          mergeMemUsage,
          mergeMemLimit,
          mergeIsGroupAccum] = getMultiAccState(mergeStateTagVal.view());
    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.view());
    tassert(5807008,
            "Two arrays to merge should have the same MaxSize component",
            maxSize == mergeMaxSize);

    for (auto [pairTag, pairVal] : array->values()) {
        auto pair = value::getArrayView(pairVal);

        TopBottomArgsDirect topBottomArgs{Sense,
                                          sortSpec,
                                          pair->swapAt(0, value::TypeTags::Null, 0),
                                          pair->swapAt(1, value::TypeTags::Null, 0)};

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

    auto stateTagVal = moveOwnedFromStack(0);
    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.view());

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
            outputArray->push_back_raw(copyTag, copyVal);
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

value::TagValueMaybeOwned ByteCode::builtinAvgFromAcc(ArityType arity) {
    auto accTagVal = value::TagValueOwned::fromRaw(genericInitializeDoubleDoubleSumState());
    size_t numNumericArgs = 0;
    value::Array* accumulator = value::getArrayView(accTagVal.value());

    auto processOne = [&](value::TypeTags tag, value::Value val) {
        if (value::isNumber(tag)) {
            aggDoubleDoubleSumImpl(accumulator, tag, val);
            ++numNumericArgs;
        }
    };

    processStackRange(0, arity, processOne);

    if (numNumericArgs == 0) {
        return value::TagValueMaybeOwned::null();
    }
    auto sum = aggDoubleDoubleSumFinalizeImpl(accumulator);
    return genericDiv(sum.view(), value::TagValueView::numberInt64(numNumericArgs));
}

template <AccumulatorMinMaxN::MinMaxSense S>
value::TagValueMaybeOwned ByteCode::builtinMinMaxNFromAcc(ArityType arity) {
    CollatorInterface* collator = nullptr;
    ArityType startIdx = 0;
    if (arity > 0) {
        auto arg0 = viewFromStack(0);
        if (arg0.tag == value::TypeTags::collator) {
            collator = value::getCollatorView(arg0.value);
            startIdx = 1;
        }
    }

    value::TagValueOwned acc = value::TagValueOwned::nothing();

    auto processOne = [&](value::TypeTags tag, value::Value val) {
        // Mirror how SBE HashAgg handles nullish accumulator inputs.
        if (value::isNullish(tag)) {
            tag = value::TypeTags::Nothing;
            val = 0;
        }
        if constexpr (S == AccumulatorMinMaxN::MinMaxSense::kMax) {
            acc = aggMax(acc.view(), value::TagValueView{tag, val}, collator);
        } else {
            acc = aggMin(acc.view(), value::TagValueView{tag, val}, collator);
        }
    };

    processStackRange(startIdx, arity, processOne);

    if (acc.tag() == value::TypeTags::Nothing) {
        return value::TagValueMaybeOwned::null();
    }
    return std::move(acc);
}
template value::TagValueMaybeOwned
ByteCode::builtinMinMaxNFromAcc<AccumulatorMinMaxN::MinMaxSense::kMin>(ArityType arity);
template value::TagValueMaybeOwned
ByteCode::builtinMinMaxNFromAcc<AccumulatorMinMaxN::MinMaxSense::kMax>(ArityType arity);

template <AccumulatorMinMaxN::MinMaxSense S>
value::TagValueMaybeOwned ByteCode::builtinAggMinMaxN(ArityType arity) {
    tassert(11080087, "Unexpected arity value", arity == 2 || arity == 3);

    auto stateTagVal = moveOwnedFromStack(0);

    auto fieldTagVal = moveOwnedFromStack(1);
    if (value::isNullish(fieldTagVal.tag())) {
        return stateTagVal;
    }

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.view());

    CollatorInterface* collator = nullptr;
    if (arity == 3) {
        auto collTagVal = value::TagValueMaybeOwned::fromRaw(getFromStack(2));
        uassert(
            7548802, "expected a collator argument", collTagVal.tag() == value::TypeTags::collator);
        collator = value::getCollatorView(collTagVal.value());
    }
    aggMinMaxN<S>(state, array, maxSize, memUsage, memLimit, collator, std::move(fieldTagVal));

    return stateTagVal;
}
template value::TagValueMaybeOwned ByteCode::builtinAggMinMaxN<(AccumulatorMinMax::Sense)-1>(
    ArityType arity);
template value::TagValueMaybeOwned ByteCode::builtinAggMinMaxN<(AccumulatorMinMax::Sense)1>(
    ArityType arity);

template <AccumulatorMinMaxN::MinMaxSense S>
value::TagValueMaybeOwned ByteCode::builtinAggMinMaxNMerge(ArityType arity) {
    tassert(11080086, "Unexpected arity value", arity == 2 || arity == 3);

    auto mergeStateTagVal = moveOwnedFromStack(0);

    auto stateTagVal = moveOwnedFromStack(1);

    auto [mergeState,
          mergeArray,
          mergeStartIdx,
          mergeMaxSize,
          mergeMemUsage,
          mergeMemLimit,
          mergeIsGroupAccum] = getMultiAccState(mergeStateTagVal.view());
    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.view());
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
    auto stateTagVal = moveOwnedFromStack(0);

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTagVal.view());

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

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
