// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/modules.h"

#include <string_view>

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
            auto [elemTag, elemVal] = bson::convertToView(cur, end, keySize);

            if (cb(elemTag, elemVal)) {
                return true;
            }

            cur = bson::advance(cur, keySize);
        }
    } else if (tag == TypeTags::Array) {
        auto array = getArrayView(val);
        for (size_t i = 0; i < array->size(); ++i) {
            auto tagVal = array->getAt(i);
            if (cb(tagVal.tag, tagVal.value)) {
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
            auto [elemTag, elemVal] = bson::convertToView(cur, end, keySize);

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
requires std::predicate<Cb&, std::string_view, TypeTags, Value, const char*>
inline void objectForEach(TypeTags tag, Value val, const Cb& cb) {
    if (tag == TypeTags::bsonObject) {
        auto bson = getRawPointerView(val);
        const auto end = bson::bsonEnd(bson);
        // Skip document length.
        const char* cur = bson + 4;
        bool done = false;
        while (!done && (cur != end - 1)) {
            std::string_view currFieldName = bson::fieldNameAndLength(cur);
            auto [eltTag, eltVal] = bson::convertToView(cur, end, currFieldName.size());
            done = cb(currFieldName, eltTag, eltVal, cur);
            cur = bson::advance(cur, currFieldName.size());
        }
    } else {
        tassert(10751104, "expected tag to be Object", tag == TypeTags::Object);
        auto obj = getObjectView(val);
        bool done = false;
        for (size_t i = 0; !done && (i < obj->size()); i++) {
            auto eltTagVal = obj->getAt(i);
            done = cb(obj->field(i), eltTagVal.tag, eltTagVal.value, nullptr);
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
