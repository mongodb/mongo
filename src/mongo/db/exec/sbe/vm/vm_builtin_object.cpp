// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/vm/vm.h"

#include <string_view>

namespace mongo {
namespace sbe {
namespace vm {
using namespace std::literals::string_view_literals;
value::TagValueMaybeOwned ByteCode::builtinDropFields(ArityType arity) {
    auto inObj = viewFromStack(0);

    // We operate only on objects.
    if (!value::isObject(inObj.tag)) {
        return value::TagValueMaybeOwned::nothing();
    }

    // Build the set of fields to drop.
    StringSet restrictFieldsSet;
    for (ArityType idx = 1; idx < arity; ++idx) {
        auto field = viewFromStack(idx);

        if (!value::isString(field.tag)) {
            return value::TagValueMaybeOwned::nothing();
        }

        restrictFieldsSet.emplace(value::getStringView(field.tag, field.value));
    }

    value::TagValueOwned result{value::makeNewObject()};
    auto obj = value::getObjectView(result.value());

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
            std::string_view sv(objRoot->field(idx));

            if (restrictFieldsSet.count(sv) == 0) {

                auto tagVal = objRoot->getAt(idx);
                auto [copyTag, copyVal] = value::copyValue(tagVal.tag, tagVal.value);
                obj->push_back_raw(sv, copyTag, copyVal);
            }
        }
    }

    return std::move(result);
}

value::TagValueMaybeOwned ByteCode::builtinKeepFields(ArityType arity) {
    auto inObj = viewFromStack(0);

    // We operate only on objects.
    if (!value::isObject(inObj.tag)) {
        return value::TagValueMaybeOwned::nothing();
    }

    // Build the set of fields to keep.
    StringSet keepFieldsSet;
    for (ArityType idx = 1; idx < arity; ++idx) {
        auto field = viewFromStack(idx);

        if (!value::isString(field.tag)) {
            return value::TagValueMaybeOwned::nothing();
        }

        keepFieldsSet.emplace(value::getStringView(field.tag, field.value));
    }

    value::TagValueOwned result{value::makeNewObject()};
    auto obj = value::getObjectView(result.value());

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
            std::string_view sv(objRoot->field(idx));

            if (keepFieldsSet.count(sv) == 1) {
                auto tagVal = objRoot->getAt(idx);
                auto [copyTag, copyVal] = value::copyValue(tagVal.tag, tagVal.value);
                obj->push_back_raw(sv, copyTag, copyVal);
            }
        }
    }

    return std::move(result);
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
                return value::TagValueMaybeOwned::nothing();
            }

            names.emplace_back(value::getStringView(nameView.tag, nameView.value));
        }
        {
            auto fieldView = viewFromStack(idx + 1);
            typeTags.push_back(fieldView.tag);
            values.push_back(fieldView.value);
        }
    }

    value::TagValueOwned result{value::makeNewObject()};
    auto obj = value::getObjectView(result.value());

    if (typeTags.size()) {
        obj->reserve(typeTags.size());
        for (size_t idx = 0; idx < typeTags.size(); ++idx) {
            auto [tagCopy, valCopy] = value::copyValue(typeTags[idx], values[idx]);
            obj->push_back_raw(names[idx], tagCopy, valCopy);
        }
    }

    return std::move(result);
}

value::TagValueMaybeOwned ByteCode::builtinNewBsonObj(ArityType arity) {
    UniqueBSONObjBuilder bob;

    for (ArityType idx = 0; idx < arity; idx += 2) {
        auto nameView = viewFromStack(idx);
        auto fieldView = viewFromStack(idx + 1);
        if (!value::isString(nameView.tag)) {
            return value::TagValueMaybeOwned::nothing();
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
    value::TagValueOwned aggState{moveRawOwnedFromStack(0)};
    // Create a new object if it does not exist yet.
    if (aggState.tag() == value::TypeTags::Nothing) {
        aggState = value::TagValueOwned{value::makeNewObject()};
    }

    tassert(
        11086807, "Unexpected type of Agg parameter", aggState.tag() == value::TypeTags::Object);

    // If our field is nothing or null or it's not an object, return the accumulator state.
    if (fieldView.tag == value::TypeTags::Nothing || fieldView.tag == value::TypeTags::Null ||
        (fieldView.tag != value::TypeTags::Object &&
         fieldView.tag != value::TypeTags::bsonObject)) {
        return std::move(aggState);
    }

    auto obj = value::getObjectView(aggState.value());

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

    return std::move(aggState);
}

value::TagValueMaybeOwned ByteCode::builtinBsonSize(ArityType arity) {
    auto operand = viewFromStack(0);

    if (operand.tag == value::TypeTags::Object) {
        BSONObjBuilder objBuilder;
        bson::convertToBsonObj(objBuilder, value::getObjectView(operand.value));
        int32_t sz = objBuilder.done<BSONObj::LargeSizeTrait>().objsize();
        return value::TagValueMaybeOwned::numberInt32(sz);
    } else if (operand.tag == value::TypeTags::bsonObject) {
        auto beginObj = value::getRawPointerView(operand.value);
        int32_t sz = ConstDataView(beginObj).read<LittleEndian<int32_t>>();
        return value::TagValueMaybeOwned::numberInt32(sz);
    }
    return value::TagValueMaybeOwned::nothing();
}

value::TagValueMaybeOwned ByteCode::builtinObjectToArray(ArityType arity) {
    tassert(11080026, "Unexpected arity value", arity == 1);

    auto obj = viewFromStack(0);

    if (!value::isObject(obj.tag)) {
        return value::TagValueMaybeOwned::nothing();
    }

    value::TagValueOwned arr{value::makeNewArray()};
    auto array = value::getArrayView(arr.value());

    value::ObjectEnumerator objectEnumerator(obj.tag, obj.value);
    while (!objectEnumerator.atEnd()) {
        // get key
        auto fieldName = objectEnumerator.getFieldName();
        value::TagValueOwned key{value::makeNewString(fieldName)};

        // get value
        auto [valueTag, valueVal] = objectEnumerator.getViewOfValue();
        value::TagValueOwned valueCopy{value::copyValue(valueTag, valueVal)};

        // create a new obejct
        value::TagValueOwned elem{value::makeNewObject()};
        auto elemObj = value::getObjectView(elem.value());

        // insert key and value to the object
        elemObj->push_back_raw("k"sv, key.releaseToRaw());
        elemObj->push_back_raw("v"sv, valueCopy.releaseToRaw());

        // insert the object to array
        array->push_back(std::move(elem));

        objectEnumerator.advance();
    }
    return std::move(arr);
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
