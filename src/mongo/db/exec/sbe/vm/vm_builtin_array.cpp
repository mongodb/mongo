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

#include "mongo/db/exec/sbe/in_list.h"
#include "mongo/db/exec/sbe/sbe_pattern_value_cmp.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"

#include <algorithm>
#include <utility>

namespace mongo {
namespace sbe {
namespace vm {

namespace {

void expectOwnedArray(bool owned, value::TypeTags type) {
    tassert(11086820, "Expecting owned value", owned);
    tassert(11086813, "Expecting array type", type == value::TypeTags::Array);
}

}  // namespace

// We need to ensure that 'size_t' is wide enough to store a 32-bit index.
// This is assumed by both builtinZipArrays and builtinExtractSubArray.
static_assert(sizeof(size_t) >= sizeof(int32_t), "size_t must be at least 32-bits");

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinNewArray(ArityType arity) {
    auto result = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto arr = value::getArrayView(result.value());

    if (arity) {
        arr->reserve(arity);
        for (ArityType idx = 0; idx < arity; ++idx) {
            arr->push_back(moveOwnedFromStack(idx));
        }
    }

    return result.releaseToMaybeOwnedRaw();
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinNewArrayFromRange(ArityType arity) {
    auto result = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto arr = value::getArrayView(result.value());

    auto startView = viewFromStack(0);
    auto endView = viewFromStack(1);
    auto stepView = viewFromStack(2);

    for (auto tag : {startView.tag, endView.tag, stepView.tag}) {
        if (value::TypeTags::NumberInt32 != tag) {
            return {false, value::TypeTags::Nothing, 0};
        }
    }

    // Cast to broader type 'int64_t' to prevent overflow during loop.
    auto startVal = value::numericCast<int64_t>(startView.tag, startView.value);
    auto endVal = value::numericCast<int64_t>(endView.tag, endView.value);
    auto stepVal = value::numericCast<int64_t>(stepView.tag, stepView.value);

    if (stepVal == 0) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Calculate how much memory is needed to generate the array and avoid going over the memLimit.
    auto steps = (endVal - startVal) / stepVal;
    // If steps not positive then no amount of steps can get you from start to end. For example
    // with start=5, end=7, step=-1 steps would be negative and in this case we would return an
    // empty array.
    auto length = steps >= 0 ? 1 + steps : 0;
    int64_t memNeeded =
        sizeof(value::Array) + length * value::getApproximateSize(startView.tag, startView.value);
    auto memLimit = internalQueryMaxRangeBytes.load();
    uassert(ErrorCodes::ExceededMemoryLimit,
            str::stream() << "$range would use too much memory (" << memNeeded
                          << " bytes) and cannot spill to disk. Memory limit: " << memLimit
                          << " bytes",
            memNeeded < memLimit);

    arr->reserve(length);
    for (auto i = startVal; stepVal > 0 ? i < endVal : i > endVal; i += stepVal) {
        arr->push_back(value::TypeTags::NumberInt32, value::bitcastTo<int32_t>(i));
    }

    return result.releaseToMaybeOwnedRaw();
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAddToArray(ArityType arity) {
    auto aggView = viewFromStack(0);
    auto field = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));

    // Create a new array if it does not exist yet.
    value::TagValueMaybeOwned agg;
    if (aggView.tag == value::TypeTags::Nothing) {
        agg = value::TagValueOwned::fromRaw(value::makeNewArray());
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
        agg = value::TagValueMaybeOwned{true, aggView.tag, aggView.value};
    }
    expectOwnedArray(agg.owned(), agg.tag());
    auto arr = value::getArrayView(agg.value());

    // Push back the value. Note that array will ignore Nothing.
    arr->push_back(field.releaseToRaw());

    return agg.releaseToRaw();
}

value::TagValueMaybeOwned ByteCode::builtinAddToArrayCappedImpl(
    value::TagValueOwned accumulatorStateTagVal,
    value::TagValueMaybeOwned newElem,
    int32_t sizeCap) {

    // The capped array accumulator holds a value of Nothing at first and gets initialized on demand
    // when the first value gets added. Once initialized, the state is a two-element array
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
    tassert(11004212,
            "Expected array for set accumulator state",
            accumulatorStateTagVal.tag() == value::TypeTags::Array);

    auto accumulatorState = value::getArrayView(accumulatorStateTagVal.value());
    tassert(11004213,
            "Array accumulator with invalid length",
            accumulatorState->size() == static_cast<size_t>(AggArrayWithSize::kLast));

    // Compute the size of the array after adding the new element.
    auto [tagAccArraySize, valAccArraySize] =
        accumulatorState->getAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues));
    tassert(11004214,
            "Expected integer value for array size",
            tagAccArraySize == value::TypeTags::NumberInt64);
    int64_t currentSize = value::bitcastTo<int64_t>(valAccArraySize);
    int newElemSize = value::getApproximateSize(newElem.tag(), newElem.value());
    int64_t newSize = currentSize + newElemSize;

    // Check that array with the new element will not exceed the limit.
    auto [tagAccArray, valAccArray] =
        accumulatorState->getAt(static_cast<size_t>(AggArrayWithSize::kValues));
    tassert(11004215, "Expected Array in accumulator state", tagAccArray == value::TypeTags::Array);
    auto accArray = value::getArrayView(valAccArray);

    uassert(ErrorCodes::ExceededMemoryLimit,
            str::stream() << "Used too much memory for a single array. Memory limit: " << sizeCap
                          << " bytes. The array contains " << accArray->size()
                          << " elements and is of size " << currentSize
                          << " bytes. The element being added has size " << newElemSize
                          << " bytes.",
            newSize < sizeCap);

    // Update the array's size as stored by the accumulator.
    accumulatorState->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
                            value::TypeTags::NumberInt64,
                            value::bitcastFrom<int64_t>(newSize));

    // Add an owned copy of the element to the array.
    accArray->push_back(newElem.releaseToOwnedRaw());

    return accumulatorStateTagVal;
}

// The value being accumulated is an SBE array that contains an integer and the accumulated array,
// where the integer is the total size in bytes of the elements in the array.
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAddToArrayCapped(ArityType arity) {
    auto accumulatorState = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto newElem = value::TagValueMaybeOwned::fromRaw(moveFromStack(1));

    auto sizeCap = viewFromStack(2);

    // Return the unmodified accumulator state when the collator or size cap is malformed.
    if (sizeCap.tag != value::TypeTags::NumberInt32) {
        return accumulatorState.releaseToMaybeOwnedRaw();
    }

    return builtinAddToArrayCappedImpl(std::move(accumulatorState),
                                       std::move(newElem),
                                       value::bitcastTo<int32_t>(sizeCap.value))
        .releaseToRaw();
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinConcatArrays(ArityType arity) {
    auto result = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto resView = value::getArrayView(result.value());

    for (ArityType idx = 0; idx < arity; ++idx) {
        auto elem = viewFromStack(idx);
        if (!value::isArray(elem.tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        value::arrayForEach(elem.tag, elem.value, [&](value::TypeTags elTag, value::Value elVal) {
            resView->push_back(value::copyValue(elTag, elVal));
        });
    }

    return result.releaseToMaybeOwnedRaw();
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinZipArrays(ArityType arity) {
    ArityType localVariables = 0;
    tassert(5156501, "Invalid parameter count for builtin ZipArrays", arity >= 2);

    const auto inputSizeView = viewFromStack(localVariables++);
    const auto useLongestLengthView = viewFromStack(localVariables++);

    if (useLongestLengthView.tag != value::TypeTags::Boolean ||
        inputSizeView.tag != value::TypeTags::NumberInt32) {
        return {false, value::TypeTags::Nothing, 0};
    }

    const bool useLongestLength = value::bitcastTo<bool>(useLongestLengthView.value);
    const size_t inputSize = value::bitcastTo<int32_t>(inputSizeView.value);

    // Check that the count of input arrays doesn't exceed the parameters received.
    tassert(5156502,
            "Invalid parameter 'input size' for builtin ZipArrays",
            inputSize <= arity - localVariables);

    const size_t defaultSize = arity - localVariables - inputSize;

    // Assert whether defaults has the same size as the input (also checked by an upper layer).
    tassert(5156503,
            "Invalid default array count for builtin ZipArrays",
            defaultSize == 0 || defaultSize == inputSize);

    // Keeps enumerators to every input array.
    absl::InlinedVector<value::ArrayEnumerator, 8> inputs;
    inputs.reserve(inputSize);

    size_t outputLength = 0;
    for (size_t i = 0; i < inputSize; ++i) {
        auto elem = viewFromStack(localVariables + i);
        if (!value::isArray(elem.tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        inputs.emplace_back(elem.tag, elem.value);

        const size_t arraySize = value::getArraySize(elem.tag, elem.value);
        if (i == 0) {
            outputLength = arraySize;
        } else {
            outputLength = useLongestLength ? std::max(arraySize, outputLength)
                                            : std::min(arraySize, outputLength);
        }
    }

    // The final output array, e.g. [[1, 2, 3], [2, 3, 4]].
    auto result = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto* resView = value::getArrayView(result.value());
    resView->reserve(outputLength);

    for (size_t row = 0; row < outputLength; row++) {
        // Used to construct each array in the output, e.g. [1, 2, 3].
        auto intermediateRes = value::TagValueOwned::fromRaw(value::makeNewArray());
        auto* intermediateResView = value::getArrayView(intermediateRes.value());
        intermediateResView->reserve(inputSize);

        for (size_t col = 0; col < inputSize; col++) {
            value::ArrayEnumerator& input = inputs[col];
            if (!input.atEnd()) {
                // Add the value from the appropriate input array.
                auto inputElem = input.getViewOfValue();
                intermediateResView->push_back(value::copyValue(inputElem.tag, inputElem.value));
                input.advance();
            } else if (col < defaultSize) {
                // Add the specified default value.
                auto defaultElem = viewFromStack(localVariables + inputSize + col);
                intermediateResView->push_back(
                    value::copyValue(defaultElem.tag, defaultElem.value));
            } else {
                // Add a null default value.
                intermediateResView->push_back(value::TypeTags::Null, 0);
            }
        }
        resView->push_back(intermediateRes.releaseToRaw());
    }

    return result.releaseToMaybeOwnedRaw();
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinConcatArraysCapped(
    ArityType arity) {
    auto accumulatorState = value::TagValueOwned::fromRaw(moveOwnedFromStack(0));
    auto newArrayElements = value::TagValueOwned::fromRaw(moveOwnedFromStack(1));

    auto sizeCap = viewFromStack(2);

    // Return the unmodified accumulator state when the size cap is malformed.
    if (sizeCap.tag != value::TypeTags::NumberInt32) {
        return accumulatorState.releaseToMaybeOwnedRaw();
    }

    auto newArrayElemsSize =
        value::getApproximateSize(newArrayElements.tag(), newArrayElements.value());

    return concatArraysAccumImpl(std::move(accumulatorState),
                                 std::move(newArrayElements),
                                 newArrayElemsSize,
                                 value::bitcastTo<int32_t>(sizeCap.value))
        .releaseToRaw();
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::isMemberImpl(value::TypeTags exprTag,
                                                                      value::Value exprVal,
                                                                      value::TypeTags arrTag,
                                                                      value::Value arrVal,
                                                                      CollatorInterface* collator) {
    if (!value::isArray(arrTag) && arrTag != value::TypeTags::inList) {
        return {false, value::TypeTags::Nothing, 0};
    }

    if (exprTag == value::TypeTags::Nothing) {
        return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(false)};
    }

    if (arrTag == value::TypeTags::inList) {
        // For InLists, we intentionally ignore the 'collator' parmeter and we use the
        // InList's collator instead.
        InList* inList = value::getInListView(arrVal);
        const bool found = inList->contains(exprTag, exprVal);

        return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(found)};
    } else if (arrTag == value::TypeTags::ArraySet) {
        // An empty ArraySet may not have a collation, but we don't need one to definitively
        // determine that the empty set doesn't contain the value we are checking.
        auto arrSet = value::getArraySetView(arrVal);
        if (arrSet->size() == 0) {
            return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(false)};
        }
        auto& values = arrSet->values();
        if (collator != nullptr) {
            // An ArraySet with a collation can lose information about its members that would be
            // necessary to answer membership queries using a different collation. We require that
            // well formed SBE programs do not execute a "collIsMember" instruction with mismatched
            // collations.
            tassert(5153701,
                    "Expected ArraySet to have matching collator",
                    CollatorInterface::collatorsMatch(collator, arrSet->getCollator()));
        }
        return {false,
                value::TypeTags::Boolean,
                value::bitcastFrom<bool>(values.find({exprTag, exprVal}) != values.end())};
    }
    const bool found =
        value::arrayAny(arrTag, arrVal, [&](value::TypeTags elemTag, value::Value elemVal) {
            auto [tag, val] = value::compareValue(exprTag, exprVal, elemTag, elemVal, collator);
            if (tag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(val) == 0) {
                return true;
            }
            return false;
        });

    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(found)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinIsMember(ArityType arity) {
    tassert(11080068, "Unexpected arity value", arity == 2);
    auto expr = viewFromStack(0);
    auto arr = viewFromStack(1);

    return ByteCode::isMemberImpl(expr.tag, expr.value, arr.tag, arr.value, nullptr);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollIsMember(ArityType arity) {
    tassert(11080067, "Unexpected arity value", arity == 3);
    auto expr = viewFromStack(0);
    auto arr = viewFromStack(1);
    auto collatorView = viewFromStack(2);

    CollatorInterface* collator = nullptr;
    if (collatorView.tag == value::TypeTags::collator) {
        collator = value::getCollatorView(collatorView.value);
    } else {
        // If a third parameter was supplied but it is not a Collator, return Nothing.
        return {false, value::TypeTags::Nothing, 0};
    }

    return ByteCode::isMemberImpl(expr.tag, expr.value, arr.tag, arr.value, collator);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinExtractSubArray(ArityType arity) {
    auto arrayView = viewFromStack(0);
    auto limitView = viewFromStack(1);

    if (!value::isArray(arrayView.tag) || limitView.tag != value::TypeTags::NumberInt32) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto limit = value::bitcastTo<int32_t>(limitView.value);

    auto absWithSign = [](int32_t value) -> std::pair<bool, size_t> {
        if (value < 0) {
            // Upcast 'value' to 'int64_t' prevent overflow during the sign change.
            return {true, -static_cast<int64_t>(value)};
        }
        return {false, value};
    };

    size_t start = 0;
    bool isNegativeStart = false;
    size_t length = 0;
    if (arity == 2) {
        std::tie(isNegativeStart, start) = absWithSign(limit);
        length = start;
        if (!isNegativeStart) {
            start = 0;
        }
    } else {
        if (limit < 0) {
            return {false, value::TypeTags::Nothing, 0};
        }
        length = limit;

        auto skipView = viewFromStack(2);
        if (skipView.tag != value::TypeTags::NumberInt32) {
            return {false, value::TypeTags::Nothing, 0};
        }

        auto skip = value::bitcastTo<int32_t>(skipView.value);
        std::tie(isNegativeStart, start) = absWithSign(skip);
    }

    auto result = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto resultView = value::getArrayView(result.value());

    if (arrayView.tag == value::TypeTags::Array) {
        auto inputView = value::getArrayView(arrayView.value);
        auto arraySize = inputView->size();

        auto convertedStart = [&]() -> size_t {
            if (isNegativeStart) {
                if (start > arraySize) {
                    return 0;
                } else {
                    return arraySize - start;
                }
            } else {
                return std::min(start, arraySize);
            }
        }();

        size_t end = convertedStart + std::min(length, arraySize - convertedStart);
        if (convertedStart < end) {
            resultView->reserve(end - convertedStart);

            for (size_t i = convertedStart; i < end; i++) {
                auto elem = inputView->getAt(i);
                resultView->push_back(value::copyValue(elem.tag, elem.value));
            }
        }
    } else {
        auto advance = [](value::ArrayEnumerator& enumerator, size_t offset) {
            size_t i = 0;
            while (i < offset && !enumerator.atEnd()) {
                i++;
                enumerator.advance();
            }
        };

        value::ArrayEnumerator startEnumerator{arrayView.tag, arrayView.value};
        if (isNegativeStart) {
            value::ArrayEnumerator windowEndEnumerator{arrayView.tag, arrayView.value};
            advance(windowEndEnumerator, start);

            while (!startEnumerator.atEnd() && !windowEndEnumerator.atEnd()) {
                startEnumerator.advance();
                windowEndEnumerator.advance();
            }
            tassert(11093713,
                    "Enumerator didn't reach the end of the array",
                    windowEndEnumerator.atEnd());
        } else {
            advance(startEnumerator, start);
        }

        size_t i = 0;
        while (i < length && !startEnumerator.atEnd()) {
            auto elem = startEnumerator.getViewOfValue();
            resultView->push_back(value::copyValue(elem.tag, elem.value));

            i++;
            startEnumerator.advance();
        }
    }

    return result.releaseToMaybeOwnedRaw();
}  // ByteCode::builtinExtractSubArray

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinIsArrayEmpty(ArityType arity) {
    tassert(11080066, "Unexpected arity value", arity == 1);
    auto arr = viewFromStack(0);

    if (!value::isArray(arr.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    if (arr.tag == value::TypeTags::Array) {
        auto arrayView = value::getArrayView(arr.value);
        return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(arrayView->size() == 0)};
    } else if (arr.tagIn(value::TypeTags::bsonArray, value::TypeTags::ArraySet)) {
        value::ArrayEnumerator enumerator(arr.tag, arr.value);
        return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(enumerator.atEnd())};
    } else {
        // Earlier in this function we bailed out if the 'arrayType' wasn't Array, ArraySet or
        // bsonArray, so it should be impossible to reach this point.
        MONGO_UNREACHABLE_TASSERT(11122942);
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinReverseArray(ArityType arity) {
    tassert(11080065, "Unexpected arity value", arity == 1);
    auto input = viewFromStack(0);

    if (!value::isArray(input.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto result = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto resultView = value::getArrayView(result.value());

    if (input.tag == value::TypeTags::Array) {
        auto inputView = value::getArrayView(input.value);
        size_t inputSize = inputView->size();
        if (inputSize) {
            resultView->reserve(inputSize);
            for (size_t i = 0; i < inputSize; i++) {
                auto orig = inputView->getAt(inputSize - 1 - i);
                resultView->push_back(copyValue(orig.tag, orig.value));
            }
        }

        return result.releaseToMaybeOwnedRaw();
    } else if (input.tagIn(value::TypeTags::bsonArray, value::TypeTags::ArraySet)) {
        // Using intermediate vector since bsonArray and ArraySet don't
        // support reverse iteration.
        std::vector<value::TagValueView> inputContents;

        if (input.tag == value::TypeTags::ArraySet) {
            // Reserve space to avoid resizing on push_back calls.
            auto arraySetView = value::getArraySetView(input.value);
            inputContents.reserve(arraySetView->size());
        }

        value::arrayForEach(input.tag, input.value, [&](value::TypeTags elTag, value::Value elVal) {
            inputContents.push_back({elTag, elVal});
        });

        if (inputContents.size()) {
            resultView->reserve(inputContents.size());

            // Run through the array backwards and copy into the result array.
            for (auto it = inputContents.rbegin(); it != inputContents.rend(); ++it) {
                resultView->push_back(copyValue(it->tag, it->value));
            }
        }

        return result.releaseToMaybeOwnedRaw();
    } else {
        // Earlier in this function we bailed out if the 'inputType' wasn't
        // Array, ArraySet or bsonArray, so it should be impossible to reach
        // this point.
        MONGO_UNREACHABLE_TASSERT(11122943);
    }
}  // ByteCode::builtinReverseArray

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSortArray(ArityType arity) {
    tassert(11080064, "Unexpected arity value", arity == 2 || arity == 3);
    auto input = viewFromStack(0);

    if (!value::isArray(input.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto spec = viewFromStack(1);

    if (!value::isObject(spec.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    CollatorInterface* collator = nullptr;
    if (arity == 3) {
        auto collatorView = viewFromStack(2);

        if (collatorView.tag == value::TypeTags::collator) {
            collator = value::getCollatorView(collatorView.value);
        } else {
            // If a third parameter was supplied but it is not a Collator, return Nothing.
            return {false, value::TypeTags::Nothing, 0};
        }
    }

    auto cmp = SbePatternValueCmp(spec.tag, spec.value, collator);

    auto result = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto resultView = value::getArrayView(result.value());

    if (input.tag == value::TypeTags::Array) {
        auto inputView = value::getArrayView(input.value);
        size_t inputSize = inputView->size();
        if (inputSize) {
            resultView->reserve(inputSize);
            std::vector<value::TagValueView> sortVector;
            for (size_t i = 0; i < inputSize; i++) {
                sortVector.push_back(inputView->getAt(i));
            }
            std::sort(sortVector.begin(), sortVector.end(), cmp);

            for (auto elem : sortVector) {
                resultView->push_back(copyValue(elem.tag, elem.value));
            }
        }

        return result.releaseToMaybeOwnedRaw();
    } else if (input.tagIn(value::TypeTags::bsonArray, value::TypeTags::ArraySet)) {
        value::ArrayEnumerator enumerator{input.tag, input.value};

        // Using intermediate vector since bsonArray and ArraySet don't
        // support random access.
        std::vector<value::TagValueView> inputContents;

        if (input.tag == value::TypeTags::ArraySet) {
            // Reserve space to avoid resizing on push_back calls.
            auto arraySetView = value::getArraySetView(input.value);
            inputContents.reserve(arraySetView->size());
        }

        while (!enumerator.atEnd()) {
            inputContents.push_back(enumerator.getViewOfValue());
            enumerator.advance();
        }

        std::sort(inputContents.begin(), inputContents.end(), cmp);

        if (inputContents.size()) {
            resultView->reserve(inputContents.size());

            for (auto elem : inputContents) {
                resultView->push_back(copyValue(elem.tag, elem.value));
            }
        }

        return result.releaseToMaybeOwnedRaw();
    } else {
        // Earlier in this function we bailed out if the 'inputType' wasn't
        // Array, ArraySet or bsonArray, so it should be impossible to reach
        // this point.
        MONGO_UNREACHABLE_TASSERT(11122944);
    }
}  // ByteCode::builtinSortArray

namespace {
/**
 * Helper function to extract and validate the 'n' parameter for topN/bottomN.
 * Returns -1 if validation fails.
 */
inline int64_t extractNParameter(value::TypeTags nTag, value::Value nVal) {
    if (!value::isNumber(nTag)) {
        return -1;
    }

    int64_t n;
    switch (nTag) {
        case value::TypeTags::NumberInt64:
            n = value::bitcastTo<int64_t>(nVal);
            break;
        case value::TypeTags::NumberInt32:
            n = value::bitcastTo<int32_t>(nVal);
            break;
        case value::TypeTags::NumberDouble: {
            double dVal = value::bitcastTo<double>(nVal);
            auto result = mongo::representAs<int64_t>(dVal);
            if (!result) {
                return -1;
            }
            n = *result;
            break;
        }
        case value::TypeTags::NumberDecimal: {
            auto dVal = value::bitcastTo<Decimal128>(nVal);
            auto result = mongo::representAs<int64_t>(dVal);
            if (!result) {
                return -1;
            }
            n = *result;
            break;
        }
        default:
            return -1;
    }

    return n;
}

/**
 * Helper function to perform partial sort and extract top N or bottom N elements from a vector.
 * Modifies the input vector in place and copies the selected N elements to the result array.
 */
void extractTopOrBottomN(std::vector<std::pair<value::TypeTags, value::Value>>& sortVector,
                         size_t n,
                         value::Array* resultView,
                         const SbePatternValueCmp& cmp,
                         TopBottomSense sense) {
    size_t inputSize = sortVector.size();
    size_t nSize = std::min(inputSize, n);

    // Sort the top or bottom n elements in the array in the correct order.
    // Partial sort uses a heap to sort the elements.
    if (sense == TopBottomSense::kBottom) {
        auto inverse = [&cmp](const auto& lhs, const auto& rhs) {
            return cmp(rhs, lhs);
        };
        std::partial_sort(
            sortVector.rbegin(), sortVector.rbegin() + nSize, sortVector.rend(), inverse);
    } else {
        std::partial_sort(sortVector.begin(), sortVector.begin() + nSize, sortVector.end(), cmp);
    }

    // Copy the top or bottom n elements into the result array.
    // For bottomN, the elements are at the end of the vector after reverse partial_sort.
    size_t startIdx = sense == TopBottomSense::kBottom ? inputSize - nSize : 0;
    for (size_t i = 0; i < nSize; i++) {
        auto [tag, val] = sortVector[startIdx + i];
        auto [copyTag, copyVal] = copyValue(tag, val);
        resultView->push_back(copyTag, copyVal);
    }
}


}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::topOrBottomImpl(ArityType arity,
                                                                         TopBottomSense sense) {
    tassert(1127464, "Unexpected arity value", arity == 2 || arity == 3);

    auto input = viewFromStack(0);
    if (!value::isArray(input.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto spec = viewFromStack(1);
    if (!value::isObject(spec.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    const CollatorInterface* collator = nullptr;
    if (arity == 3) {
        auto collatorView = viewFromStack(2);
        if (collatorView.tag == value::TypeTags::collator) {
            collator = value::getCollatorView(collatorView.value);
        } else {
            // If a third parameter was supplied but it is not a Collator, return Nothing.
            return {false, value::TypeTags::Nothing, 0};
        }
    }

    auto cmpInput = SbePatternValueCmp(spec.tag, spec.value, collator);

    // Inverse comparator if the sense is kBottom
    auto cmp = [sense, &cmpInput](auto lhs, auto rhs) {
        if (sense == TopBottomSense::kTop) {
            return cmpInput(lhs, rhs);
        } else {
            return cmpInput(rhs, lhs);
        }
    };

    if (input.tag == value::TypeTags::Array) {
        auto inputView = value::getArrayView(input.value);
        size_t inputSize = inputView->size();
        if (inputSize == 0) {
            return {true, value::TypeTags::Null, 0};
        }

        // Get the best element, either the top or bottom element depending on cmp.
        auto best_element = inputView->getAt(0);
        for (size_t i = 1; i < inputSize; i++) {
            if (cmp(inputView->getAt(i), best_element)) {
                best_element = inputView->getAt(i);
            }
        }

        return best_element.copy().releaseToMaybeOwnedRaw();
    } else if (input.tagIn(value::TypeTags::bsonArray, value::TypeTags::ArraySet)) {
        value::ArrayEnumerator enumerator{input.tag, input.value};

        if (enumerator.atEnd()) {
            return {true, value::TypeTags::Null, 0};
        }

        // Get the best element, either the top or bottom element depending on cmp.
        auto best_element = enumerator.getViewOfValue();
        enumerator.advance();
        while (!enumerator.atEnd()) {
            auto current_element = enumerator.getViewOfValue();
            if (cmp(current_element, best_element)) {
                best_element = current_element;
            }
            enumerator.advance();
        }

        return best_element.copy().releaseToMaybeOwnedRaw();
    } else {
        // Earlier in this function we bailed out if the 'inputType' wasn't
        // Array, ArraySet or bsonArray, so it should be impossible to reach
        // this point.
        MONGO_UNREACHABLE_TASSERT(1127465);
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::topOrBottomNImpl(ArityType arity,
                                                                          TopBottomSense sense) {
    tassert(1127461, "Unexpected arity value", arity == 3 || arity == 4);

    auto nView = viewFromStack(0);
    int64_t n = extractNParameter(nView.tag, nView.value);
    if (n < 0) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto input = viewFromStack(1);
    if (!value::isArray(input.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto spec = viewFromStack(2);
    if (!value::isObject(spec.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    const CollatorInterface* collator = nullptr;
    if (arity == 4) {
        auto collatorView = viewFromStack(3);
        if (collatorView.tag == value::TypeTags::collator) {
            collator = value::getCollatorView(collatorView.value);
        } else {
            // If a fourth parameter was supplied but it is not a Collator, return Nothing.
            return {false, value::TypeTags::Nothing, 0};
        }
    }

    auto cmp = SbePatternValueCmp(spec.tag, spec.value, collator);

    auto result = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto resultView = value::getArrayView(result.value());

    if (input.tag == value::TypeTags::Array) {
        auto inputView = value::getArrayView(input.value);
        size_t inputSize = inputView->size();
        if (inputSize == 0) {
            return result.releaseToMaybeOwnedRaw();
        }

        resultView->reserve(std::min(inputSize, static_cast<size_t>(n)));
        std::vector<std::pair<value::TypeTags, value::Value>> sortVector;
        for (size_t i = 0; i < inputSize; i++) {
            sortVector.push_back(inputView->getAt(i));
        }
        extractTopOrBottomN(sortVector, static_cast<size_t>(n), resultView, cmp, sense);

        return result.releaseToMaybeOwnedRaw();
    } else if (input.tagIn(value::TypeTags::bsonArray, value::TypeTags::ArraySet)) {
        value::ArrayEnumerator enumerator{input.tag, input.value};
        if (enumerator.atEnd()) {
            return result.releaseToMaybeOwnedRaw();
        }
        // Using intermediate vector since bsonArray and ArraySet don't
        // support reverse iteration.
        std::vector<std::pair<value::TypeTags, value::Value>> inputContents;

        if (input.tag == value::TypeTags::ArraySet) {
            // Reserve space to avoid resizing on push_back calls.
            auto arraySetView = value::getArraySetView(input.value);
            inputContents.reserve(arraySetView->size());
        }

        while (!enumerator.atEnd()) {
            inputContents.push_back(enumerator.getViewOfValue());
            enumerator.advance();
        }

        if (inputContents.size()) {
            size_t nSize = std::min(inputContents.size(), static_cast<size_t>(n));
            resultView->reserve(nSize);
            extractTopOrBottomN(inputContents, static_cast<size_t>(n), resultView, cmp, sense);
        }

        return result.releaseToMaybeOwnedRaw();
    } else {
        // Earlier in this function we bailed out if the 'inputType' wasn't
        // Array, ArraySet or bsonArray, so it should be impossible to reach
        // this point.
        MONGO_UNREACHABLE_TASSERT(1127468);
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinTopN(ArityType arity) {
    return topOrBottomNImpl(arity, TopBottomSense::kTop);
}  // ByteCode::builtinTopN

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinBottomN(ArityType arity) {
    return topOrBottomNImpl(arity, TopBottomSense::kBottom);
}  // ByteCode::builtinBottomN

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinTop(ArityType arity) {
    return topOrBottomImpl(arity, TopBottomSense::kTop);
}  // ByteCode::builtinTop

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinBottom(ArityType arity) {
    return topOrBottomImpl(arity, TopBottomSense::kBottom);
}  // ByteCode::builtinBottom

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinArrayToObject(ArityType arity) {
    tassert(11080063, "Unexpected arity value", arity == 1);

    auto arr = viewFromStack(0);

    if (!value::isArray(arr.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto obj = value::TagValueOwned::fromRaw(value::makeNewObject());
    auto object = value::getObjectView(obj.value());

    value::ArrayEnumerator arrayEnumerator(arr.tag, arr.value);

    // return empty object for empty array
    if (arrayEnumerator.atEnd()) {
        return obj.releaseToMaybeOwnedRaw();
    }

    // There are two accepted input formats in an array: [ [key, val] ] or [ {k:key, v:val} ]. The
    // first array element determines the format for the rest of the array. Mixing input formats is
    // not allowed.
    bool inputArrayFormat;
    auto firstElem = arrayEnumerator.getViewOfValue();
    if (value::isArray(firstElem.tag)) {
        inputArrayFormat = true;
    } else if (value::isObject(firstElem.tag)) {
        inputArrayFormat = false;
    } else {
        uasserted(5153201, "Input to $arrayToObject should be either an array or object");
    }

    // Use a StringMap to store the indices in object for added fieldNames
    // Only the last value should be added for duplicate fieldNames.
    StringMap<int> keyMap{};

    while (!arrayEnumerator.atEnd()) {
        auto elem = arrayEnumerator.getViewOfValue();
        if (inputArrayFormat) {
            uassert(5153202,
                    "$arrayToObject requires a consistent input format. Expected an array",
                    value::isArray(elem.tag));

            value::ArrayEnumerator innerArrayEnum(elem.tag, elem.value);
            uassert(5153203,
                    "$arrayToObject requires an array of size 2 arrays",
                    !innerArrayEnum.atEnd());

            auto key = innerArrayEnum.getViewOfValue();
            uassert(5153204,
                    "$arrayToObject requires an array of key-value pairs, where the key must be of "
                    "type string",
                    value::isString(key.tag));

            innerArrayEnum.advance();
            uassert(5153205,
                    "$arrayToObject requires an array of size 2 arrays",
                    !innerArrayEnum.atEnd());

            auto val = innerArrayEnum.getViewOfValue();

            innerArrayEnum.advance();
            uassert(5153206,
                    "$arrayToObject requires an array of size 2 arrays",
                    innerArrayEnum.atEnd());

            auto keyStringData = value::getStringView(key.tag, key.value);
            uassert(5153207,
                    "Key field cannot contain an embedded null byte",
                    keyStringData.find('\0') == std::string::npos);

            auto valueCopy = value::TagValueOwned::fromRaw(value::copyValue(val.tag, val.value));
            if (keyMap.contains(keyStringData)) {
                auto idx = keyMap[keyStringData];
                auto [copyTag, copyVal] = valueCopy.releaseToRaw();
                object->setAt(idx, copyTag, copyVal);
            } else {
                keyMap[keyStringData] = object->size();
                auto [copyTag, copyVal] = valueCopy.releaseToRaw();
                object->push_back(keyStringData, copyTag, copyVal);
            }
        } else {
            uassert(5153208,
                    "$arrayToObject requires a consistent input format. Expected an object",
                    value::isObject(elem.tag));

            value::ObjectEnumerator innerObjEnum(elem.tag, elem.value);
            uassert(5153209,
                    "$arrayToObject requires an object keys of 'k' and 'v'. "
                    "Found incorrect number of keys",
                    !innerObjEnum.atEnd());

            auto keyName = innerObjEnum.getFieldName();
            auto key = innerObjEnum.getViewOfValue();

            innerObjEnum.advance();
            uassert(5153210,
                    "$arrayToObject requires an object keys of 'k' and 'v'. "
                    "Found incorrect number of keys",
                    !innerObjEnum.atEnd());

            auto valueName = innerObjEnum.getFieldName();
            auto val = innerObjEnum.getViewOfValue();

            innerObjEnum.advance();
            uassert(5153211,
                    "$arrayToObject requires an object keys of 'k' and 'v'. "
                    "Found incorrect number of keys",
                    innerObjEnum.atEnd());

            uassert(5153212,
                    "$arrayToObject requires an object with keys 'k' and 'v'.",
                    ((keyName == "k" && valueName == "v") || (keyName == "k" && valueName == "v")));
            if (keyName == "v" && valueName == "k") {
                std::swap(key, val);
            }

            uassert(5153213,
                    "$arrayToObject requires an object with keys 'k' and 'v', where "
                    "the value of 'k' must be of type string",
                    value::isString(key.tag));

            auto keyStringData = value::getStringView(key.tag, key.value);
            uassert(5153214,
                    "Key field cannot contain an embedded null byte",
                    keyStringData.find('\0') == std::string::npos);

            auto valueCopy = value::TagValueOwned::fromRaw(value::copyValue(val.tag, val.value));
            if (keyMap.contains(keyStringData)) {
                auto idx = keyMap[keyStringData];
                auto [copyTag, copyVal] = valueCopy.releaseToRaw();
                object->setAt(idx, copyTag, copyVal);
            } else {
                keyMap[keyStringData] = object->size();
                auto [copyTag, copyVal] = valueCopy.releaseToRaw();
                object->push_back(keyStringData, copyTag, copyVal);
            }
        }
        arrayEnumerator.advance();
    }
    return obj.releaseToMaybeOwnedRaw();
}  // ByteCode::builtinArrayToObject

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAvgOfArray(ArityType arity) {
    return avgOrSumOfArrayHelper(arity, true);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinMaxOfArray(ArityType arity) {
    return maxMinArrayHelper(arity, true);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinMinOfArray(ArityType arity) {
    return maxMinArrayHelper(arity, false);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::maxMinArrayHelper(ArityType arity,
                                                                           bool isMax) {
    tassert(11080062, "Unexpected arity value", arity == 1);

    auto arr = viewFromStack(0);

    if (!value::isArray(arr.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    value::TypeTags retTag = value::TypeTags::Nothing;
    value::Value retVal = 0;

    value::arrayForEach(arr.tag, arr.value, [&](value::TypeTags elemTag, value::Value elemVal) {
        if (elemTag != value::TypeTags::Null && elemTag != value::TypeTags::bsonUndefined) {
            if (retTag == value::TypeTags::Nothing) {
                retTag = elemTag;
                retVal = elemVal;
            } else {
                auto [compTag, compVal] = value::compareValue(elemTag, elemVal, retTag, retVal);
                if (compTag == value::TypeTags::NumberInt32 &&
                    ((isMax && value::bitcastTo<int32_t>(compVal) == 1) ||
                     (!isMax && value::bitcastTo<int32_t>(compVal) == -1))) {
                    retTag = elemTag;
                    retVal = elemVal;
                }
            }
        }
    });

    auto [outputTag, outputVal] = value::copyValue(retTag, retVal);
    return {true, outputTag, outputVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinStdDevPop(ArityType arity) {
    return stdDevHelper(arity, false);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinStdDevSamp(ArityType arity) {
    return stdDevHelper(arity, true);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::stdDevHelper(ArityType arity,
                                                                      bool isSamp) {
    tassert(11080061, "Unexpected arity value", arity == 1);
    auto arr = viewFromStack(0);
    if (!value::isArray(arr.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // This is an implementation of Welford's online algorithm.
    int64_t count = 0;
    value::TypeTags meanTag = value::TypeTags::NumberInt32;
    value::Value meanVal = 0;
    value::TypeTags meanSquaredTag = value::TypeTags::NumberInt32;
    value::Value meanSquaredVal = 0;
    value::arrayForEach(arr.tag, arr.value, [&](value::TypeTags elemTag, value::Value elemVal) {
        if (value::isNumber(elemTag)) {
            count++;

            auto [deltaOwned, deltaTag, deltaVal] = genericSub(elemTag, elemVal, meanTag, meanVal);
            auto [divOwned, divTag, divVal] =
                genericDiv(deltaTag, deltaVal, value::TypeTags::NumberInt64, count).releaseToRaw();
            auto [newMeanOwned, newMeanTag, newMeanVal] =
                genericAdd(meanTag, meanVal, divTag, divVal);
            meanTag = newMeanTag;
            meanVal = newMeanVal;

            auto [deltaOwned2, deltaTag2, deltaVal2] =
                genericSub(elemTag, elemVal, meanTag, meanVal);
            auto [multOwned, multTag, multVal] =
                genericMul(deltaTag, deltaVal, deltaTag2, deltaVal2);
            auto [newMeanSquaredOwned, newMeanSquaredTag, newMeanSquaredVal] =
                genericAdd(meanSquaredTag, meanSquaredVal, multTag, multVal);
            meanSquaredTag = newMeanSquaredTag;
            meanSquaredVal = newMeanSquaredVal;
        }
    });

    if (count == 0 || (count == 1 && isSamp)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    if (count == 1) {
        return {false, value::TypeTags::NumberInt32, 0};
    }

    if (isSamp) {
        auto [resultOwned, resultTag, resultVal] =
            genericDiv(meanSquaredTag, meanSquaredVal, value::TypeTags::NumberInt64, (count - 1))
                .releaseToRaw();
        return genericSqrt(resultTag, resultVal).releaseToRaw();
    }

    auto [resultOwned, resultTag, resultVal] =
        genericDiv(meanSquaredTag, meanSquaredVal, value::TypeTags::NumberInt64, count)
            .releaseToRaw();
    return genericSqrt(resultTag, resultVal).releaseToRaw();
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSumOfArray(ArityType arity) {
    return avgOrSumOfArrayHelper(arity, false);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::avgOrSumOfArrayHelper(ArityType arity,
                                                                               bool isAvg) {
    tassert(11080060, "Unexpected arity value", arity == 1);
    auto arr = viewFromStack(0);

    if (!value::isArray(arr.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    int64_t count = 0;
    value::TypeTags sumTag = value::TypeTags::NumberInt32;
    value::Value sumVal = 0;
    value::arrayForEach(arr.tag, arr.value, [&](value::TypeTags elemTag, value::Value elemVal) {
        if (value::isNumber(elemTag)) {
            count++;
            auto [partialSumOwned, partialSumTag, partialSumVal] =
                value::genericAdd(sumTag, sumVal, elemTag, elemVal);
            sumTag = partialSumTag;
            sumVal = partialSumVal;
        }
    });

    if (isAvg) {
        if (count == 0) {
            return {false, value::TypeTags::Nothing, 0};
        }
        return genericDiv(sumTag, sumVal, value::TypeTags::NumberInt64, count).releaseToRaw();
    }

    if (sumTag == value::TypeTags::NumberDecimal) {
        auto [outputTag, outputVal] = value::copyValue(sumTag, sumVal);
        return {true, outputTag, outputVal};
    }
    return {false, sumTag, sumVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinUnwindArray(ArityType arity) {
    tassert(11080059, "Unexpected arity value", arity == 1);

    auto arr = viewFromStack(0);

    if (!value::isArray(arr.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    value::ArrayEnumerator arrayEnumerator(arr.tag, arr.value);
    if (arrayEnumerator.atEnd()) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto result = value::TagValueOwned::fromRaw(value::makeNewArray());
    value::Array* resultView = value::getArrayView(result.value());

    while (!arrayEnumerator.atEnd()) {
        auto elem = arrayEnumerator.getViewOfValue();
        if (value::isArray(elem.tag)) {
            value::ArrayEnumerator subArrayEnumerator(elem.tag, elem.value);
            while (!subArrayEnumerator.atEnd()) {
                auto subElem = subArrayEnumerator.getViewOfValue();
                resultView->push_back(value::copyValue(subElem.tag, subElem.value));
                subArrayEnumerator.advance();
            }
        } else {
            resultView->push_back(value::copyValue(elem.tag, elem.value));
        }
        arrayEnumerator.advance();
    }

    return result.releaseToMaybeOwnedRaw();
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinArrayToSet(ArityType arity) {
    tassert(11080058, "Unexpected arity value", arity == 1);
    auto arr = viewFromStack(0);

    if (!value::isArray(arr.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto result = value::TagValueOwned::fromRaw(value::makeNewArraySet());
    value::ArraySet* arrSet = value::getArraySetView(result.value());

    auto sizeView = getArraySize(arr.tag, arr.value);
    tassert(11086810, "Unexpected type of size", sizeView.b == value::TypeTags::NumberInt64);
    arrSet->reserve(static_cast<int64_t>(sizeView.c));

    value::ArrayEnumerator arrayEnumerator(arr.tag, arr.value);
    while (!arrayEnumerator.atEnd()) {
        auto elem = arrayEnumerator.getViewOfValue();
        arrSet->push_back(value::copyValue(elem.tag, elem.value));
        arrayEnumerator.advance();
    }

    return result.releaseToMaybeOwnedRaw();
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollArrayToSet(ArityType arity) {
    tassert(11080057, "Unexpected arity value", arity == 2);

    auto collView = viewFromStack(0);
    if (collView.tag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto arr = viewFromStack(1);

    if (!value::isArray(arr.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto result = value::TagValueOwned::fromRaw(
        value::makeNewArraySet(value::getCollatorView(collView.value)));
    value::ArraySet* arrSet = value::getArraySetView(result.value());

    auto sizeView = getArraySize(arr.tag, arr.value);
    tassert(11086809, "Unexpected type of size", sizeView.b == value::TypeTags::NumberInt64);
    arrSet->reserve(static_cast<int64_t>(sizeView.c));

    value::ArrayEnumerator arrayEnumerator(arr.tag, arr.value);
    while (!arrayEnumerator.atEnd()) {
        auto elem = arrayEnumerator.getViewOfValue();
        arrSet->push_back(value::copyValue(elem.tag, elem.value));
        arrayEnumerator.advance();
    }

    return result.releaseToMaybeOwnedRaw();
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
