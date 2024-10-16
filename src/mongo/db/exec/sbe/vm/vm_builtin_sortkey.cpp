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

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo {
namespace sbe {
namespace vm {
std::pair<SortSpec*, CollatorInterface*> ByteCode::generateSortKeyHelper(ArityType arity) {
    invariant(arity == 2 || arity == 3);

    auto [ssOwned, ssTag, ssVal] = getFromStack(0);
    auto [objOwned, objTag, objVal] = getFromStack(1);
    if (ssTag != value::TypeTags::sortSpec || !value::isObject(objTag)) {
        return {nullptr, nullptr};
    }

    CollatorInterface* collator{nullptr};
    if (arity == 3) {
        auto [collatorOwned, collatorTag, collatorVal] = getFromStack(2);
        if (collatorTag != value::TypeTags::collator) {
            return {nullptr, nullptr};
        }
        collator = value::getCollatorView(collatorVal);
    }

    auto ss = value::getSortSpecView(ssVal);
    return {ss, collator};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinGenerateCheapSortKey(
    ArityType arity) {
    auto [sortSpec, collator] = generateSortKeyHelper(arity);
    if (!sortSpec) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // We "move" the object argument into the sort spec.
    auto sortKeyComponentVector =
        sortSpec->generateSortKeyComponentVector(moveFromStack(1), collator);

    return {false,
            value::TypeTags::sortKeyComponentVector,
            value::bitcastFrom<value::SortKeyComponentVector*>(sortKeyComponentVector)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinGenerateSortKey(ArityType arity) {
    auto [sortSpec, collator] = generateSortKeyHelper(arity);
    if (!sortSpec) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [objOwned, objTag, objVal] = getFromStack(1);
    auto bsonObj = [objTag = objTag, objVal = objVal]() {
        if (objTag == value::TypeTags::bsonObject) {
            return BSONObj{value::bitcastTo<const char*>(objVal)};
        } else if (objTag == value::TypeTags::Object) {
            BSONObjBuilder objBuilder;
            bson::convertToBsonObj(objBuilder, value::getObjectView(objVal));
            return objBuilder.obj();
        } else {
            MONGO_UNREACHABLE_TASSERT(5037004);
        }
    }();

    return {true,
            value::TypeTags::keyString,
            value::makeKeyString(sortSpec->generateSortKey(bsonObj, collator)).second};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSortKeyComponentVectorGetElement(
    ArityType arity) {
    invariant(arity == 2);

    auto [sortVecOwned, sortVecTag, sortVecVal] = getFromStack(0);
    auto [idxOwned, idxTag, idxVal] = getFromStack(1);
    if (sortVecTag != value::TypeTags::sortKeyComponentVector ||
        idxTag != value::TypeTags::NumberInt32) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto* sortObj = value::getSortKeyComponentVectorView(sortVecVal);
    const auto idxInt32 = value::bitcastTo<int32_t>(idxVal);

    invariant(idxInt32 >= 0 && static_cast<size_t>(idxInt32) < sortObj->elts.size());
    auto [outTag, outVal] = sortObj->elts[idxInt32];
    return {false, outTag, outVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSortKeyComponentVectorToArray(
    ArityType arity) {
    invariant(arity == 1);

    auto [sortVecOwned, sortVecTag, sortVecVal] = getFromStack(0);
    if (sortVecTag != value::TypeTags::sortKeyComponentVector) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto* sortVec = value::getSortKeyComponentVectorView(sortVecVal);

    if (sortVec->elts.size() == 1) {
        auto [tag, val] = sortVec->elts[0];
        auto [copyTag, copyVal] = value::copyValue(tag, val);
        return {true, copyTag, copyVal};
    } else {
        auto [arrayTag, arrayVal] = value::makeNewArray();
        value::ValueGuard arrayGuard{arrayTag, arrayVal};
        auto array = value::getArrayView(arrayVal);
        array->reserve(sortVec->elts.size());
        for (size_t i = 0; i < sortVec->elts.size(); ++i) {
            auto [tag, val] = sortVec->elts[i];
            auto [copyTag, copyVal] = value::copyValue(tag, val);
            array->push_back(copyTag, copyVal);
        }
        arrayGuard.reset();
        return {true, arrayTag, arrayVal};
    }
}

template <bool IsAscending, bool IsLeaf>
std::pair<value::TypeTags, value::Value> builtinGetSortKeyImpl(value::TypeTags inputTag,
                                                               value::Value inputVal,
                                                               CollatorInterface* collator) {
    if (!value::isArray(inputTag)) {
        // If 'input' is not an array, return 'fillEmpty(input, null)'.
        if (inputTag != value::TypeTags::Nothing) {
            return {inputTag, inputVal};
        } else {
            return {value::TypeTags::Null, 0};
        }
    }

    value::ArrayEnumerator arrayEnum(inputTag, inputVal);
    if (arrayEnum.atEnd()) {
        // If 'input' is an empty array, return Undefined or Null depending on whether 'IsLeaf'
        // is true or false.
        if constexpr (IsLeaf) {
            return {sbe::value::TypeTags::bsonUndefined, 0};
        } else {
            return {sbe::value::TypeTags::Null, 0};
        }
    }

    auto [accTag, accVal] = arrayEnum.getViewOfValue();
    arrayEnum.advance();

    // If we reach here, then 'input' is a non-empty array. Loop over the elements and find
    // the minimum element (if IsAscending is true) or the maximum element (if IsAscending
    // is false) and return it.
    while (!arrayEnum.atEnd()) {
        auto [itemTag, itemVal] = arrayEnum.getViewOfValue();
        auto [tag, val] = value::compare3way(itemTag, itemVal, accTag, accVal, collator);

        if (tag == value::TypeTags::Nothing) {
            // The comparison returns Nothing if one of the arguments is Nothing or if a sort order
            // cannot be determined: bail out immediately and return Null.
            return {sbe::value::TypeTags::Null, 0};
        }

        if (tag == value::TypeTags::NumberInt32) {
            int32_t cmp = value::bitcastTo<int32_t>(val);

            if ((IsAscending && cmp < 0) || (!IsAscending && cmp > 0)) {
                accTag = itemTag;
                accVal = itemVal;
            }
        }

        arrayEnum.advance();
    }

    return {accTag, accVal};
}

template <bool IsAscending, bool IsLeaf>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinGetSortKey(ArityType arity) {
    invariant(arity == 1 || arity == 2);

    CollatorInterface* collator = nullptr;
    if (arity == 2) {
        auto [_, collTag, collVal] = getFromStack(1);
        if (collTag == value::TypeTags::collator) {
            collator = value::getCollatorView(collVal);
        }
    }

    auto [inputOwned, inputTag, inputVal] = getFromStack(0);

    // If the argument is an array, find out the min/max value and place it in the stack. If it
    // is Nothing or another simple type, treat it as the return value.
    if (!value::isArray(inputTag)) {
        if (inputTag != value::TypeTags::Nothing) {
            return moveFromStack(0);
        } else {
            return {false, value::TypeTags::Null, 0};
        }
    }

    auto [resultTag, resultVal] =
        builtinGetSortKeyImpl<IsAscending, IsLeaf>(inputTag, inputVal, collator);

    // If the array is owned by the stack, make a copy of the item, or it will become invalid after
    // the caller clears the array from it.
    if (inputOwned) {
        auto [copyTag, copyVal] = value::copyValue(resultTag, resultVal);
        return {true, copyTag, copyVal};
    } else {
        return {false, resultTag, resultVal};
    }
}
template FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinGetSortKey<false, false>(
    ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinGetSortKey<false, true>(
    ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinGetSortKey<true, false>(
    ArityType arity);
template FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinGetSortKey<true, true>(
    ArityType arity);

std::pair<value::TypeTags, value::Value> GetSortKeyAscFunctor::operator()(value::TypeTags tag,
                                                                          value::Value val) const {
    auto [skTag, skVal] = builtinGetSortKeyImpl<true, true>(tag, val, collator);
    return value::copyValue(skTag, skVal);
}

std::pair<value::TypeTags, value::Value> GetSortKeyDescFunctor::operator()(value::TypeTags tag,
                                                                           value::Value val) const {
    auto [skTag, skVal] = builtinGetSortKeyImpl<false, true>(tag, val, collator);
    return value::copyValue(skTag, skVal);
}

const value::ColumnOpInstanceWithParams<value::ColumnOpType::kNoFlags, GetSortKeyAscFunctor>
    getSortKeyAscOp =
        value::makeColumnOpWithParams<value::ColumnOpType::kNoFlags, GetSortKeyAscFunctor>();

const value::ColumnOpInstanceWithParams<value::ColumnOpType::kNoFlags, GetSortKeyDescFunctor>
    getSortKeyDescOp =
        value::makeColumnOpWithParams<value::ColumnOpType::kNoFlags, GetSortKeyDescFunctor>();

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
