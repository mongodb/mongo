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
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo {
namespace sbe {
namespace vm {
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinNewArray(ArityType arity) {
    auto [tag, val] = value::makeNewArray();
    value::ValueGuard guard{tag, val};

    auto arr = value::getArrayView(val);

    if (arity) {
        arr->reserve(arity);
        for (ArityType idx = 0; idx < arity; ++idx) {
            auto [tag, val] = moveOwnedFromStack(idx);
            arr->push_back(tag, val);
        }
    }

    guard.reset();
    return {true, tag, val};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinNewArrayFromRange(ArityType arity) {
    auto [tag, val] = value::makeNewArray();
    value::ValueGuard guard{tag, val};

    auto arr = value::getArrayView(val);

    auto [startOwned, startTag, start] = getFromStack(0);
    auto [endOwned, endTag, end] = getFromStack(1);
    auto [stepOwned, stepTag, step] = getFromStack(2);

    for (auto& tag : {startTag, endTag, stepTag}) {
        if (value::TypeTags::NumberInt32 != tag) {
            return {false, value::TypeTags::Nothing, 0};
        }
    }

    // Cast to broader type 'int64_t' to prevent overflow during loop.
    auto startVal = value::numericCast<int64_t>(startTag, start);
    auto endVal = value::numericCast<int64_t>(endTag, end);
    auto stepVal = value::numericCast<int64_t>(stepTag, step);

    if (stepVal == 0) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Calculate how much memory is needed to generate the array and avoid going over the memLimit.
    auto steps = (endVal - startVal) / stepVal;
    // If steps not positive then no amount of steps can get you from start to end. For example
    // with start=5, end=7, step=-1 steps would be negative and in this case we would return an
    // empty array.
    auto length = steps >= 0 ? 1 + steps : 0;
    int64_t memNeeded = sizeof(value::Array) + length * value::getApproximateSize(startTag, start);
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

    guard.reset();
    return {true, tag, val};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAddToArray(ArityType arity) {
    auto [ownAgg, tagAgg, valAgg] = getFromStack(0);
    auto [tagField, valField] = moveOwnedFromStack(1);
    value::ValueGuard guardField{tagField, valField};

    // Create a new array is it does not exist yet.
    if (tagAgg == value::TypeTags::Nothing) {
        ownAgg = true;
        std::tie(tagAgg, valAgg) = value::makeNewArray();
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }
    value::ValueGuard guard{tagAgg, valAgg};

    invariant(ownAgg && tagAgg == value::TypeTags::Array);
    auto arr = value::getArrayView(valAgg);

    // Push back the value. Note that array will ignore Nothing.
    arr->push_back(tagField, valField);
    guardField.reset();

    guard.reset();
    return {ownAgg, tagAgg, valAgg};
}

// The value being accumulated is an SBE array that contains an integer and the accumulated array,
// where the integer is the total size in bytes of the elements in the array.
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAddToArrayCapped(ArityType arity) {
    auto [ownArr, tagArr, valArr] = getFromStack(0);
    auto [tagNewElem, valNewElem] = moveOwnedFromStack(1);
    value::ValueGuard guardNewElem{tagNewElem, valNewElem};
    auto [_, tagSizeCap, valSizeCap] = getFromStack(2);

    if (tagSizeCap != value::TypeTags::NumberInt32) {
        auto [ownArr, tagArr, valArr] = getFromStack(0);
        topStack(false, value::TypeTags::Nothing, 0);
        return {ownArr, tagArr, valArr};
    }
    const int32_t sizeCap = value::bitcastTo<int32_t>(valSizeCap);

    // Create a new array to hold size and added elements, if is it does not exist yet.
    if (tagArr == value::TypeTags::Nothing) {
        ownArr = true;
        std::tie(tagArr, valArr) = value::makeNewArray();
        auto arr = value::getArrayView(valArr);

        auto [tagAccArr, valAccArr] = value::makeNewArray();

        // The order is important! The accumulated array should be at index
        // AggArrayWithSize::kValues, and the size should be at index
        // AggArrayWithSize::kSizeOfValues.
        arr->push_back(tagAccArr, valAccArr);
        arr->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }
    value::ValueGuard guardArr{tagArr, valArr};

    invariant(ownArr && tagArr == value::TypeTags::Array);
    auto arr = value::getArrayView(valArr);
    invariant(arr->size() == static_cast<size_t>(AggArrayWithSize::kLast));

    // Check that the accumulated size of the array doesn't exceed the limit.
    int elemSize = value::getApproximateSize(tagNewElem, valNewElem);
    auto [tagAccSize, valAccSize] =
        arr->getAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues));
    invariant(tagAccSize == value::TypeTags::NumberInt64);
    const int64_t currentSize = value::bitcastTo<int64_t>(valAccSize);
    const int64_t newSize = currentSize + elemSize;

    auto [tagAccArr, valAccArr] = arr->getAt(static_cast<size_t>(AggArrayWithSize::kValues));
    auto accArr = value::getArrayView(valAccArr);
    if (newSize >= static_cast<int64_t>(sizeCap)) {
        uasserted(ErrorCodes::ExceededMemoryLimit,
                  str::stream() << "Used too much memory for a single array. Memory limit: "
                                << sizeCap << " bytes. The array contains " << accArr->size()
                                << " elements and is of size " << currentSize
                                << " bytes. The element being added has size " << elemSize
                                << " bytes.");
    }

    arr->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
               value::TypeTags::NumberInt64,
               value::bitcastFrom<int64_t>(newSize));

    // Push back the new value. Note that array will ignore Nothing.
    guardNewElem.reset();
    accArr->push_back(tagNewElem, valNewElem);

    guardArr.reset();
    return {ownArr, tagArr, valArr};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinConcatArrays(ArityType arity) {
    auto [resTag, resVal] = value::makeNewArray();
    value::ValueGuard resGuard{resTag, resVal};
    auto resView = value::getArrayView(resVal);

    for (ArityType idx = 0; idx < arity; ++idx) {
        auto [_, tag, val] = getFromStack(idx);
        if (!value::isArray(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        value::arrayForEach(tag, val, [&](value::TypeTags elTag, value::Value elVal) {
            auto [copyTag, copyVal] = value::copyValue(elTag, elVal);
            resView->push_back(copyTag, copyVal);
        });
    }

    resGuard.reset();

    return {true, resTag, resVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinConcatArraysCapped(
    ArityType arity) {
    auto [newElemTag, newElemVal] = moveOwnedFromStack(1);
    // Note that we do not call 'reset()' on the guard below, as 'concatArraysAccumImpl' assumes
    // that callers will manage the memory associated with 'newElemTag/Val'. See the comment on
    // 'concatArraysAccumImpl' for more details.
    value::ValueGuard newElemGuard{newElemTag, newElemVal};
    auto [_, sizeCapTag, sizeCapVal] = getFromStack(2);
    if (sizeCapTag != value::TypeTags::NumberInt32) {
        auto [arrOwned, arrTag, arrVal] = getFromStack(0);
        topStack(false, value::TypeTags::Nothing, 0);
        return {arrOwned, arrTag, arrVal};
    }

    auto [arrOwned, arrTag, arrVal] = getFromStack(0);
    return concatArraysAccumImpl(newElemTag,
                                 newElemVal,
                                 value::bitcastTo<int32_t>(sizeCapVal),
                                 arrOwned,
                                 arrTag,
                                 arrVal,
                                 value::getApproximateSize(newElemTag, newElemVal));
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
    invariant(arity == 2);
    auto [_, exprTag, exprVal] = getFromStack(0);
    auto [__, arrTag, arrVal] = getFromStack(1);

    return ByteCode::isMemberImpl(exprTag, exprVal, arrTag, arrVal, nullptr);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollIsMember(ArityType arity) {
    invariant(arity == 3);
    auto [_, exprTag, exprVal] = getFromStack(0);
    auto [__, arrTag, arrVal] = getFromStack(1);

    CollatorInterface* collator = nullptr;
    auto [collatorOwned, collatorType, collatorVal] = getFromStack(2);

    if (collatorType == value::TypeTags::collator) {
        collator = value::getCollatorView(collatorVal);
    } else {
        // If a third parameter was supplied but it is not a Collator, return Nothing.
        return {false, value::TypeTags::Nothing, 0};
    }

    return ByteCode::isMemberImpl(exprTag, exprVal, arrTag, arrVal, collator);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinExtractSubArray(ArityType arity) {
    // We need to ensure that 'size_t' is wide enough to store 32-bit index.
    static_assert(sizeof(size_t) >= sizeof(int32_t), "size_t must be at least 32-bits");

    auto [arrayOwned, arrayTag, arrayValue] = getFromStack(0);
    auto [limitOwned, limitTag, limitValue] = getFromStack(1);

    if (!value::isArray(arrayTag) || limitTag != value::TypeTags::NumberInt32) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto limit = value::bitcastTo<int32_t>(limitValue);

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

        auto [skipOwned, skipTag, skipValue] = getFromStack(2);
        if (skipTag != value::TypeTags::NumberInt32) {
            return {false, value::TypeTags::Nothing, 0};
        }

        auto skip = value::bitcastTo<int32_t>(skipValue);
        std::tie(isNegativeStart, start) = absWithSign(skip);
    }

    auto [resultTag, resultValue] = value::makeNewArray();
    value::ValueGuard resultGuard{resultTag, resultValue};
    auto resultView = value::getArrayView(resultValue);

    if (arrayTag == value::TypeTags::Array) {
        auto arrayView = value::getArrayView(arrayValue);
        auto arraySize = arrayView->size();

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
                auto [tag, value] = arrayView->getAt(i);
                auto [copyTag, copyValue] = value::copyValue(tag, value);
                resultView->push_back(copyTag, copyValue);
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

        value::ArrayEnumerator startEnumerator{arrayTag, arrayValue};
        if (isNegativeStart) {
            value::ArrayEnumerator windowEndEnumerator{arrayTag, arrayValue};
            advance(windowEndEnumerator, start);

            while (!startEnumerator.atEnd() && !windowEndEnumerator.atEnd()) {
                startEnumerator.advance();
                windowEndEnumerator.advance();
            }
            invariant(windowEndEnumerator.atEnd());
        } else {
            advance(startEnumerator, start);
        }

        size_t i = 0;
        while (i < length && !startEnumerator.atEnd()) {
            auto [tag, value] = startEnumerator.getViewOfValue();
            auto [copyTag, copyValue] = value::copyValue(tag, value);
            resultView->push_back(copyTag, copyValue);

            i++;
            startEnumerator.advance();
        }
    }

    resultGuard.reset();
    return {true, resultTag, resultValue};
}  // ByteCode::builtinExtractSubArray

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinIsArrayEmpty(ArityType arity) {
    invariant(arity == 1);
    auto [arrayOwned, arrayType, arrayValue] = getFromStack(0);

    if (!value::isArray(arrayType)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    if (arrayType == value::TypeTags::Array) {
        auto arrayView = value::getArrayView(arrayValue);
        return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(arrayView->size() == 0)};
    } else if (arrayType == value::TypeTags::bsonArray || arrayType == value::TypeTags::ArraySet) {
        value::ArrayEnumerator enumerator(arrayType, arrayValue);
        return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(enumerator.atEnd())};
    } else {
        // Earlier in this function we bailed out if the 'arrayType' wasn't Array, ArraySet or
        // bsonArray, so it should be impossible to reach this point.
        MONGO_UNREACHABLE
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinReverseArray(ArityType arity) {
    invariant(arity == 1);
    auto [inputOwned, inputType, inputVal] = getFromStack(0);

    if (!value::isArray(inputType)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [resultTag, resultVal] = value::makeNewArray();
    auto resultView = value::getArrayView(resultVal);
    value::ValueGuard resultGuard{resultTag, resultVal};

    if (inputType == value::TypeTags::Array) {
        auto inputView = value::getArrayView(inputVal);
        size_t inputSize = inputView->size();
        if (inputSize) {
            resultView->reserve(inputSize);
            for (size_t i = 0; i < inputSize; i++) {
                auto [origTag, origVal] = inputView->getAt(inputSize - 1 - i);
                auto [copyTag, copyVal] = copyValue(origTag, origVal);
                resultView->push_back(copyTag, copyVal);
            }
        }

        resultGuard.reset();
        return {true, resultTag, resultVal};
    } else if (inputType == value::TypeTags::bsonArray || inputType == value::TypeTags::ArraySet) {
        // Using intermediate vector since bsonArray and ArraySet don't
        // support reverse iteration.
        std::vector<std::pair<value::TypeTags, value::Value>> inputContents;

        if (inputType == value::TypeTags::ArraySet) {
            // Reserve space to avoid resizing on push_back calls.
            auto arraySetView = value::getArraySetView(inputVal);
            inputContents.reserve(arraySetView->size());
        }

        value::arrayForEach(inputType, inputVal, [&](value::TypeTags elTag, value::Value elVal) {
            inputContents.push_back({elTag, elVal});
        });

        if (inputContents.size()) {
            resultView->reserve(inputContents.size());

            // Run through the array backwards and copy into the result array.
            for (auto it = inputContents.rbegin(); it != inputContents.rend(); ++it) {
                auto [copyTag, copyVal] = copyValue(it->first, it->second);
                resultView->push_back(copyTag, copyVal);
            }
        }

        resultGuard.reset();
        return {true, resultTag, resultVal};
    } else {
        // Earlier in this function we bailed out if the 'inputType' wasn't
        // Array, ArraySet or bsonArray, so it should be impossible to reach
        // this point.
        MONGO_UNREACHABLE;
    }
}  // ByteCode::builtinReverseArray

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSortArray(ArityType arity) {
    invariant(arity == 2 || arity == 3);
    auto [inputOwned, inputType, inputVal] = getFromStack(0);

    if (!value::isArray(inputType)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [specOwned, specTag, specVal] = getFromStack(1);

    if (!value::isObject(specTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    CollatorInterface* collator = nullptr;
    if (arity == 3) {
        auto [collatorOwned, collatorType, collatorVal] = getFromStack(2);

        if (collatorType == value::TypeTags::collator) {
            collator = value::getCollatorView(collatorVal);
        } else {
            // If a third parameter was supplied but it is not a Collator, return Nothing.
            return {false, value::TypeTags::Nothing, 0};
        }
    }

    auto cmp = SbePatternValueCmp(specTag, specVal, collator);

    auto [resultTag, resultVal] = value::makeNewArray();
    auto resultView = value::getArrayView(resultVal);
    value::ValueGuard resultGuard{resultTag, resultVal};

    if (inputType == value::TypeTags::Array) {
        auto inputView = value::getArrayView(inputVal);
        size_t inputSize = inputView->size();
        if (inputSize) {
            resultView->reserve(inputSize);
            std::vector<std::pair<value::TypeTags, value::Value>> sortVector;
            for (size_t i = 0; i < inputSize; i++) {
                sortVector.push_back(inputView->getAt(i));
            }
            std::sort(sortVector.begin(), sortVector.end(), cmp);

            for (size_t i = 0; i < inputSize; i++) {
                auto [tag, val] = sortVector[i];
                auto [copyTag, copyVal] = copyValue(tag, val);
                resultView->push_back(copyTag, copyVal);
            }
        }

        resultGuard.reset();
        return {true, resultTag, resultVal};
    } else if (inputType == value::TypeTags::bsonArray || inputType == value::TypeTags::ArraySet) {
        value::ArrayEnumerator enumerator{inputType, inputVal};

        // Using intermediate vector since bsonArray and ArraySet don't
        // support reverse iteration.
        std::vector<std::pair<value::TypeTags, value::Value>> inputContents;

        if (inputType == value::TypeTags::ArraySet) {
            // Reserve space to avoid resizing on push_back calls.
            auto arraySetView = value::getArraySetView(inputVal);
            inputContents.reserve(arraySetView->size());
        }

        while (!enumerator.atEnd()) {
            inputContents.push_back(enumerator.getViewOfValue());
            enumerator.advance();
        }

        std::sort(inputContents.begin(), inputContents.end(), cmp);

        if (inputContents.size()) {
            resultView->reserve(inputContents.size());

            for (auto it = inputContents.begin(); it != inputContents.end(); ++it) {
                auto [copyTag, copyVal] = copyValue(it->first, it->second);
                resultView->push_back(copyTag, copyVal);
            }
        }

        resultGuard.reset();
        return {true, resultTag, resultVal};
    } else {
        // Earlier in this function we bailed out if the 'inputType' wasn't
        // Array, ArraySet or bsonArray, so it should be impossible to reach
        // this point.
        MONGO_UNREACHABLE;
    }
}  // ByteCode::builtinSortArray

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinArrayToObject(ArityType arity) {
    invariant(arity == 1);

    auto [arrOwned, arrTag, arrVal] = getFromStack(0);

    if (!value::isArray(arrTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [objTag, objVal] = value::makeNewObject();
    value::ValueGuard objGuard{objTag, objVal};
    auto object = value::getObjectView(objVal);

    value::ArrayEnumerator arrayEnumerator(arrTag, arrVal);

    // return empty object for empty array
    if (arrayEnumerator.atEnd()) {
        objGuard.reset();
        return {true, objTag, objVal};
    }

    // There are two accepted input formats in an array: [ [key, val] ] or [ {k:key, v:val} ]. The
    // first array element determines the format for the rest of the array. Mixing input formats is
    // not allowed.
    bool inputArrayFormat;
    auto [firstElemTag, firstElemVal] = arrayEnumerator.getViewOfValue();
    if (value::isArray(firstElemTag)) {
        inputArrayFormat = true;
    } else if (value::isObject(firstElemTag)) {
        inputArrayFormat = false;
    } else {
        uasserted(5153201, "Input to $arrayToObject should be either an array or object");
    }

    // Use a StringMap to store the indices in object for added fieldNames
    // Only the last value should be added for duplicate fieldNames.
    StringMap<int> keyMap{};

    while (!arrayEnumerator.atEnd()) {
        auto [elemTag, elemVal] = arrayEnumerator.getViewOfValue();
        if (inputArrayFormat) {
            uassert(5153202,
                    "$arrayToObject requires a consistent input format. Expected an array",
                    value::isArray(elemTag));

            value::ArrayEnumerator innerArrayEnum(elemTag, elemVal);
            uassert(5153203,
                    "$arrayToObject requires an array of size 2 arrays",
                    !innerArrayEnum.atEnd());

            auto [keyTag, keyVal] = innerArrayEnum.getViewOfValue();
            uassert(5153204,
                    "$arrayToObject requires an array of key-value pairs, where the key must be of "
                    "type string",
                    value::isString(keyTag));

            innerArrayEnum.advance();
            uassert(5153205,
                    "$arrayToObject requires an array of size 2 arrays",
                    !innerArrayEnum.atEnd());

            auto [valueTag, valueVal] = innerArrayEnum.getViewOfValue();

            innerArrayEnum.advance();
            uassert(5153206,
                    "$arrayToObject requires an array of size 2 arrays",
                    innerArrayEnum.atEnd());

            auto keyStringData = value::getStringView(keyTag, keyVal);
            uassert(5153207,
                    "Key field cannot contain an embedded null byte",
                    keyStringData.find('\0') == std::string::npos);

            auto [valueCopyTag, valueCopyVal] = value::copyValue(valueTag, valueVal);
            if (keyMap.contains(keyStringData)) {
                auto idx = keyMap[keyStringData];
                object->setAt(idx, valueCopyTag, valueCopyVal);
            } else {
                keyMap[keyStringData] = object->size();
                object->push_back(keyStringData, valueCopyTag, valueCopyVal);
            }
        } else {
            uassert(5153208,
                    "$arrayToObject requires a consistent input format. Expected an object",
                    value::isObject(elemTag));

            value::ObjectEnumerator innerObjEnum(elemTag, elemVal);
            uassert(5153209,
                    "$arrayToObject requires an object keys of 'k' and 'v'. "
                    "Found incorrect number of keys",
                    !innerObjEnum.atEnd());

            auto keyName = innerObjEnum.getFieldName();
            auto [keyTag, keyVal] = innerObjEnum.getViewOfValue();

            innerObjEnum.advance();
            uassert(5153210,
                    "$arrayToObject requires an object keys of 'k' and 'v'. "
                    "Found incorrect number of keys",
                    !innerObjEnum.atEnd());

            auto valueName = innerObjEnum.getFieldName();
            auto [valueTag, valueVal] = innerObjEnum.getViewOfValue();

            innerObjEnum.advance();
            uassert(5153211,
                    "$arrayToObject requires an object keys of 'k' and 'v'. "
                    "Found incorrect number of keys",
                    innerObjEnum.atEnd());

            uassert(5153212,
                    "$arrayToObject requires an object with keys 'k' and 'v'.",
                    ((keyName == "k" && valueName == "v") || (keyName == "k" && valueName == "v")));
            if (keyName == "v" && valueName == "k") {
                std::swap(keyTag, valueTag);
                std::swap(keyVal, valueVal);
            }

            uassert(5153213,
                    "$arrayToObject requires an object with keys 'k' and 'v', where "
                    "the value of 'k' must be of type string",
                    value::isString(keyTag));

            auto keyStringData = value::getStringView(keyTag, keyVal);
            uassert(5153214,
                    "Key field cannot contain an embedded null byte",
                    keyStringData.find('\0') == std::string::npos);

            auto [valueCopyTag, valueCopyVal] = value::copyValue(valueTag, valueVal);
            if (keyMap.contains(keyStringData)) {
                auto idx = keyMap[keyStringData];
                object->setAt(idx, valueCopyTag, valueCopyVal);
            } else {
                keyMap[keyStringData] = object->size();
                object->push_back(keyStringData, valueCopyTag, valueCopyVal);
            }
        }
        arrayEnumerator.advance();
    }
    objGuard.reset();
    return {true, objTag, objVal};
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
    invariant(arity == 1);

    auto [arrOwned, arrTag, arrVal] = getFromStack(0);

    if (!value::isArray(arrTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    value::TypeTags retTag = value::TypeTags::Nothing;
    value::Value retVal = 0;

    value::arrayForEach(arrTag, arrVal, [&](value::TypeTags elemTag, value::Value elemVal) {
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
    invariant(arity == 1);
    auto [arrOwned, arrTag, arrVal] = getFromStack(0);
    if (!value::isArray(arrTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // This is an implementation of Welford's online algorithm.
    int64_t count = 0;
    value::TypeTags meanTag = value::TypeTags::NumberInt32;
    value::Value meanVal = 0;
    value::TypeTags meanSquaredTag = value::TypeTags::NumberInt32;
    value::Value meanSquaredVal = 0;
    value::arrayForEach(arrTag, arrVal, [&](value::TypeTags elemTag, value::Value elemVal) {
        if (value::isNumber(elemTag)) {
            count++;

            auto [deltaOwned, deltaTag, deltaVal] = genericSub(elemTag, elemVal, meanTag, meanVal);
            auto [divOwned, divTag, divVal] =
                genericDiv(deltaTag, deltaVal, value::TypeTags::NumberInt64, count);
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
            genericDiv(meanSquaredTag, meanSquaredVal, value::TypeTags::NumberInt64, (count - 1));
        return genericSqrt(resultTag, resultVal);
    }

    auto [resultOwned, resultTag, resultVal] =
        genericDiv(meanSquaredTag, meanSquaredVal, value::TypeTags::NumberInt64, count);
    return genericSqrt(resultTag, resultVal);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSumOfArray(ArityType arity) {
    return avgOrSumOfArrayHelper(arity, false);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::avgOrSumOfArrayHelper(ArityType arity,
                                                                               bool isAvg) {
    invariant(arity == 1);
    auto [arrOwned, arrTag, arrVal] = getFromStack(0);

    if (!value::isArray(arrTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    int64_t count = 0;
    value::TypeTags sumTag = value::TypeTags::NumberInt32;
    value::Value sumVal = 0;
    value::arrayForEach(arrTag, arrVal, [&](value::TypeTags elemTag, value::Value elemVal) {
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
        return genericDiv(sumTag, sumVal, value::TypeTags::NumberInt64, count);
    }

    if (sumTag == value::TypeTags::NumberDecimal) {
        auto [outputTag, outputVal] = value::copyValue(sumTag, sumVal);
        return {true, outputTag, outputVal};
    }
    return {false, sumTag, sumVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinUnwindArray(ArityType arity) {
    invariant(arity == 1);

    auto [arrOwned, arrTag, arrVal] = getFromStack(0);

    if (!value::isArray(arrTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    value::ArrayEnumerator arrayEnumerator(arrTag, arrVal);
    if (arrayEnumerator.atEnd()) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [resultTag, resultVal] = value::makeNewArray();
    value::ValueGuard resultGuard{resultTag, resultVal};
    value::Array* result = value::getArrayView(resultVal);

    while (!arrayEnumerator.atEnd()) {
        auto [elemTag, elemVal] = arrayEnumerator.getViewOfValue();
        if (value::isArray(elemTag)) {
            value::ArrayEnumerator subArrayEnumerator(elemTag, elemVal);
            while (!subArrayEnumerator.atEnd()) {
                auto [subElemTag, subElemVal] = subArrayEnumerator.getViewOfValue();
                result->push_back(value::copyValue(subElemTag, subElemVal));
                subArrayEnumerator.advance();
            }
        } else {
            result->push_back(value::copyValue(elemTag, elemVal));
        }
        arrayEnumerator.advance();
    }

    resultGuard.reset();
    return {true, resultTag, resultVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinArrayToSet(ArityType arity) {
    invariant(arity == 1);
    auto [arrOwned, arrTag, arrVal] = getFromStack(0);

    if (!value::isArray(arrTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [tag, val] = value::makeNewArraySet();
    value::ValueGuard guard{tag, val};

    value::ArraySet* arrSet = value::getArraySetView(val);

    auto [sizeOwned, sizeTag, sizeVal] = getArraySize(arrTag, arrVal);
    invariant(sizeTag == value::TypeTags::NumberInt64);
    arrSet->reserve(static_cast<int64_t>(sizeVal));

    value::ArrayEnumerator arrayEnumerator(arrTag, arrVal);
    while (!arrayEnumerator.atEnd()) {
        auto [elemTag, elemVal] = arrayEnumerator.getViewOfValue();
        arrSet->push_back(value::copyValue(elemTag, elemVal));
        arrayEnumerator.advance();
    }

    guard.reset();
    return {true, tag, val};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollArrayToSet(ArityType arity) {
    invariant(arity == 2);

    auto [_, collTag, collVal] = getFromStack(0);
    if (collTag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [arrOwned, arrTag, arrVal] = getFromStack(1);

    if (!value::isArray(arrTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [tag, val] = value::makeNewArraySet(value::getCollatorView(collVal));
    value::ValueGuard guard{tag, val};

    value::ArraySet* arrSet = value::getArraySetView(val);

    auto [sizeOwned, sizeTag, sizeVal] = getArraySize(arrTag, arrVal);
    invariant(sizeTag == value::TypeTags::NumberInt64);
    arrSet->reserve(static_cast<int64_t>(sizeVal));

    value::ArrayEnumerator arrayEnumerator(arrTag, arrVal);
    while (!arrayEnumerator.atEnd()) {
        auto [elemTag, elemVal] = arrayEnumerator.getViewOfValue();
        arrSet->push_back(value::copyValue(elemTag, elemVal));
        arrayEnumerator.advance();
    }

    guard.reset();
    return {true, tag, val};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
