/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <MurmurHash3.h>

#include "mongo/base/data_range.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/string_data.h"

namespace mongo {

/**
 * Wraps the third-party hash function MurmurHash3. Callers should generally prefer this wrapper (or
 * one of the other overloads below) rather than calling into the third-party functions directly.
 * This interface is intended to be easier to consume safely.
 */
template <int SizeOfOutput>
inline size_t murmur3(StringData str, size_t seed);

/**
 * Template specialization for hashing a 'StringData' to a 32-bit hash code.
 */
template <>
inline size_t murmur3<4>(StringData str, size_t seed) {
    char hash[4];
    MurmurHash3_x86_32(str.data(), str.size(), seed, &hash);
    return ConstDataView(hash).read<LittleEndian<std::uint32_t>>();
}

/**
 * Template specialization for hashing a 'StringData' to a 64-bit hash code. Returns the first 8
 * bytes of the 128-bit version of MurmurHash, interpreting these 8 bytes as having a little-endian
 * byte order.
 */
template <>
inline size_t murmur3<8>(StringData str, size_t seed) {
    char hash[16];
    MurmurHash3_x64_128(str.data(), str.size(), seed, hash);
    return static_cast<size_t>(ConstDataView(hash).read<LittleEndian<std::uint64_t>>());
}

/**
 * Overload for callers which use a byte-array representation for the data and thus cannot easily
 * represent the input as a 'StringData'.
 */
template <int SizeOfOutput>
inline size_t murmur3(ConstDataRange data, size_t seed);

/**
 * Template specialization for hashing a 'ConstDataRange' to a 32-bit hash code.
 */
template <>
inline size_t murmur3<4>(ConstDataRange data, size_t seed) {
    char hash[4];
    MurmurHash3_x86_32(data.data(), data.length(), seed, &hash);
    return ConstDataView(hash).read<LittleEndian<std::uint32_t>>();
}

/**
 * Template specialization for hashing a 'ConstDataRange' to a 64-bit hash code. Returns the first 8
 * bytes of the 128-bit version of MurmurHash, interpreting these 8 bytes as having a little-endian
 * byte order.
 */
template <>
inline size_t murmur3<8>(ConstDataRange data, size_t seed) {
    char hash[16];
    MurmurHash3_x64_128(data.data(), data.length(), seed, hash);
    return static_cast<size_t>(ConstDataView(hash).read<LittleEndian<std::uint64_t>>());
}

/**
 * Writes the full output of the 128-bit version of MurmurHash to the given 'output' array.
 */
inline void murmur3(StringData str, size_t seed, std::array<char, 16>& output) {
    MurmurHash3_x64_128(str.data(), str.size(), seed, output.data());
}

/**
 * 128-bit overload where the input is given as a 'ConstDataRange'.
 */
inline void murmur3(ConstDataRange data, size_t seed, std::array<char, 16>& output) {
    MurmurHash3_x64_128(data.data(), data.length(), seed, output.data());
}

}  // namespace mongo
