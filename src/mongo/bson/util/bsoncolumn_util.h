/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/bsontypes.h"
#include "mongo/platform/int128.h"

namespace mongo::bsoncolumn {
static constexpr char kInterleavedStartControlByteLegacy = (char)0xF0;
static constexpr char kInterleavedStartControlByte = (char)0xF1;
static constexpr char kInterleavedStartArrayRootControlByte = (char)0xF2;

inline bool isLiteralControlByte(char control) {
    return (control & 0xE0) == 0;
}

inline uint8_t numSimple8bBlocksForControlByte(char control) {
    return (control & 0x0F) + 1;
}

bool usesDeltaOfDelta(BSONType type);
bool uses128bit(BSONType type);

int64_t calcDelta(int64_t val, int64_t prev);
int128_t calcDelta(int128_t val, int128_t prev);

int64_t expandDelta(int64_t prev, int64_t delta);
int128_t expandDelta(int128_t prev, int128_t delta);

inline bool usesDeltaOfDelta(BSONType type) {
    return type == jstOID || type == Date || type == bsonTimestamp;
}

inline bool uses128bit(BSONType type) {
    return type == NumberDecimal || type == BinData || type == String || type == Code;
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
