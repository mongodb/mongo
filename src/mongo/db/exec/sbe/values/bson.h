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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <utility>

// TODO(SERVER-114140): Remove all MONGO_MOD_NEEDS_REPLACEMENT annotations

namespace mongo {
namespace sbe {
namespace bson {
MONGO_MOD_NEEDS_REPLACEMENT value::TagValueView convertToView(const char* be,
                                                              const char* end,
                                                              size_t fieldNameSize);
MONGO_MOD_NEEDS_REPLACEMENT value::TagValueView convertToView(const BSONElement& elem);

MONGO_MOD_NEEDS_REPLACEMENT value::TagValueOwned convertToOwned(const char* be,
                                                                const char* end,
                                                                size_t fieldNameSize);
MONGO_MOD_NEEDS_REPLACEMENT value::TagValueOwned convertToOwned(const BSONElement& elem);

/**
 * Advance table specifies how to change the pointer to skip current BSON value (so that pointer
 * points to the next byte after the BSON value):
 *  - For each entry N in 'kAdvanceTable' that is less than 0x7F, pointer is advanced by N.
 *  - For each entry N in 'kAdvanceTable' that is greater than 0x7F, pointer is advanced by
 *      the 32-bit integer stored in buffer plus ~N.
 *  - For each entry N in 'kAdvanceTable' that is equal to 0x7F, the type is either RegEx or it
 *      is an unsupported type (EOO) or its an invalid type value (i.e. the type value does not
 *      correspond to any known type).
 */
extern const uint8_t kAdvanceTable alignas(64)[256];

const char* advanceHelper(const char* be, size_t fieldNameSize);

inline const char* advance(const char* be, size_t fieldNameSize) {
    auto type = static_cast<unsigned char>(*be);
    auto advOffset = kAdvanceTable[type];

    size_t sizeOfTypeCodeAndFieldName =
        1 /*type*/ + fieldNameSize + 1 /*zero at the end of fieldname*/;

    if (MONGO_likely(advOffset < 0x7Fu)) {
        be += sizeOfTypeCodeAndFieldName;
        be += advOffset;
        return be;
    } else if (MONGO_likely(advOffset > 0x7Fu)) {
        advOffset = ~advOffset;
        be += sizeOfTypeCodeAndFieldName;
        be += ConstDataView(be).read<LittleEndian<int32_t>>();
        be += advOffset;
        return be;
    }

    return advanceHelper(be, fieldNameSize);
}

inline auto fieldNameAndLength(const char* be) noexcept {
    return StringData{be + 1, strlen(be + 1)};
}

// add 1(typetag) + stringlength + 1(nullptr) to skip the null byte should give the value
inline const char* getValue(const char* be) noexcept {
    return be + 1 + strlen(be + 1) + 1;
}

inline std::pair<value::TypeTags, value::Value> getField(const char* be,
                                                         StringData fieldStr) noexcept {
    const auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
    be += sizeof(int);
    const auto targetSize = fieldStr.size();
    const char* targetData = fieldStr.data();

    bool match;
    size_t size;
    while (be != end - 1) {
        if (MONGO_unlikely(*(be + 1) == '\0')) {
            size = 0;
        } else if (*(be + 2) == '\0') {
            size = 1;
        } else if (*(be + 3) == '\0') {
            size = 2;
        } else if (*(be + 4) == '\0') {
            size = 3;
        } else if (*(be + 5) == '\0') {
            size = 4;
        } else if (*(be + 6) == '\0') {
            size = 5;
        } else if (*(be + 7) == '\0') {
            size = 6;
        } else if (*(be + 8) == '\0') {
            size = 7;
        } else if (*(be + 9) == '\0') {
            size = 8;
        } else {
            size = 8 + strlen(be + 9);
        }
        if (size == targetSize) {
            match = true;
            switch (targetSize) {
                case 8:
                    match &= *(be + 8) == targetData[7];
                    [[fallthrough]];
                case 7:
                    match &= *(be + 7) == targetData[6];
                    [[fallthrough]];
                case 6:
                    match &= *(be + 6) == targetData[5];
                    [[fallthrough]];
                case 5:
                    match &= *(be + 5) == targetData[4];
                    [[fallthrough]];
                case 4:
                    match &= *(be + 4) == targetData[3];
                    [[fallthrough]];
                case 3:
                    match &= *(be + 3) == targetData[2];
                    [[fallthrough]];
                case 2:
                    match &= *(be + 2) == targetData[1];
                    [[fallthrough]];
                case 1:
                    match &= *(be + 1) == targetData[0];
                    [[fallthrough]];
                case 0:
                    break;
                default:
                    match =
                        *(be + 1) == targetData[0] && std::memcmp(be + 1, targetData, size) == 0;
                    break;
            }
            if (match) {
                return convertToView(be, end, targetSize);
            }
        }
        be = bson::advance(be, size);
    }
    return {value::TypeTags::Nothing, 0};
}

inline const char* fieldNameRaw(const char* be) noexcept {
    return be + 1;
}

inline const char* bsonEnd(const char* bsonStart) noexcept {
    return bsonStart + ConstDataView(bsonStart).read<LittleEndian<uint32_t>>();
}

template <class ArrayBuilder>
void convertToBsonArr(ArrayBuilder& builder, value::Array* arr);

template <class ArrayBuilder>
void appendValueToBsonArr(ArrayBuilder& builder, value::TypeTags tag, value::Value val);

template <class ObjBuilder>
void convertToBsonObj(ObjBuilder& builder, value::Object* obj);

template <class ObjBuilder>
void appendValueToBsonObj(ObjBuilder& builder,
                          StringData name,
                          value::TypeTags tag,
                          value::Value val);

template <class ArrayBuilder>
void convertToBsonArr(ArrayBuilder& builder, value::ArrayEnumerator arr);

}  // namespace bson
}  // namespace sbe
}  // namespace mongo
