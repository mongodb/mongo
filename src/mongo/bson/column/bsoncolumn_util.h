// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/column/simple8b.h"
#include "mongo/platform/int128.h"
#include "mongo/stdx/utility.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo::bsoncolumn {
inline constexpr char kInterleavedStartControlByteLegacy = (char)0xF0;
inline constexpr char kInterleavedStartControlByte = (char)0xF1;
inline constexpr char kInterleavedStartArrayRootControlByte = (char)0xF2;
inline constexpr uint8_t kInvalidScaleIndex = 0xFF;
inline constexpr uint8_t kInvalidControlByte = 0xFE;
inline constexpr uint8_t kMaxNumSimple8bPerControl = 16;
inline constexpr std::array<uint8_t, Simple8bTypeUtil::kMemoryAsInteger + 1>
    kControlByteForScaleIndex = {0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0x80};

inline bool isUncompressedLiteralControlByte(uint8_t control) {
    return (control & 0xE0) == 0 || control == (uint8_t)stdx::to_underlying(BSONType::minKey) ||
        control == (uint8_t)stdx::to_underlying(BSONType::maxKey);
}

inline bool isInterleavedStartControlByte(char control) {
    return control == kInterleavedStartControlByteLegacy ||
        control == kInterleavedStartControlByte || control == kInterleavedStartArrayRootControlByte;
}

inline bool isSimple8bControlByte(uint8_t control) {
    return control != stdx::to_underlying(BSONType::eoo) &&
        !isUncompressedLiteralControlByte(control) && !isInterleavedStartControlByte(control);
}

inline uint8_t numSimple8bBlocksForControlByte(uint8_t control) {
    return (control & 0x0F) + 1;
}

inline uint32_t numElemsForControlByte(const char* control) {
    if (bsoncolumn::isUncompressedLiteralControlByte(*control)) {
        return 1;
    }

    Simple8b<uint128_t> reader(
        control + 1, sizeof(uint64_t) * bsoncolumn::numSimple8bBlocksForControlByte(*control));

    uint32_t num = 0;
    auto it = reader.begin();
    auto end = reader.end();
    while (it != end) {
        num += it.blockSize();
        it.advanceBlock();
    }
    return num;
}

/*
 * Calculate number of interleaved streams for a reference object and interleaved control byte. Will
 * throw if no scalar streams are found as that is an invalid reference object for the control byte.
 */
uint32_t numInterleavedStreams(const BSONObj& refObj, uint8_t control);

inline uint8_t scaleIndexForControlByte(uint8_t control) {
    static constexpr std::array<uint8_t, 16> kControlToScaleIndex = {kInvalidScaleIndex,
                                                                     kInvalidScaleIndex,
                                                                     kInvalidScaleIndex,
                                                                     kInvalidScaleIndex,
                                                                     kInvalidScaleIndex,
                                                                     kInvalidScaleIndex,
                                                                     kInvalidScaleIndex,
                                                                     kInvalidScaleIndex,
                                                                     5,  // 0b1000
                                                                     0,  // 0b1001
                                                                     1,  // 0b1010
                                                                     2,  // 0b1011
                                                                     3,  // 0b1100
                                                                     4,  // 0b1101
                                                                     kInvalidScaleIndex,
                                                                     kInvalidScaleIndex};

    return kControlToScaleIndex[(control & 0xF0) >> 4];
}

bool usesDeltaOfDelta(BSONType type);
bool uses128bit(BSONType type);

int64_t calcDelta(int64_t val, int64_t prev);
int128_t calcDelta(int128_t val, int128_t prev);

int64_t expandDelta(int64_t prev, int64_t delta);
int128_t expandDelta(int128_t prev, int128_t delta);

inline bool usesDeltaOfDelta(BSONType type) {
    return type == BSONType::oid || type == BSONType::date || type == BSONType::timestamp;
}

inline bool onlyZeroDelta(BSONType type) {
    return type == BSONType::regEx || type == BSONType::dbRef || type == BSONType::codeWScope ||
        type == BSONType::symbol || type == BSONType::object || type == BSONType::array ||
        type == BSONType::null || type == BSONType::undefined || type == BSONType::minKey ||
        type == BSONType::maxKey;
}

inline bool uses128bit(BSONType type) {
    return type == BSONType::numberDecimal || type == BSONType::binData ||
        type == BSONType::string || type == BSONType::code;
}

inline int64_t calcDelta(int64_t val, int64_t prev) {
    // Do the subtraction as unsigned and cast back to signed to get overflow defined to wrapped
    // around instead of undefined behavior.
    return static_cast<int64_t>(static_cast<uint64_t>(val) - static_cast<uint64_t>(prev));
}

inline int128_t calcDelta(int128_t val, int128_t prev) {
    // Do the subtraction as unsigned and cast back to signed to get overflow defined to wrapped
    // around instead of undefined behavior.
    return static_cast<int128_t>(static_cast<uint128_t>(val) - static_cast<uint128_t>(prev));
}

inline int64_t expandDelta(int64_t prev, int64_t delta) {
    // Do the addition as unsigned and cast back to signed to get overflow defined to wrapped around
    // instead of undefined behavior.
    return static_cast<int64_t>(static_cast<uint64_t>(prev) + static_cast<uint64_t>(delta));
}

inline int128_t expandDelta(int128_t prev, int128_t delta) {
    // Do the addition as unsigned and cast back to signed to get overflow defined to wrapped around
    // instead of undefined behavior.
    return static_cast<int128_t>(static_cast<uint128_t>(prev) + static_cast<uint128_t>(delta));
}

}  // namespace mongo::bsoncolumn
