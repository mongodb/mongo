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

#include "mongo/db/exec/sbe/vm/vm.h"

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace sbe {
namespace vm {
void ByteCode::allocStackImpl(size_t newSizeDelta) noexcept {
    invariant(newSizeDelta > 0);

    auto oldSize = _argStackEnd - _argStack;
    auto oldTop = _argStackTop - _argStack;

    auto newSize = oldSize + newSizeDelta;
    uint8_t* newArgStack = static_cast<uint8_t*>(::operator new(newSize));
    memcpy(newArgStack, _argStack, oldSize);
    ::operator delete(_argStack, oldSize);

    _argStack = newArgStack;
    _argStackEnd = _argStack + newSize;
    _argStackTop = _argStack + oldTop;
}

ByteCode::TopBottomArgs::~TopBottomArgs() {}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::getField(value::TypeTags objTag,
                                                                  value::Value objValue,
                                                                  value::TypeTags fieldTag,
                                                                  value::Value fieldValue) {
    if (!value::isString(fieldTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto fieldStr = value::getStringView(fieldTag, fieldValue);

    return getField(objTag, objValue, fieldStr);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::getField(value::TypeTags objTag,
                                                                  value::Value objValue,
                                                                  StringData fieldStr) {
    if (MONGO_likely(objTag == value::TypeTags::bsonObject)) {
        auto be = value::bitcastTo<const char*>(objValue);
        const auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
        // Skip document length.
        be += 4;
        while (be != end - 1) {
            auto sv = bson::fieldNameAndLength(be);

            if (sv == fieldStr) {
                auto [tag, val] = bson::convertFrom<true>(be, end, fieldStr.size());
                return {false, tag, val};
            }

            be = bson::advance(be, sv.size());
        }
    } else if (objTag == value::TypeTags::Object) {
        auto [tag, val] = value::getObjectView(objValue)->getField(fieldStr);
        return {false, tag, val};
    }
    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::getElement(value::TypeTags arrTag,
                                                                    value::Value arrValue,
                                                                    value::TypeTags idxTag,
                                                                    value::Value idxValue) {
    // We need to ensure that 'size_t' is wide enough to store 32-bit index.
    static_assert(sizeof(size_t) >= sizeof(int32_t), "size_t must be at least 32-bits");

    if (!value::isArray(arrTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    if (idxTag != value::TypeTags::NumberInt32) {
        return {false, value::TypeTags::Nothing, 0};
    }

    const auto idxInt32 = value::bitcastTo<int32_t>(idxValue);
    const bool isNegative = idxInt32 < 0;

    size_t idx = 0;
    if (isNegative) {
        // Upcast 'idxInt32' to 'int64_t' prevent overflow during the sign change.
        idx = static_cast<size_t>(-static_cast<int64_t>(idxInt32));
    } else {
        idx = static_cast<size_t>(idxInt32);
    }

    if (arrTag == value::TypeTags::Array) {
        // If 'arr' is an SBE array, use Array::getAt() to retrieve the element at index 'idx'.
        auto arrayView = value::getArrayView(arrValue);

        size_t convertedIdx = idx;
        if (isNegative) {
            if (idx > arrayView->size()) {
                return {false, value::TypeTags::Nothing, 0};
            }
            convertedIdx = arrayView->size() - idx;
        }

        auto [tag, val] = value::getArrayView(arrValue)->getAt(convertedIdx);
        return {false, tag, val};
    } else if (arrTag == value::TypeTags::bsonArray || arrTag == value::TypeTags::ArraySet ||
               arrTag == value::TypeTags::ArrayMultiSet) {
        value::ArrayEnumerator enumerator(arrTag, arrValue);

        if (!isNegative) {
            // Loop through array until we meet element at position 'idx'.
            size_t i = 0;
            while (i < idx && !enumerator.atEnd()) {
                i++;
                enumerator.advance();
            }
            // If the array didn't have an element at index 'idx', return Nothing.
            if (enumerator.atEnd()) {
                return {false, value::TypeTags::Nothing, 0};
            }
            auto [tag, val] = enumerator.getViewOfValue();
            return {false, tag, val};
        }

        // For negative indexes we use two pointers approach. We start two array enumerators at the
        // distance of 'idx' and move them at the same time. Once one of the enumerators reaches the
        // end of the array, the second one points to the element at position '-idx'.
        //
        // First, move one of the enumerators 'idx' elements forward.
        size_t i = 0;
        while (i < idx && !enumerator.atEnd()) {
            enumerator.advance();
            i++;
        }

        if (i != idx) {
            // Array is too small to have an element at the requested index.
            return {false, value::TypeTags::Nothing, 0};
        }

        // Initiate second enumerator at the start of the array. Now the distance between
        // 'enumerator' and 'windowEndEnumerator' is exactly 'idx' elements. Move both enumerators
        // until the first one reaches the end of the array.
        value::ArrayEnumerator windowEndEnumerator(arrTag, arrValue);
        while (!enumerator.atEnd() && !windowEndEnumerator.atEnd()) {
            enumerator.advance();
            windowEndEnumerator.advance();
        }
        invariant(enumerator.atEnd());
        invariant(!windowEndEnumerator.atEnd());

        auto [tag, val] = windowEndEnumerator.getViewOfValue();
        return {false, tag, val};
    } else {
        // Earlier in this function we bailed out if the 'arrTag' wasn't Array, ArraySet or
        // bsonArray, so it should be impossible to reach this point.
        MONGO_UNREACHABLE
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::getFieldOrElement(
    value::TypeTags objTag,
    value::Value objValue,
    value::TypeTags fieldTag,
    value::Value fieldValue) {
    // If this is an array and we can convert the "field name" to a reasonable number then treat
    // this as getElement call.
    if (value::isArray(objTag) && value::isString(fieldTag)) {
        int idx;
        auto status = NumberParser{}(value::getStringView(fieldTag, fieldValue), &idx);
        if (!status.isOK()) {
            return {false, value::TypeTags::Nothing, 0};
        }
        return getElement(
            objTag, objValue, value::TypeTags::NumberInt32, value::bitcastFrom<int>(idx));
    } else {
        return getField(objTag, objValue, fieldTag, fieldValue);
    }
}

void ByteCode::traverseP(const CodeFragment* code) {
    // Traverse a projection path - evaluate the input lambda on every element of the input array.
    // The traversal is recursive; i.e. we visit nested arrays if any.
    auto [maxDepthOwn, maxDepthTag, maxDepthVal] = getFromStack(0);
    popAndReleaseStack();
    auto [lamOwn, lamTag, lamVal] = getFromStack(0);
    popAndReleaseStack();

    if ((maxDepthTag != value::TypeTags::Nothing && maxDepthTag != value::TypeTags::NumberInt32) ||
        lamTag != value::TypeTags::LocalLambda) {
        popAndReleaseStack();
        pushStack(false, value::TypeTags::Nothing, 0);
        return;
    }

    int64_t lamPos = value::bitcastTo<int64_t>(lamVal);
    int64_t maxDepth = maxDepthTag == value::TypeTags::NumberInt32
        ? value::bitcastTo<int32_t>(maxDepthVal)
        : std::numeric_limits<int64_t>::max();

    traverseP(code, lamPos, maxDepth);
}

void ByteCode::traverseP(const CodeFragment* code, int64_t position, int64_t maxDepth) {
    auto [own, tag, val] = getFromStack(0);

    if (value::isArray(tag) && maxDepth > 0) {
        value::ValueGuard input(own, tag, val);
        popStack();

        if (maxDepth != std::numeric_limits<int64_t>::max()) {
            --maxDepth;
        }

        traverseP_nested(code, position, tag, val, maxDepth);
    } else {
        runLambdaInternal(code, position);
    }
}

void ByteCode::traverseP_nested(const CodeFragment* code,
                                int64_t position,
                                value::TypeTags tagInput,
                                value::Value valInput,
                                int64_t maxDepth) {
    auto decrement = [](int64_t d) {
        return d == std::numeric_limits<int64_t>::max() ? d : d - 1;
    };

    auto [tagArrOutput, valArrOutput] = value::makeNewArray();
    auto arrOutput = value::getArrayView(valArrOutput);
    value::ValueGuard guard{tagArrOutput, valArrOutput};

    value::arrayForEach(tagInput, valInput, [&](value::TypeTags elemTag, value::Value elemVal) {
        if (maxDepth > 0 && value::isArray(elemTag)) {
            traverseP_nested(code, position, elemTag, elemVal, decrement(maxDepth));
        } else {
            pushStack(false, elemTag, elemVal);
            runLambdaInternal(code, position);
        }

        auto [retOwn, retTag, retVal] = getFromStack(0);
        popStack();
        if (!retOwn) {
            auto [copyTag, copyVal] = value::copyValue(retTag, retVal);
            retTag = copyTag;
            retVal = copyVal;
        }
        arrOutput->push_back(retTag, retVal);
    });
    guard.reset();
    pushStack(true, tagArrOutput, valArrOutput);
}

void ByteCode::magicTraverseF(const CodeFragment* code) {
    // A combined filter traversal (i.e. non-recursive visit of both array elements and the array
    // itself) with getField/getElement to simulate numeric paths.
    // The semantics are controlled by 2 runtime conditions:
    // 1. is a value to be examined coming from an object (i.e. getField) or from an array (i.e.
    // getElement)? Values originating from objects are further traversed whereas array values are
    // not.
    // 2. is this traversal at the leaf position of the path? If so then the further object
    // traversals are followed. Otherwise there is no further traversals.
    auto [ownFlag, tagFlag, valFlag] = getFromStack(0, true);
    value::ValueGuard firstGuard{ownFlag, tagFlag, valFlag};
    auto [lamOwn, lamTag, lamVal] = getFromStack(0, true);
    value::ValueGuard lamGuard{lamOwn, lamTag, lamVal};
    auto arrayIndex = getFromStack(0, true);
    value::ValueGuard indexGuard{arrayIndex};
    auto fieldName = getFromStack(0, true);
    value::ValueGuard fieldGuard{fieldName};
    auto [ownInput, tagInput, valInput] = getFromStack(0, true);
    value::ValueGuard inputGuard{ownInput, tagInput, valInput};

    const bool preTraverse = value::bitcastTo<int32_t>(valFlag) & MagicTraverse::kPreTraverse;
    const bool postTraverse = value::bitcastTo<int32_t>(valFlag) & MagicTraverse::kPostTraverse;

    auto lambdaPtr = value::bitcastTo<int64_t>(lamVal);

    enum class Traverse { document, array };
    auto innerTraverse = [&](value::TypeTags tagElem,
                             value::Value valElem,
                             Traverse type,
                             bool nested) {
        auto [ownArrayIndex, tagArrayIndex, valArrayIndex] = arrayIndex;
        auto [ownFieldName, tagFieldName, valFieldName] = fieldName;

        auto [ownInner, tagInner, valInner] = type == Traverse::document
            ? getField(tagElem, valElem, tagFieldName, valFieldName)
            : getElement(tagElem, valElem, tagArrayIndex, valArrayIndex);

        // Follow on with a traversal only if the flag is set.
        if (value::isArray(tagInner) && nested) {
            const bool passed = value::arrayAny(
                tagInner, valInner, [&](value::TypeTags tagElem, value::Value valElem) {
                    pushStack(false, tagElem, valElem);
                    if (runLambdaPredicate(code, lambdaPtr)) {
                        pushStack(false, value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
                        return true;
                    }
                    return false;
                });
            if (passed) {
                return passed;
            }
        }
        pushStack(ownInner, tagInner, valInner);
        if (runLambdaPredicate(code, lambdaPtr)) {
            pushStack(false, value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
            return true;
        }
        return false;
    };

    if (value::isArray(tagInput)) {
        const bool passed =
            value::arrayAny(tagInput, valInput, [&](value::TypeTags tagElem, value::Value valElem) {
                return innerTraverse(tagElem, valElem, Traverse::document, preTraverse);
            });

        if (passed) {
            return;
        }

        // For values originating from arrays we do not run the inner traversal unless the flag is
        // set.
        if (!innerTraverse(tagInput, valInput, Traverse::array, postTraverse)) {
            pushStack(false, value::TypeTags::Boolean, value::bitcastFrom<bool>(false));
        }
        return;
    } else {
        if (!innerTraverse(tagInput, valInput, Traverse::document, preTraverse)) {
            pushStack(false, value::TypeTags::Boolean, value::bitcastFrom<bool>(false));
        }
        return;
    }
}

void ByteCode::traverseF(const CodeFragment* code) {
    // Traverse a filter path - evaluate the input lambda (predicate) on every element of the input
    // array without recursion.
    auto [numberOwn, numberTag, numberVal] = getFromStack(0);
    popAndReleaseStack();
    auto [lamOwn, lamTag, lamVal] = getFromStack(0);
    popAndReleaseStack();

    if (lamTag != value::TypeTags::LocalLambda) {
        popAndReleaseStack();
        pushStack(false, value::TypeTags::Nothing, 0);
        return;
    }
    int64_t lamPos = value::bitcastTo<int64_t>(lamVal);

    bool compareArray = numberTag == value::TypeTags::Boolean && value::bitcastTo<bool>(numberVal);

    traverseF(code, lamPos, compareArray);
}

void ByteCode::traverseF(const CodeFragment* code, int64_t position, bool compareArray) {
    auto [ownInput, tagInput, valInput] = getFromStack(0);

    if (value::isArray(tagInput)) {
        traverseFInArray(code, position, compareArray);
    } else {
        runLambdaInternal(code, position);
    }
}

bool ByteCode::runLambdaPredicate(const CodeFragment* code, int64_t position) {
    runLambdaInternal(code, position);
    auto [retOwn, retTag, retVal] = getFromStack(0);
    popStack();

    bool isTrue = (retTag == value::TypeTags::Boolean) && value::bitcastTo<bool>(retVal);
    if (retOwn) {
        value::releaseValue(retTag, retVal);
    }
    return isTrue;
}

void ByteCode::traverseFInArray(const CodeFragment* code, int64_t position, bool compareArray) {
    auto [ownInput, tagInput, valInput] = getFromStack(0);

    value::ValueGuard input(ownInput, tagInput, valInput);
    popStack();

    const bool passed =
        value::arrayAny(tagInput, valInput, [&](value::TypeTags tag, value::Value val) {
            pushStack(false, tag, val);
            if (runLambdaPredicate(code, position)) {
                pushStack(false, value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
                return true;
            }
            return false;
        });

    if (passed) {
        return;
    }

    // If this is a filter over a number path then run over the whole array. More details in
    // SERVER-27442.
    if (compareArray) {
        // Transfer the ownership to the lambda
        pushStack(ownInput, tagInput, valInput);
        input.reset();
        runLambdaInternal(code, position);
        return;
    }

    pushStack(false, value::TypeTags::Boolean, value::bitcastFrom<bool>(false));
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::setField() {
    auto [newOwn, newTag, newVal] = moveFromStack(0);
    value::ValueGuard guardNewElem{newTag, newVal};
    auto [fieldOwn, fieldTag, fieldVal] = getFromStack(1);
    // Consider using a moveFromStack optimization.
    auto [objOwn, objTag, objVal] = getFromStack(2);

    if (!value::isString(fieldTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto fieldName = value::getStringView(fieldTag, fieldVal);

    if (newTag == value::TypeTags::Nothing) {
        // Setting a field value to nothing means removing the field.
        if (value::isObject(objTag)) {
            auto [tagOutput, valOutput] = value::makeNewObject();
            auto objOutput = value::getObjectView(valOutput);
            value::ValueGuard guard{tagOutput, valOutput};

            if (objTag == value::TypeTags::bsonObject) {
                auto be = value::bitcastTo<const char*>(objVal);
                const auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();

                // Skip document length.
                be += 4;
                while (be != end - 1) {
                    auto sv = bson::fieldNameAndLength(be);

                    if (sv != fieldName) {
                        auto [tag, val] = bson::convertFrom<false>(be, end, sv.size());
                        objOutput->push_back(sv, tag, val);
                    }

                    be = bson::advance(be, sv.size());
                }
            } else {
                auto objRoot = value::getObjectView(objVal);
                for (size_t idx = 0; idx < objRoot->size(); ++idx) {
                    StringData sv(objRoot->field(idx));

                    if (sv != fieldName) {
                        auto [tag, val] = objRoot->getAt(idx);
                        auto [copyTag, copyVal] = value::copyValue(tag, val);
                        objOutput->push_back(sv, copyTag, copyVal);
                    }
                }
            }

            guard.reset();
            return {true, tagOutput, valOutput};
        } else {
            // Removing field from non-object value hardly makes any sense.
            return {false, value::TypeTags::Nothing, 0};
        }
    } else {
        // New value is not Nothing. We will be returning a new Object no matter what.
        auto [tagOutput, valOutput] = value::makeNewObject();
        auto objOutput = value::getObjectView(valOutput);
        value::ValueGuard guard{tagOutput, valOutput};

        if (objTag == value::TypeTags::bsonObject) {
            auto be = value::bitcastTo<const char*>(objVal);
            const auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();

            // Skip document length.
            be += 4;
            while (be != end - 1) {
                auto sv = bson::fieldNameAndLength(be);

                if (sv != fieldName) {
                    auto [tag, val] = bson::convertFrom<false>(be, end, sv.size());
                    objOutput->push_back(sv, tag, val);
                }

                be = bson::advance(be, sv.size());
            }
        } else if (objTag == value::TypeTags::Object) {
            auto objRoot = value::getObjectView(objVal);
            for (size_t idx = 0; idx < objRoot->size(); ++idx) {
                StringData sv(objRoot->field(idx));

                if (sv != fieldName) {
                    auto [tag, val] = objRoot->getAt(idx);
                    auto [copyTag, copyVal] = value::copyValue(tag, val);
                    objOutput->push_back(sv, copyTag, copyVal);
                }
            }
        }
        guardNewElem.reset();
        if (!newOwn) {
            auto [copyTag, copyVal] = value::copyValue(newTag, newVal);
            newTag = copyTag;
            newVal = copyVal;
        }
        objOutput->push_back(fieldName, newTag, newVal);

        guard.reset();
        return {true, tagOutput, valOutput};
    }
    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::getArraySize(value::TypeTags tag,
                                                                      value::Value val) {
    size_t result = 0;

    switch (tag) {
        case value::TypeTags::Array: {
            result = value::getArrayView(val)->size();
            break;
        }
        case value::TypeTags::ArraySet: {
            result = value::getArraySetView(val)->size();
            break;
        }
        case value::TypeTags::ArrayMultiSet: {
            result = value::getArrayMultiSetView(val)->size();
            break;
        }
        case value::TypeTags::bsonArray: {
            value::arrayForEach(
                tag, val, [&](value::TypeTags t_unused, value::Value v_unused) { result++; });
            break;
        }
        default:
            return {false, value::TypeTags::Nothing, 0};
    }

    return {false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(result)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::aggSum(value::TypeTags accTag,
                                                                value::Value accValue,
                                                                value::TypeTags fieldTag,
                                                                value::Value fieldValue) {
    value::ValueGuard guard{accTag, accValue};

    // Skip aggregation step if the input is Nothing or non-numeric.
    if (!value::isNumber(fieldTag)) {
        guard.reset();
        return {true, accTag, accValue};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        accTag = value::TypeTags::NumberInt32;
        accValue = value::bitcastFrom<int32_t>(0);
    }

    auto resultTuple = genericAdd(accTag, accValue, fieldTag, fieldValue);

    guard.reset();
    return resultTuple;
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::aggCount(value::TypeTags accTag,
                                                                  value::Value accValue) {
    value::ValueGuard guard{accTag, accValue};
    int64_t n = accTag == value::TypeTags::NumberInt64 ? value::bitcastTo<int64_t>(accValue) : 0;
    return {true, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(n + 1)};
}

void ByteCode::genericResetDoubleDoubleSumState(value::Array* state) {
    state->clear();
    // The order of the following three elements should match to 'AggSumValueElems'. An absent
    // 'kDecimalTotal' element means that we've not seen any decimal value. So, we're not adding
    // 'kDecimalTotal' element yet.
    state->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
    state->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0));
    state->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0));
}

std::pair<value::TypeTags, value::Value> ByteCode::genericInitializeDoubleDoubleSumState() {
    auto [accTag, accValue] = value::makeNewArray();
    value::ValueGuard newArrGuard{accTag, accValue};
    auto arr = value::getArrayView(accValue);
    arr->reserve(AggSumValueElems::kMaxSizeOfArray);

    genericResetDoubleDoubleSumState(arr);

    newArrGuard.reset();
    return {accTag, accValue};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::aggMin(value::TypeTags accTag,
                                                                value::Value accValue,
                                                                value::TypeTags fieldTag,
                                                                value::Value fieldValue,
                                                                CollatorInterface* collator) {
    // Skip aggregation step if we don't have the input.
    if (fieldTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }

    auto [tag, val] = value::compare3way(accTag, accValue, fieldTag, fieldValue, collator);

    if (tag == value::TypeTags::NumberInt32 && value::bitcastTo<int>(val) < 0) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    } else {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::aggMax(value::TypeTags accTag,
                                                                value::Value accValue,
                                                                value::TypeTags fieldTag,
                                                                value::Value fieldValue,
                                                                CollatorInterface* collator) {
    // Skip aggregation step if we don't have the input.
    if (fieldTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }

    auto [tag, val] = value::compare3way(accTag, accValue, fieldTag, fieldValue, collator);

    if (tag == value::TypeTags::NumberInt32 && value::bitcastTo<int>(val) > 0) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    } else {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::aggFirst(value::TypeTags accTag,
                                                                  value::Value accValue,
                                                                  value::TypeTags fieldTag,
                                                                  value::Value fieldValue) {
    // Skip aggregation step if we don't have the input.
    if (fieldTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }

    // Disregard the next value, always return the first one.
    auto [tag, val] = value::copyValue(accTag, accValue);
    return {true, tag, val};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::aggLast(value::TypeTags accTag,
                                                                 value::Value accValue,
                                                                 value::TypeTags fieldTag,
                                                                 value::Value fieldValue) {
    // Skip aggregation step if we don't have the input.
    if (fieldTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }

    // Disregard the accumulator, always return the next value.
    auto [tag, val] = value::copyValue(fieldTag, fieldValue);
    return {true, tag, val};
}


bool hasSeparatorAt(size_t idx, StringData input, StringData separator) {
    return (idx + separator.size() <= input.size()) &&
        input.substr(idx, separator.size()) == separator;
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::addToSetCappedImpl(
    value::TypeTags tagAccumulatorState,
    value::Value valAccumulatorState,  // Owned
    bool ownedNewElem,
    value::TypeTags tagNewElem,
    value::Value valNewElem,
    int32_t sizeCap,
    CollatorInterface* collator) {
    value::ValueGuard guardNewElem{ownedNewElem, tagNewElem, valNewElem};

    // The capped addToSet accumulator holds a value of Nothing at first and gets initialized on
    // demand when the first value gets added. Once initialized, the state is a two-element array
    // containing the set and its size in bytes, which is necessary to enforce the memory cap.
    if (tagAccumulatorState == value::TypeTags::Nothing) {
        std::tie(tagAccumulatorState, valAccumulatorState) = value::makeNewArray();
        value::ValueGuard guardAccumulatorState{tagAccumulatorState, valAccumulatorState};
        auto accumulatorState = value::getArrayView(valAccumulatorState);

        auto [tagAccSet, valAccSet] = value::makeNewArraySet(collator);

        // The order is important! The accumulated array should be at index
        // AggArrayWithSize::kValues, and the size should be at index
        // AggArrayWithSize::kSizeOfValues.
        accumulatorState->push_back(tagAccSet, valAccSet);
        accumulatorState->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));

        // Transfer ownership to the 'ValueGuard' in the enclosing scope.
        guardAccumulatorState.reset();
    }
    value::ValueGuard guardAccumulatorState{tagAccumulatorState, valAccumulatorState};
    tassert(10936800,
            "Expected array for set accumulator state",
            tagAccumulatorState == value::TypeTags::Array);

    auto accumulatorState = value::getArrayView(valAccumulatorState);
    tassert(10936801,
            "Set accumulator with invalid length",
            accumulatorState->size() == static_cast<size_t>(AggArrayWithSize::kLast));

    // Check that the accumulated size of the set won't exceed the limit after adding the new value,
    // and if so, add the value.
    auto [tagAccSet, valAccSet] =
        accumulatorState->getAt(static_cast<size_t>(AggArrayWithSize::kValues));
    tassert(
        10936802, "Expected ArraySet in accumulator state", tagAccSet == value::TypeTags::ArraySet);
    auto accSet = value::getArraySetView(valAccSet);
    if (!accSet->values().contains({tagNewElem, valNewElem})) {
        auto elemSize = value::getApproximateSize(tagNewElem, valNewElem);
        auto [tagAccSize, valAccSize] =
            accumulatorState->getAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues));
        tassert(10936803,
                "Expected integer value for ArraySet size",
                tagAccSize == value::TypeTags::NumberInt64);
        const int64_t currentSize = value::bitcastTo<int64_t>(valAccSize);
        int64_t newSize = currentSize + elemSize;

        uassert(ErrorCodes::ExceededMemoryLimit,
                str::stream() << "Used too much memory for a single set. Memory limit: " << sizeCap
                              << " bytes. The set contains " << accSet->size()
                              << " elements and is of size " << currentSize
                              << " bytes. The element being added has size " << elemSize
                              << " bytes.",
                newSize < static_cast<int64_t>(sizeCap));

        accumulatorState->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
                                value::TypeTags::NumberInt64,
                                value::bitcastFrom<int64_t>(newSize));

        // Insert the new value. Note that array will ignore Nothing.
        guardNewElem.reset();
        if (ownedNewElem) {
            accSet->push_back(tagNewElem, valNewElem);
        } else {
            accSet->push_back_clone(tagNewElem, valNewElem);
        }
    }

    guardAccumulatorState.reset();
    return {true, tagAccumulatorState, valAccumulatorState};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::setUnionAccumImpl(
    value::TypeTags tagAccumulatorState,
    value::Value valAccumulatorState,  // Owned
    value::TypeTags tagNewSetMembers,
    value::Value valNewSetMembers,  // Owned
    int32_t sizeCap,
    CollatorInterface* collator) {
    value::ValueGuard guardNewSetMembers{tagNewSetMembers, valNewSetMembers};

    // The capped addToSet accumulator holds a value of Nothing at first and gets initialized on
    // demand when the first value gets added. Once initialized, the state is a two-element array
    // containing the set and its size in bytes, which is necessary to enforce the memory cap.
    if (tagAccumulatorState == value::TypeTags::Nothing) {
        auto [tagAccSet, valAccSet] = value::makeNewArraySet(collator);

        std::tie(tagAccumulatorState, valAccumulatorState) = value::makeNewArray();
        auto accumulatorState = value::getArrayView(valAccumulatorState);

        // The order is important! The accumulated array should be at index
        // AggArrayWithSize::kValues, and the size should be at index
        // AggArrayWithSize::kSizeOfValues.
        accumulatorState->push_back(tagAccSet, valAccSet);
        accumulatorState->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    }
    value::ValueGuard guardAccumulatorState{tagAccumulatorState, valAccumulatorState};
    tassert(7039521,
            "Expected array for set accumulator state",
            tagAccumulatorState == value::TypeTags::Array);

    tassert(10936804,
            "Expected right-hand side of union to have array type",
            value::isArray(tagNewSetMembers) || tagNewSetMembers == value::TypeTags::Nothing);

    auto accumulatorState = value::getArrayView(valAccumulatorState);
    tassert(1093605,
            "Set accumulator with invalid length",
            accumulatorState->size() == static_cast<size_t>(AggArrayWithSize::kLast));

    auto [tagAccSet, valAccSet] =
        accumulatorState->getAt(static_cast<size_t>(AggArrayWithSize::kValues));
    tassert(
        7039523, "Expected ArraySet in accumulator state", tagAccSet == value::TypeTags::ArraySet);
    auto accSet = value::getArraySetView(valAccSet);

    // Extract the current size of the accumulator. As we add elements to the set, we will increment
    // the current size accordingly and throw an exception if we ever exceed the size limit. We
    // cannot simply sum the two sizes, since the two sets could have a substantial intersection.
    auto [accSizeTag, accSizeVal] =
        accumulatorState->getAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues));
    tassert(7039524, "expected 64-bit int", accSizeTag == value::TypeTags::NumberInt64);
    int64_t currentSize = value::bitcastTo<int64_t>(accSizeVal);

    if (tagNewSetMembers != value::TypeTags::Nothing) {
        value::arrayForEach<true>(
            tagNewSetMembers,
            valNewSetMembers,
            [&](value::TypeTags tagNewElem, value::Value valNewElem) {
                value::ValueGuard guardNewElem{tagNewElem, valNewElem};

                if (accSet->values().contains({tagNewElem, valNewElem})) {
                    // Skip this element, because it's already in the set, and continue with the
                    // remaining elements.
                    return;
                }

                int elemSize = value::getApproximateSize(tagNewElem, valNewElem);
                currentSize += elemSize;
                uassert(ErrorCodes::ExceededMemoryLimit,
                        str::stream() << "Used too much memory for a single array. Memory limit: "
                                      << sizeCap << ". Current set has " << accSet->size()
                                      << " elements and is " << currentSize << " bytes.",
                        currentSize < static_cast<int64_t>(sizeCap));

                guardNewElem.reset();
                accSet->push_back(tagNewElem, valNewElem);
            });
    }

    // Update the accumulator with the new total size.
    accumulatorState->setAt(static_cast<size_t>(AggArrayWithSize::kSizeOfValues),
                            value::TypeTags::NumberInt64,
                            value::bitcastFrom<int64_t>(currentSize));

    guardAccumulatorState.reset();
    return {true, tagAccumulatorState, valAccumulatorState};
}

ByteCode::MultiAccState ByteCode::getMultiAccState(value::TypeTags stateTag,
                                                   value::Value stateVal) {
    uassert(
        7548600, "The accumulator state should be an array", stateTag == value::TypeTags::Array);
    auto state = value::getArrayView(stateVal);

    uassert(7548601,
            "The accumulator state should have correct number of elements",
            state->size() == static_cast<size_t>(AggMultiElems::kSizeOfArray));

    auto [arrayTag, arrayVal] = state->getAt(static_cast<size_t>(AggMultiElems::kInternalArr));
    uassert(7548602,
            "Internal array component is not of correct type",
            arrayTag == value::TypeTags::Array);
    auto array = value::getArrayView(arrayVal);

    auto [startIndexTag, startIndexVal] =
        state->getAt(static_cast<size_t>(AggMultiElems::kStartIdx));
    uassert(7548700,
            "Index component be a 64-bit integer",
            startIndexTag == value::TypeTags::NumberInt64);
    int64_t startIndex = value::bitcastTo<int64_t>(startIndexVal);

    auto [maxSizeTag, maxSizeVal] = state->getAt(static_cast<size_t>(AggMultiElems::kMaxSize));
    uassert(7548603,
            "MaxSize component should be a 64-bit integer",
            maxSizeTag == value::TypeTags::NumberInt64);
    int64_t maxSize = value::bitcastTo<int64_t>(maxSizeVal);

    auto [memUsageTag, memUsageVal] = state->getAt(static_cast<size_t>(AggMultiElems::kMemUsage));
    uassert(7548612,
            "MemUsage component should be a 32-bit integer",
            memUsageTag == value::TypeTags::NumberInt32);
    int32_t memUsage = value::bitcastTo<int32_t>(memUsageVal);

    auto [memLimitTag, memLimitVal] = state->getAt(static_cast<size_t>(AggMultiElems::kMemLimit));
    uassert(7548613,
            "MemLimit component should be a 32-bit integer",
            memLimitTag == value::TypeTags::NumberInt32);
    auto memLimit = value::bitcastTo<int32_t>(memLimitVal);

    auto [isGroupAccumTag, isGroupAccumVal] =
        state->getAt(static_cast<size_t>(AggMultiElems::kIsGroupAccum));
    uassert(8070611,
            "IsGroupAccum component should be a boolean",
            isGroupAccumTag == value::TypeTags::Boolean);
    auto isGroupAccum = value::bitcastTo<bool>(isGroupAccumVal);

    return {state, array, startIndex, maxSize, memUsage, memLimit, isGroupAccum};
}

int32_t updateAndCheckMemUsage(
    value::Array* state, int32_t memUsage, int32_t memAdded, int32_t memLimit, size_t idx) {
    memUsage += memAdded;
    uassert(ErrorCodes::ExceededMemoryLimit,
            str::stream()
                << "Accumulator used too much memory and spilling to disk cannot reduce memory "
                   "consumption any further. Memory limit: "
                << memLimit << " bytes",
            memUsage < memLimit);
    state->setAt(idx, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(memUsage));
    return memUsage;
}

template <TopBottomSense Sense>
int32_t ByteCode::aggTopBottomNAdd(value::Array* state,
                                   value::Array* array,
                                   size_t maxSize,
                                   int32_t memUsage,
                                   int32_t memLimit,
                                   ByteCode::TopBottomArgs& args) {
    using Less =
        std::conditional_t<Sense == TopBottomSense::kTop, SortPatternLess, SortPatternGreater>;

    auto memAdded = [](std::pair<value::TypeTags, value::Value> key,
                       std::pair<value::TypeTags, value::Value> value) {
        return value::getApproximateSize(key.first, key.second) +
            value::getApproximateSize(value.first, value.second);
    };

    auto less = Less(args.getSortSpec());
    auto keyLess = PairKeyComp(less);
    auto& heap = array->values();

    if (array->size() < maxSize) {
        auto [pairTag, pairVal] = value::makeNewArray();
        value::ValueGuard pairGuard{pairTag, pairVal};
        auto pair = value::getArrayView(pairVal);
        pair->reserve(2);

        auto [keyTag, keyVal] = args.getOwnedKey();
        pair->push_back(keyTag, keyVal);

        auto [valueTag, valueVal] = args.getOwnedValue();
        pair->push_back(valueTag, valueVal);

        memUsage = updateAndCheckMemUsage(
            state, memUsage, memAdded({keyTag, keyVal}, {valueTag, valueVal}), memLimit);

        pairGuard.reset();
        array->push_back(pairTag, pairVal);
        std::push_heap(heap.begin(), heap.end(), keyLess);
    } else {
        tassert(5807005,
                "Heap should contain same number of elements as MaxSize",
                array->size() == maxSize);

        auto [worstTag, worstVal] = heap.front();
        auto worst = value::getArrayView(worstVal);
        auto worstKey = worst->getAt(0);

        if (args.keySortsBefore(worstKey)) {
            auto [keyTag, keyVal] = args.getOwnedKey();
            value::ValueGuard keyGuard{keyTag, keyVal};

            auto [valueTag, valueVal] = args.getOwnedValue();
            value::ValueGuard valueGuard{valueTag, valueVal};

            memUsage = updateAndCheckMemUsage(state,
                                              memUsage,
                                              -memAdded(worst->getAt(0), worst->getAt(1)) +
                                                  memAdded({keyTag, keyVal}, {valueTag, valueVal}),
                                              memLimit);

            std::pop_heap(heap.begin(), heap.end(), keyLess);

            keyGuard.reset();
            worst->setAt(0, keyTag, keyVal);

            valueGuard.reset();
            worst->setAt(1, valueTag, valueVal);

            std::push_heap(heap.begin(), heap.end(), keyLess);
        }
    }

    return memUsage;
}  // aggTopBottomNAdd

int32_t ByteCode::aggTopNAdd(value::Array* state,
                             value::Array* array,
                             size_t maxSize,
                             int32_t memUsage,
                             int32_t memLimit,
                             TopBottomArgs& args) {
    return aggTopBottomNAdd<TopBottomSense::kTop>(state, array, maxSize, memUsage, memLimit, args);
}

int32_t ByteCode::aggBottomNAdd(value::Array* state,
                                value::Array* array,
                                size_t maxSize,
                                int32_t memUsage,
                                int32_t memLimit,
                                TopBottomArgs& args) {
    return aggTopBottomNAdd<TopBottomSense::kBottom>(
        state, array, maxSize, memUsage, memLimit, args);
}

std::tuple<value::Array*, int64_t, int64_t, int64_t, int64_t, int64_t>
ByteCode::genericRemovableSumState(value::Array* state) {
    uassert(7795101,
            "incorrect size of state array",
            state->size() == static_cast<size_t>(AggRemovableSumElems::kSizeOfArray));

    auto [sumAccTag, sumAccVal] = state->getAt(static_cast<size_t>(AggRemovableSumElems::kSumAcc));
    uassert(7795102,
            "sum accumulator elem should be of array type",
            sumAccTag == value::TypeTags::Array);
    auto sumAcc = value::getArrayView(sumAccVal);

    auto [nanCountTag, nanCountVal] =
        state->getAt(static_cast<size_t>(AggRemovableSumElems::kNanCount));
    uassert(7795103,
            "nanCount elem should be of int64 type",
            nanCountTag == value::TypeTags::NumberInt64);
    auto nanCount = value::bitcastTo<int64_t>(nanCountVal);

    auto [posInfinityCountTag, posInfinityCountVal] =
        state->getAt(static_cast<size_t>(AggRemovableSumElems::kPosInfinityCount));
    uassert(7795104,
            "posInfinityCount elem should be of int64 type",
            posInfinityCountTag == value::TypeTags::NumberInt64);
    auto posInfinityCount = value::bitcastTo<int64_t>(posInfinityCountVal);

    auto [negInfinityCountTag, negInfinityCountVal] =
        state->getAt(static_cast<size_t>(AggRemovableSumElems::kNegInfinityCount));
    uassert(7795105,
            "negInfinityCount elem should be of int64 type",
            negInfinityCountTag == value::TypeTags::NumberInt64);
    auto negInfinityCount = value::bitcastTo<int64_t>(negInfinityCountVal);

    auto [doubleCountTag, doubleCountVal] =
        state->getAt(static_cast<size_t>(AggRemovableSumElems::kDoubleCount));
    uassert(7795106,
            "doubleCount elem should be of int64 type",
            doubleCountTag == value::TypeTags::NumberInt64);
    auto doubleCount = value::bitcastTo<int64_t>(doubleCountVal);

    auto [decimalCountTag, decimalCountVal] =
        state->getAt(static_cast<size_t>(AggRemovableSumElems::kDecimalCount));
    uassert(7795107,
            "decimalCount elem should be of int64 type",
            decimalCountTag == value::TypeTags::NumberInt64);
    auto decimalCount = value::bitcastTo<int64_t>(decimalCountVal);

    return {sumAcc, nanCount, posInfinityCount, negInfinityCount, doubleCount, decimalCount};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::aggRemovableSumFinalizeImpl(
    value::Array* state) {
    auto [sumAcc, nanCount, posInfinityCount, negInfinityCount, doubleCount, decimalCount] =
        genericRemovableSumState(state);

    if (nanCount > 0) {
        if (decimalCount > 0) {
            return {true,
                    value::TypeTags::NumberDecimal,
                    value::makeCopyDecimal(Decimal128::kPositiveNaN).second};
        } else {
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())};
        }
    }
    if (posInfinityCount > 0 && negInfinityCount > 0) {
        if (decimalCount > 0) {
            return {true,
                    value::TypeTags::NumberDecimal,
                    value::makeCopyDecimal(Decimal128::kPositiveNaN).second};
        } else {
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())};
        }
    }
    if (posInfinityCount > 0) {
        if (decimalCount > 0) {
            return {true,
                    value::TypeTags::NumberDecimal,
                    value::makeCopyDecimal(Decimal128::kPositiveInfinity).second};
        } else {
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(std::numeric_limits<double>::infinity())};
        }
    }
    if (negInfinityCount > 0) {
        if (decimalCount > 0) {
            return {true,
                    value::TypeTags::NumberDecimal,
                    value::makeCopyDecimal(Decimal128::kNegativeInfinity).second};
        } else {
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(-std::numeric_limits<double>::infinity())};
        }
    }

    auto [sumOwned, sumTag, sumVal] = aggDoubleDoubleSumFinalizeImpl(sumAcc);
    value::ValueGuard sumGuard{sumOwned, sumTag, sumVal};

    if (sumTag == value::TypeTags::NumberDecimal && decimalCount == 0) {
        auto decimalVal = value::bitcastTo<Decimal128>(sumVal);
        if (doubleCount > 0) {  // Narrow Decimal128 to double.
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(decimalVal.toDouble())};
        }
        std::uint32_t signalingFlags = Decimal128::SignalingFlag::kNoFlag;
        auto longVal = decimalVal.toLong(&signalingFlags);  // Narrow Decimal128 to integral.
        if (signalingFlags == Decimal128::SignalingFlag::kNoFlag) {
            auto [numTag, numVal] = value::makeIntOrLong(longVal);
            return {false, numTag, numVal};
        }
        // Narrow Decimal128 to double if overflows long.
        return {false,
                value::TypeTags::NumberDouble,
                value::bitcastFrom<double>(decimalVal.toDouble())};
    }
    if (sumTag == value::TypeTags::NumberDouble && doubleCount == 0 &&
        value::bitcastTo<double>(sumVal) >= std::numeric_limits<long long>::min() &&
        value::bitcastTo<double>(sumVal) <
            static_cast<double>(std::numeric_limits<long long>::max())) {
        // Narrow double to integral.
        auto longVal = llround(value::bitcastTo<double>(sumVal));
        auto [numTag, numVal] = value::makeIntOrLong(longVal);
        return {false, numTag, numVal};
    }
    if (sumTag == value::TypeTags::NumberInt64) {  // Narrow long to int
        auto longVal = value::bitcastTo<long long>(sumVal);
        auto [numTag, numVal] = value::makeIntOrLong(longVal);
        return {false, numTag, numVal};
    }
    sumGuard.reset();
    return {sumOwned, sumTag, sumVal};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
