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

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo {
namespace sbe {
namespace value {
/*
 * Similar to std::any_of, for SBE arrays.
 */
template <class Cb>
requires std::invocable<Cb&, TypeTags, Value>
bool arrayAny(TypeTags tag, Value val, const Cb& cb) {
    if (tag == TypeTags::bsonArray) {
        auto bson = getRawPointerView(val);
        const auto* cur = bson + 4;
        const auto* end = bson + ConstDataView(bson).read<LittleEndian<uint32_t>>();

        while (cur != end - 1) {
            auto* fieldName = bson::fieldNameRaw(cur);
            size_t keySize = TinyStrHelpers::strlen(fieldName);
            auto [elemTag, elemVal] = bson::convertFrom<true>(cur, end, keySize);

            if (cb(elemTag, elemVal)) {
                return true;
            }

            cur = bson::advance(cur, keySize);
        }
    } else if (tag == TypeTags::Array) {
        auto array = getArrayView(val);
        for (size_t i = 0; i < array->size(); ++i) {
            auto [t, v] = array->getAt(i);
            if (cb(t, v)) {
                return true;
            }
        }
    } else if (tag == TypeTags::ArraySet) {
        auto arraySet = getArraySetView(val);
        for (auto [t, v] : arraySet->values()) {
            if (cb(t, v)) {
                return true;
            }
        }
    } else if (tag == TypeTags::ArrayMultiSet) {
        auto arrayMultiSet = getArrayMultiSetView(val);
        for (auto [t, v] : arrayMultiSet->values()) {
            if (cb(t, v)) {
                return true;
            }
        }
    } else {
        MONGO_UNREACHABLE_TASSERT(11122918);
    }
    return false;
}

/**
 * Allows the caller to invoke either of the two function signatures.
 */
template <class Cb>
requires std::invocable<Cb&, TypeTags, Value> || std::invocable<Cb&, TypeTags, Value, const char*>
MONGO_COMPILER_ALWAYS_INLINE inline void invokeCb(const Cb& cb,
                                                  TypeTags elemTag,
                                                  Value elemVal,
                                                  const char* rawBson) {
    if constexpr (std::is_invocable_v<Cb, TypeTags, Value, const char*>) {
        cb(elemTag, elemVal, rawBson);
    } else {
        cb(elemTag, elemVal);
    }
}

/*
 * Invokes callback on each element of the given array.
 */
template <bool MoveOrCopy = false, class Cb>
requires std::invocable<Cb&, TypeTags, Value> || std::invocable<Cb&, TypeTags, Value, const char*>
inline void arrayForEach(TypeTags tag, Value val, const Cb& cb) {
    if (tag == TypeTags::bsonArray) {
        auto bson = getRawPointerView(val);
        const auto* cur = bson + 4;
        const auto* end = bson + ConstDataView(bson).read<LittleEndian<uint32_t>>();

        while (cur != end - 1) {
            auto* fieldName = bson::fieldNameRaw(cur);
            size_t keySize = TinyStrHelpers::strlen(fieldName);
            auto [elemTag, elemVal] = bson::convertFrom<true>(cur, end, keySize);

            if constexpr (MoveOrCopy) {
                std::tie(elemTag, elemVal) = value::copyValue(elemTag, elemVal);
            }
            invokeCb(cb, elemTag, elemVal, cur);
            cur = bson::advance(cur, keySize);
        }
    } else if (tag == TypeTags::Array) {
        auto array = getArrayView(val);
        for (auto& tv : array->values()) {
            auto [t, v] = tv;
            if constexpr (MoveOrCopy) {
                tv.first = value::TypeTags::Nothing;
                tv.second = 0;
            }
            invokeCb(cb, t, v, nullptr /*rawBson*/);
        }
        if constexpr (MoveOrCopy) {
            array->values().clear();
        }
    } else if (tag == TypeTags::ArraySet) {
        auto arraySet = getArraySetView(val);
        for (auto it = arraySet->values().begin(); it != arraySet->values().end();) {
            auto [t, v] = *it;
            if constexpr (MoveOrCopy) {
                arraySet->values().erase(it++);
            } else {
                ++it;
            }
            invokeCb(cb, t, v, nullptr /*rawBson*/);
        }
    } else if (tag == TypeTags::ArrayMultiSet) {
        auto arrayMultiSet = getArrayMultiSetView(val);
        for (auto it = arrayMultiSet->values().begin(); it != arrayMultiSet->values().end();) {
            auto [t, v] = *it;
            if constexpr (MoveOrCopy) {
                arrayMultiSet->values().erase(it++);
            } else {
                ++it;
            }
            invokeCb(cb, t, v, nullptr /*rawBson*/);
        }
    } else {
        MONGO_UNREACHABLE_TASSERT(11122919);
    }
}

/**
 * Invokes callback on each field's value of the given Object. Exits early if the callback returns
 * true.
 */
template <class Cb>
requires std::predicate<Cb&, StringData, TypeTags, Value, const char*>
inline void objectForEach(TypeTags tag, Value val, const Cb& cb) {
    if (tag == TypeTags::bsonObject) {
        auto bson = getRawPointerView(val);
        const auto end = bson::bsonEnd(bson);
        // Skip document length.
        const char* cur = bson + 4;
        bool done = false;
        while (!done && (cur != end - 1)) {
            StringData currFieldName = bson::fieldNameAndLength(cur);
            auto [eltTag, eltVal] = bson::convertFrom<true>(cur, end, currFieldName.size());
            done = cb(currFieldName, eltTag, eltVal, cur);
            cur = bson::advance(cur, currFieldName.size());
        }
    } else {
        tassert(10751104, "expected tag to be Object", tag == TypeTags::Object);
        auto obj = getObjectView(val);
        bool done = false;
        for (size_t i = 0; !done && (i < obj->size()); i++) {
            auto [eltTag, eltVal] = obj->getAt(i);
            done = cb(obj->field(i), eltTag, eltVal, nullptr);
        }
    }
}

/**
 * Return the length of the array. Throws an error if the value is not one of the possible types of
 * array.
 */
size_t getArraySize(TypeTags tag, Value value);

}  // namespace value
}  // namespace sbe
}  // namespace mongo
