// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <MurmurHash3.h>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Wraps the third-party hash function MurmurHash3. Callers should generally prefer this wrapper (or
 * one of the other overloads below) rather than calling into the third-party functions directly.
 * This interface is intended to be easier to consume safely.
 */
template <int SizeOfOutput>
inline size_t murmur3(std::string_view str, size_t seed);

/**
 * Template specialization for hashing a 'std::string_view' to a 32-bit hash code.
 */
template <>
inline size_t murmur3<4>(std::string_view str, size_t seed) {
    char hash[4];
    MurmurHash3_x86_32(str.data(), str.size(), seed, &hash);
    return ConstDataView(hash).read<LittleEndian<std::uint32_t>>();
}

/**
 * Template specialization for hashing a 'std::string_view' to a 64-bit hash code. Returns the first
 * 8 bytes of the 128-bit version of MurmurHash, interpreting these 8 bytes as having a
 * little-endian byte order.
 */
template <>
inline size_t murmur3<8>(std::string_view str, size_t seed) {
    char hash[16];
    MurmurHash3_x64_128(str.data(), str.size(), seed, hash);
    return static_cast<size_t>(ConstDataView(hash).read<LittleEndian<std::uint64_t>>());
}

/**
 * Overload for callers which use a byte-array representation for the data and thus cannot easily
 * represent the input as a 'std::string_view'.
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
inline void murmur3(std::string_view str, size_t seed, std::array<char, 16>& output) {
    MurmurHash3_x64_128(str.data(), str.size(), seed, output.data());
}

/**
 * 128-bit overload where the input is given as a 'ConstDataRange'.
 */
inline void murmur3(ConstDataRange data, size_t seed, std::array<char, 16>& output) {
    MurmurHash3_x64_128(data.data(), data.length(), seed, output.data());
}

}  // namespace mongo
