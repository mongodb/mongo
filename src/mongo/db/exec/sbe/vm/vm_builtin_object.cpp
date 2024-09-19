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

namespace mongo {
namespace sbe {
namespace vm {
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinDropFields(ArityType arity) {
    auto [ownedSeparator, tagInObj, valInObj] = getFromStack(0);

    // We operate only on objects.
    if (!value::isObject(tagInObj)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Build the set of fields to drop.
    StringSet restrictFieldsSet;
    for (ArityType idx = 1; idx < arity; ++idx) {
        auto [owned, tag, val] = getFromStack(idx);

        if (!value::isString(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        restrictFieldsSet.emplace(value::getStringView(tag, val));
    }

    auto [tag, val] = value::makeNewObject();
    auto obj = value::getObjectView(val);
    value::ValueGuard guard{tag, val};

    if (tagInObj == value::TypeTags::bsonObject) {
        auto be = value::bitcastTo<const char*>(valInObj);
        const auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
        // Skip document length.
        be += 4;
        while (be != end - 1) {
            auto sv = bson::fieldNameAndLength(be);

            if (restrictFieldsSet.count(sv) == 0) {
                auto [tag, val] = bson::convertFrom<false>(be, end, sv.size());
                obj->push_back(sv, tag, val);
            }

            be = bson::advance(be, sv.size());
        }
    } else if (tagInObj == value::TypeTags::Object) {
        auto objRoot = value::getObjectView(valInObj);
        for (size_t idx = 0; idx < objRoot->size(); ++idx) {
            StringData sv(objRoot->field(idx));

            if (restrictFieldsSet.count(sv) == 0) {

                auto [tag, val] = objRoot->getAt(idx);
                auto [copyTag, copyVal] = value::copyValue(tag, val);
                obj->push_back(sv, copyTag, copyVal);
            }
        }
    }

    guard.reset();
    return {true, tag, val};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinKeepFields(ArityType arity) {
    auto [ownedInObj, tagInObj, valInObj] = getFromStack(0);

    // We operate only on objects.
    if (!value::isObject(tagInObj)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Build the set of fields to keep.
    StringSet keepFieldsSet;
    for (ArityType idx = 1; idx < arity; ++idx) {
        auto [owned, tag, val] = getFromStack(idx);

        if (!value::isString(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        keepFieldsSet.emplace(value::getStringView(tag, val));
    }

    auto [tag, val] = value::makeNewObject();
    auto obj = value::getObjectView(val);
    value::ValueGuard guard{tag, val};

    if (tagInObj == value::TypeTags::bsonObject) {
        auto be = value::bitcastTo<const char*>(valInObj);
        const auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
        // Skip document length.
        be += 4;
        while (be != end - 1) {
            auto sv = bson::fieldNameAndLength(be);

            if (keepFieldsSet.count(sv) == 1) {
                auto [tag, val] = bson::convertFrom<true>(be, end, sv.size());
                auto [copyTag, copyVal] = value::copyValue(tag, val);
                obj->push_back(sv, copyTag, copyVal);
            }

            be = bson::advance(be, sv.size());
        }
    } else if (tagInObj == value::TypeTags::Object) {
        auto objRoot = value::getObjectView(valInObj);
        for (size_t idx = 0; idx < objRoot->size(); ++idx) {
            StringData sv(objRoot->field(idx));

            if (keepFieldsSet.count(sv) == 1) {
                auto [tag, val] = objRoot->getAt(idx);
                auto [copyTag, copyVal] = value::copyValue(tag, val);
                obj->push_back(sv, copyTag, copyVal);
            }
        }
    }

    guard.reset();
    return {true, tag, val};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinNewObj(ArityType arity) {
    std::vector<value::TypeTags> typeTags;
    std::vector<value::Value> values;
    std::vector<std::string> names;

    size_t tmpVectorLen = arity >> 1;
    typeTags.reserve(tmpVectorLen);
    values.reserve(tmpVectorLen);
    names.reserve(tmpVectorLen);

    for (ArityType idx = 0; idx < arity; idx += 2) {
        {
            auto [owned, tag, val] = getFromStack(idx);

            if (!value::isString(tag)) {
                return {false, value::TypeTags::Nothing, 0};
            }

            names.emplace_back(value::getStringView(tag, val));
        }
        {
            auto [owned, tag, val] = getFromStack(idx + 1);
            typeTags.push_back(tag);
            values.push_back(val);
        }
    }

    auto [tag, val] = value::makeNewObject();
    auto obj = value::getObjectView(val);
    value::ValueGuard guard{tag, val};

    if (typeTags.size()) {
        obj->reserve(typeTags.size());
        for (size_t idx = 0; idx < typeTags.size(); ++idx) {
            auto [tagCopy, valCopy] = value::copyValue(typeTags[idx], values[idx]);
            obj->push_back(names[idx], tagCopy, valCopy);
        }
    }

    guard.reset();
    return {true, tag, val};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinNewBsonObj(ArityType arity) {
    UniqueBSONObjBuilder bob;

    for (ArityType idx = 0; idx < arity; idx += 2) {
        auto [_, nameTag, nameVal] = getFromStack(idx);
        auto [__, fieldTag, fieldVal] = getFromStack(idx + 1);
        if (!value::isString(nameTag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        auto name = value::getStringView(nameTag, nameVal);
        bson::appendValueToBsonObj(bob, name, fieldTag, fieldVal);
    }

    bob.doneFast();
    char* data = bob.bb().release().release();
    return {true, value::TypeTags::bsonObject, value::bitcastFrom<char*>(data)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinMakeBsonObj(
    ArityType arity, const CodeFragment* code) {
    constexpr int64_t maxInt64 = std::numeric_limits<int64_t>::max();

    tassert(6897002,
            str::stream() << "Unsupported number of args passed to makeBsonObj(): " << arity,
            arity >= 2);

    auto [specOwned, specTag, specVal] = getFromStack(0);
    auto [objOwned, objTag, objVal] = getFromStack(1);

    if (specTag != value::TypeTags::makeObjSpec) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto spec = value::getMakeObjSpecView(specVal);
    auto maxDepth = spec->traversalDepth ? *spec->traversalDepth : maxInt64;

    // If 'obj' is not an object or array, check if there is any applicable "NonObjInputBehavior"
    // that should be applied.
    if (spec->nonObjInputBehavior != MakeObjSpec::NonObjInputBehavior::kNewObj &&
        !value::isObject(objTag) && (!value::isArray(objTag) || maxDepth == 0)) {
        if (spec->nonObjInputBehavior == MakeObjSpec::NonObjInputBehavior::kReturnNothing) {
            // If the input is Nothing or not an Object and if 'nonObjInputBehavior' equals
            // 'kReturnNothing', then return Nothing.
            return {false, value::TypeTags::Nothing, 0};
        } else if (spec->nonObjInputBehavior == MakeObjSpec::NonObjInputBehavior::kReturnInput) {
            // If the input is Nothing or not an Object and if 'nonObjInputBehavior' equals
            // 'kReturnInput', then return the input.
            topStack(false, value::TypeTags::Nothing, 0);
            return {objOwned, objTag, objVal};
        }
    }

    const int argsStackOff = 2;
    const auto ctx = ProduceObjContext{argsStackOff, code};

    // If 'tag' is an array and we have not exceeded the maximum depth yet, traverse the array.
    if (maxDepth > 0 && value::isArray(objTag)) {
        UniqueBSONArrayBuilder bab;

        auto newMaxDepth = maxDepth == maxInt64 ? maxDepth : maxDepth - 1;
        traverseAndProduceBsonObj({ctx, spec}, objTag, objVal, newMaxDepth, bab);

        bab.doneFast();
        char* data = bab.bb().release().release();
        return {true, value::TypeTags::bsonArray, value::bitcastFrom<char*>(data)};
    } else {
        UniqueBSONObjBuilder bob;

        produceBsonObject(ctx, spec, bob, objTag, objVal);

        bob.doneFast();
        char* data = bob.bb().release().release();
        return {true, value::TypeTags::bsonObject, value::bitcastFrom<char*>(data)};
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinMergeObjects(ArityType arity) {
    auto [_, tagField, valField] = getFromStack(1);
    // Move the incoming accumulator state from the stack. Given that we are now the owner of the
    // state we are free to do any in-place update as we see fit.
    auto [tagAgg, valAgg] = moveOwnedFromStack(0);

    value::ValueGuard guard{tagAgg, valAgg};
    // Create a new object if it does not exist yet.
    if (tagAgg == value::TypeTags::Nothing) {
        std::tie(tagAgg, valAgg) = value::makeNewObject();
    }

    invariant(tagAgg == value::TypeTags::Object);

    // If our field is nothing or null or it's not an object, return the accumulator state.
    if (tagField == value::TypeTags::Nothing || tagField == value::TypeTags::Null ||
        (tagField != value::TypeTags::Object && tagField != value::TypeTags::bsonObject)) {
        guard.reset();
        return {true, tagAgg, valAgg};
    }

    auto obj = value::getObjectView(valAgg);

    StringMap<std::pair<value::TypeTags, value::Value>> currObjMap;
    for (auto currObjEnum = value::ObjectEnumerator{tagField, valField}; !currObjEnum.atEnd();
         currObjEnum.advance()) {
        currObjMap[currObjEnum.getFieldName()] = currObjEnum.getViewOfValue();
    }

    // Process the accumulated fields and if a field within the current object already exists
    // within the existing accuultor, we set the value of that field within the accumuator to the
    // value contained within the current object. Preserves the order of existing fields in the
    // accumulator
    for (size_t idx = 0, numFields = obj->size(); idx < numFields; ++idx) {
        auto it = currObjMap.find(obj->field(idx));
        if (it != currObjMap.end()) {
            auto [currObjTag, currObjVal] = it->second;
            auto [currObjTagCopy, currObjValCopy] = value::copyValue(currObjTag, currObjVal);
            obj->setAt(idx, currObjTagCopy, currObjValCopy);
            currObjMap.erase(it);
        }
    }

    // Copy the remaining fields of the current object being processed to the
    // accumulator. Fields that were already present in the accumulated fields
    // have been set already. Preserves the relative order of the new fields
    for (auto currObjEnum = value::ObjectEnumerator{tagField, valField}; !currObjEnum.atEnd();
         currObjEnum.advance()) {
        auto it = currObjMap.find(currObjEnum.getFieldName());
        if (it != currObjMap.end()) {
            auto [currObjTag, currObjVal] = it->second;
            auto [currObjTagCopy, currObjValCopy] = value::copyValue(currObjTag, currObjVal);
            obj->push_back(currObjEnum.getFieldName(), currObjTagCopy, currObjValCopy);
        }
    }

    guard.reset();
    return {true, tagAgg, valAgg};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinBsonSize(ArityType arity) {
    auto [_, tagOperand, valOperand] = getFromStack(0);

    if (tagOperand == value::TypeTags::Object) {
        BSONObjBuilder objBuilder;
        bson::convertToBsonObj(objBuilder, value::getObjectView(valOperand));
        int32_t sz = objBuilder.done().objsize();
        return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(sz)};
    } else if (tagOperand == value::TypeTags::bsonObject) {
        auto beginObj = value::getRawPointerView(valOperand);
        int32_t sz = ConstDataView(beginObj).read<LittleEndian<int32_t>>();
        return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(sz)};
    }
    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinObjectToArray(ArityType arity) {
    invariant(arity == 1);

    auto [objOwned, objTag, objVal] = getFromStack(0);

    if (!value::isObject(objTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [arrTag, arrVal] = value::makeNewArray();
    value::ValueGuard arrGuard{arrTag, arrVal};
    auto array = value::getArrayView(arrVal);

    value::ObjectEnumerator objectEnumerator(objTag, objVal);
    while (!objectEnumerator.atEnd()) {
        // get key
        auto fieldName = objectEnumerator.getFieldName();
        auto [keyTag, keyVal] = value::makeNewString(fieldName);
        value::ValueGuard keyGuard{keyTag, keyVal};

        // get value
        auto [valueTag, valueVal] = objectEnumerator.getViewOfValue();
        auto [valueCopyTag, valueCopyVal] = value::copyValue(valueTag, valueVal);

        // create a new obejct
        auto [elemTag, elemVal] = value::makeNewObject();
        value::ValueGuard elemGuard{elemTag, elemVal};
        auto elemObj = value::getObjectView(elemVal);

        // insert key and value to the object
        elemObj->push_back("k"_sd, keyTag, keyVal);
        keyGuard.reset();
        elemObj->push_back("v"_sd, valueCopyTag, valueCopyVal);

        // insert the object to array
        array->push_back(elemTag, elemVal);
        elemGuard.reset();

        objectEnumerator.advance();
    }
    arrGuard.reset();
    return {true, arrTag, arrVal};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
