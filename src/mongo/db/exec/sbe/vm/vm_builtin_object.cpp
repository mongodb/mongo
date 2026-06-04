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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo {
namespace sbe {
namespace vm {
value::TagValueMaybeOwned ByteCode::builtinDropFields(ArityType arity) {
    auto inObj = viewFromStack(0);

    // We operate only on objects.
    if (!value::isObject(inObj.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Build the set of fields to drop.
    StringSet restrictFieldsSet;
    for (ArityType idx = 1; idx < arity; ++idx) {
        auto field = viewFromStack(idx);

        if (!value::isString(field.tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        restrictFieldsSet.emplace(value::getStringView(field.tag, field.value));
    }

    auto [tag, val] = value::makeNewObject();
    auto obj = value::getObjectView(val);
    value::ValueGuard guard{tag, val};

    if (inObj.tag == value::TypeTags::bsonObject) {
        auto be = value::bitcastTo<const char*>(inObj.value);
        const auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
        // Skip document length.
        be += 4;
        while (be != end - 1) {
            auto sv = bson::fieldNameAndLength(be);

            if (restrictFieldsSet.count(sv) == 0) {
                auto [tag, val] = bson::convertToOwned(be, end, sv.size()).releaseToRaw();
                obj->push_back_raw(sv, tag, val);
            }

            be = bson::advance(be, sv.size());
        }
    } else if (inObj.tag == value::TypeTags::Object) {
        auto objRoot = value::getObjectView(inObj.value);
        for (size_t idx = 0; idx < objRoot->size(); ++idx) {
            StringData sv(objRoot->field(idx));

            if (restrictFieldsSet.count(sv) == 0) {

                auto [tag, val] = objRoot->getAt(idx);
                auto [copyTag, copyVal] = value::copyValue(tag, val);
                obj->push_back_raw(sv, copyTag, copyVal);
            }
        }
    }

    guard.reset();
    return {true, tag, val};
}

value::TagValueMaybeOwned ByteCode::builtinKeepFields(ArityType arity) {
    auto inObj = viewFromStack(0);

    // We operate only on objects.
    if (!value::isObject(inObj.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Build the set of fields to keep.
    StringSet keepFieldsSet;
    for (ArityType idx = 1; idx < arity; ++idx) {
        auto field = viewFromStack(idx);

        if (!value::isString(field.tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        keepFieldsSet.emplace(value::getStringView(field.tag, field.value));
    }

    auto [tag, val] = value::makeNewObject();
    auto obj = value::getObjectView(val);
    value::ValueGuard guard{tag, val};

    if (inObj.tag == value::TypeTags::bsonObject) {
        auto be = value::bitcastTo<const char*>(inObj.value);
        const auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
        // Skip document length.
        be += 4;
        while (be != end - 1) {
            auto sv = bson::fieldNameAndLength(be);

            if (keepFieldsSet.count(sv) == 1) {
                auto [tag, val] = bson::convertToView(be, end, sv.size());
                auto [copyTag, copyVal] = value::copyValue(tag, val);
                obj->push_back_raw(sv, copyTag, copyVal);
            }

            be = bson::advance(be, sv.size());
        }
    } else if (inObj.tag == value::TypeTags::Object) {
        auto objRoot = value::getObjectView(inObj.value);
        for (size_t idx = 0; idx < objRoot->size(); ++idx) {
            StringData sv(objRoot->field(idx));

            if (keepFieldsSet.count(sv) == 1) {
                auto [tag, val] = objRoot->getAt(idx);
                auto [copyTag, copyVal] = value::copyValue(tag, val);
                obj->push_back_raw(sv, copyTag, copyVal);
            }
        }
    }

    guard.reset();
    return {true, tag, val};
}

value::TagValueMaybeOwned ByteCode::builtinNewObj(ArityType arity) {
    std::vector<value::TypeTags> typeTags;
    std::vector<value::Value> values;
    std::vector<std::string> names;

    size_t tmpVectorLen = arity >> 1;
    typeTags.reserve(tmpVectorLen);
    values.reserve(tmpVectorLen);
    names.reserve(tmpVectorLen);

    for (ArityType idx = 0; idx < arity; idx += 2) {
        {
            auto nameView = viewFromStack(idx);

            if (!value::isString(nameView.tag)) {
                return {false, value::TypeTags::Nothing, 0};
            }

            names.emplace_back(value::getStringView(nameView.tag, nameView.value));
        }
        {
            auto fieldView = viewFromStack(idx + 1);
            typeTags.push_back(fieldView.tag);
            values.push_back(fieldView.value);
        }
    }

    auto [tag, val] = value::makeNewObject();
    auto obj = value::getObjectView(val);
    value::ValueGuard guard{tag, val};

    if (typeTags.size()) {
        obj->reserve(typeTags.size());
        for (size_t idx = 0; idx < typeTags.size(); ++idx) {
            auto [tagCopy, valCopy] = value::copyValue(typeTags[idx], values[idx]);
            obj->push_back_raw(names[idx], tagCopy, valCopy);
        }
    }

    guard.reset();
    return {true, tag, val};
}

value::TagValueMaybeOwned ByteCode::builtinNewBsonObj(ArityType arity) {
    UniqueBSONObjBuilder bob;

    for (ArityType idx = 0; idx < arity; idx += 2) {
        auto nameView = viewFromStack(idx);
        auto fieldView = viewFromStack(idx + 1);
        if (!value::isString(nameView.tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        auto name = value::getStringView(nameView.tag, nameView.value);
        bson::appendValueToBsonObj(bob, name, fieldView.tag, fieldView.value);
    }

    bob.doneFast();
    char* data = bob.bb().release().release();
    return {true, value::TypeTags::bsonObject, value::bitcastFrom<char*>(data)};
}

value::TagValueMaybeOwned ByteCode::builtinMergeObjects(ArityType arity) {
    auto fieldView = viewFromStack(1);
    // Move the incoming accumulator state from the stack. Given that we are now the owner of the
    // state we are free to do any in-place update as we see fit.
    auto [tagAgg, valAgg] = moveOwnedFromStack(0);

    value::ValueGuard guard{tagAgg, valAgg};
    // Create a new object if it does not exist yet.
    if (tagAgg == value::TypeTags::Nothing) {
        std::tie(tagAgg, valAgg) = value::makeNewObject();
    }

    tassert(11086807, "Unexpected type of Agg parameter", tagAgg == value::TypeTags::Object);

    // If our field is nothing or null or it's not an object, return the accumulator state.
    if (fieldView.tag == value::TypeTags::Nothing || fieldView.tag == value::TypeTags::Null ||
        (fieldView.tag != value::TypeTags::Object &&
         fieldView.tag != value::TypeTags::bsonObject)) {
        guard.reset();
        return {true, tagAgg, valAgg};
    }

    auto obj = value::getObjectView(valAgg);

    StringMap<value::TagValueView> currObjMap;
    for (auto currObjEnum = value::ObjectEnumerator{fieldView.tag, fieldView.value};
         !currObjEnum.atEnd();
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
    for (auto currObjEnum = value::ObjectEnumerator{fieldView.tag, fieldView.value};
         !currObjEnum.atEnd();
         currObjEnum.advance()) {
        auto it = currObjMap.find(currObjEnum.getFieldName());
        if (it != currObjMap.end()) {
            auto [currObjTag, currObjVal] = it->second;
            auto [currObjTagCopy, currObjValCopy] = value::copyValue(currObjTag, currObjVal);
            obj->push_back_raw(currObjEnum.getFieldName(), currObjTagCopy, currObjValCopy);
        }
    }

    guard.reset();
    return {true, tagAgg, valAgg};
}

value::TagValueMaybeOwned ByteCode::builtinBsonSize(ArityType arity) {
    auto operand = viewFromStack(0);

    if (operand.tag == value::TypeTags::Object) {
        BSONObjBuilder objBuilder;
        bson::convertToBsonObj(objBuilder, value::getObjectView(operand.value));
        int32_t sz = objBuilder.done<BSONObj::LargeSizeTrait>().objsize();
        return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(sz)};
    } else if (operand.tag == value::TypeTags::bsonObject) {
        auto beginObj = value::getRawPointerView(operand.value);
        int32_t sz = ConstDataView(beginObj).read<LittleEndian<int32_t>>();
        return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(sz)};
    }
    return {false, value::TypeTags::Nothing, 0};
}

value::TagValueMaybeOwned ByteCode::builtinObjectToArray(ArityType arity) {
    tassert(11080026, "Unexpected arity value", arity == 1);

    auto obj = viewFromStack(0);

    if (!value::isObject(obj.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [arrTag, arrVal] = value::makeNewArray();
    value::ValueGuard arrGuard{arrTag, arrVal};
    auto array = value::getArrayView(arrVal);

    value::ObjectEnumerator objectEnumerator(obj.tag, obj.value);
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
        elemObj->push_back_raw("k"_sd, keyTag, keyVal);
        keyGuard.reset();
        elemObj->push_back_raw("v"_sd, valueCopyTag, valueCopyVal);

        // insert the object to array
        array->push_back_raw(elemTag, elemVal);
        elemGuard.reset();

        objectEnumerator.advance();
    }
    arrGuard.reset();
    return {true, arrTag, arrVal};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
