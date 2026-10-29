// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// TODO(SERVER-114140): Remove all [[MONGO_MOD_NEEDS_REPLACEMENT]] annotations

namespace mongo {
namespace sbe {
namespace bson {
[[MONGO_MOD_NEEDS_REPLACEMENT]] value::TagValueView convertToView(const char* be,
                                                                  const char* end,
                                                                  size_t fieldNameSize);
[[MONGO_MOD_NEEDS_REPLACEMENT]] value::TagValueView convertToView(const BSONElement& elem);

[[MONGO_MOD_NEEDS_REPLACEMENT]] value::TagValueOwned convertToOwned(const char* be,
                                                                    const char* end,
                                                                    size_t fieldNameSize);
[[MONGO_MOD_NEEDS_REPLACEMENT]] value::TagValueOwned convertToOwned(const BSONElement& elem);

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

/**
 * Length of the NUL-terminated BSON field name starting at 's', which is known to terminate before
 * 'bsonEnd'. 'bsonEnd' points just past the terminating null byte of the BSON object.
 *
 * Scans the first 'kInlineWords' words inline, covering the short names that dominate real
 * documents without a library call, then hands long ones to 'memchr()'. Everything is bounded by
 * 'bsonEnd', which is what makes the word load safe: unbounded, an 8-byte load at a field name can
 * read past the buffer, since the shortest legal trailing sequence after a 1-byte name is only 2
 * bytes (NUL + empty value + EOO).
 *
 * 'memchr()' rather than scanning inline all the way because it is vectorized and takes a length,
 * so it stays safe and beats this loop on long names, which measured 0.75x libc at 128 bytes.
 */
inline size_t fieldNameLength(const char* s, const char* bsonEnd) noexcept {
    const char* const start = s;
    constexpr int kInlineWords = 4;

    // Below this many remaining bytes a byte loop beats calling into 'memchr()', whose fixed call
    // cost is not worth paying to scan a handful of bytes.
    constexpr std::ptrdiff_t kMinBytesForMemchr = 32;

    for (int word = 0; word < kInlineWords; ++word) {
        if (bsonEnd - s < static_cast<std::ptrdiff_t>(sizeof(uint64_t))) {
            break;
        }

        // Read little-endian on any host so the lowest-addressed byte is the least significant
        // lane. The subtraction below is one 64-bit op, so a zero lane borrows into the lane above
        // and can falsely flag it; borrows only propagate upward, so the lowest set bit is always
        // the real terminator. A native big-endian load would put that false flag first instead.
        const uint64_t chunk = ConstDataView(s).read<LittleEndian<uint64_t>>();

        // Sets the high bit of each byte that was zero, and clears every other byte. See
        // https://graphics.stanford.edu/~seander/bithacks.html#ZeroInWord.
        const uint64_t zeroes = (chunk - 0x0101010101010101ULL) & ~chunk & 0x8080808080808080ULL;
        if (zeroes) {
            return (s - start) + (countTrailingZerosNonZero64(zeroes) >> 3);
        }
        s += sizeof(uint64_t);
    }

    if (bsonEnd - s >= kMinBytesForMemchr) {
        if (const void* nul = std::memchr(s, '\0', static_cast<size_t>(bsonEnd - s))) {
            return static_cast<const char*>(nul) - start;
        }
        // No terminator before 'bsonEnd' means malformed BSON. Fall through: the loop below walks
        // to 'bsonEnd' and returns the same length, so the caller sees one behaviour either way.
    }

    while (s != bsonEnd && *s != '\0') {
        ++s;
    }
    return s - start;
}

inline auto fieldNameAndLength(const char* be, const char* bsonEnd) noexcept {
    return std::string_view{be + 1, fieldNameLength(be + 1, bsonEnd)};
}

// Add 1(typetag) + stringlength + 1(nullptr) to skip the null byte should give the value.
// 'bsonEnd' points just past the terminating null byte of the BSON object.
inline const char* getValue(const char* be, const char* bsonEnd) noexcept {
    return be + 1 + fieldNameLength(be + 1, bsonEnd) + 1;
}

inline value::TagValueView getField(const char* be, std::string_view fieldStr) noexcept {
    const auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
    be += sizeof(int);
    const auto targetSize = fieldStr.size();
    const char* targetData = fieldStr.data();

    bool match;
    size_t size;
    while (be != end - 1) {
        size = fieldNameLength(be + 1, end);
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
                          std::string_view name,
                          value::TypeTags tag,
                          value::Value val);

template <class ArrayBuilder>
void convertToBsonArr(ArrayBuilder& builder, value::ArrayEnumerator arr);

}  // namespace bson
}  // namespace sbe
}  // namespace mongo
