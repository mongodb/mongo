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

#include <cstddef>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo {
namespace sbe {
namespace bson {
template <bool View>
std::pair<value::TypeTags, value::Value> convertFrom(const char* be,
                                                     const char* end,
                                                     size_t fieldNameSize);

template <bool View>
std::pair<value::TypeTags, value::Value> convertFrom(const BSONElement& elem) {
    return convertFrom<View>(
        elem.rawdata(), elem.rawdata() + elem.size(), elem.fieldNameSize() - 1);
}

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
