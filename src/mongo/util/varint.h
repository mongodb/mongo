// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_type.h"
#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Methods to compress and decompress 64-bit integers into variable integers
 *
 * Uses a technique described here:
 * S. Buttcher, C. L. A. Clarke, and G. V. Cormack.
 *  Information Retrieval: Implementing and Evaluating Search Engines. MIT Press, Cambridge, MA,
 *  2010
 */
struct VarInt {
    /**
     * Maximum number of bytes an integer can compress to
     */
    static const std::size_t kMaxSizeBytes64 = 10;

    VarInt() = default;
    VarInt(std::uint64_t t) : _value(t) {}

    operator std::uint64_t() const {
        return _value;
    }

private:
    std::uint64_t _value{0};
};

template <>
struct DataType::Handler<VarInt> {
    /**
     * Compress a 64-bit integer and return the new buffer position.
     *
     * end should be the byte after the end of the buffer.
     *
     * Return nullptr for bad encoded data.
     */
    static Status load(
        VarInt* t, const char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset);

    /**
     * Compress a 64-bit integer and return the new buffer position
     */
    static Status store(
        const VarInt& t, char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset);

    static VarInt defaultConstruct() {
        return 0;
    }
};

}  // namespace mongo
