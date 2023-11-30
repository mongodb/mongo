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

#include "mongo/bson/util/simple8b.h"
#include "mongo/bson/util/simple8b_type_util.h"

#include <limits>

namespace mongo::simple8b {
namespace {

// Performs addition as unsigned and cast back to signed to get overflow defined to wrapped around
// instead of undefined behavior.
static constexpr int64_t add(int64_t lhs, int64_t rhs) {
    return static_cast<int64_t>(static_cast<uint64_t>(lhs) + static_cast<uint64_t>(rhs));
}

static constexpr int128_t add(int128_t lhs, int128_t rhs) {
    return static_cast<int128_t>(static_cast<uint128_t>(lhs) + static_cast<uint128_t>(rhs));
}

// Simple Simple8b decoder for decoding any basic simple8b block where all bits are used for the
// value, decodes signed integer at runtime. Suitable for selectors with many bits per slot. Encoded
// should be be machine endian and first slot should start at least significant bit.
template <int bits>
struct SimpleDecoder {
    // Number of values in this block.
    static constexpr int iters = 60 / bits;

    // Bit mask to extract a single slot and to check for the missing bit pattern.
    static constexpr uint64_t mask = (1ull << bits) - 1;

    // Calculate the sum of all slots.
    template <typename T>
    static T sum(uint64_t encoded) {
        T decoded = 0;
        for (int i = iters; i; --i) {
            uint64_t slot = encoded & mask;
            if (slot != mask) {
                decoded = add(decoded, Simple8bTypeUtil::decodeInt64(slot));
            }
            encoded >>= bits;
        };
        return decoded;
    }

    // Returns value of last slot. Treats missing as 0.
    static int64_t lastIgnoreSkip(uint64_t encoded) {
        encoded >>= (bits * (iters - 1));
        if (encoded == mask)
            return 0;
        return Simple8bTypeUtil::decodeInt64(encoded);
    }
};

// Table-based decoder that uses a lookup table for decoding unsigned integers into signed. Suitable
// for selectors with few bits per slot as the internal lookup table grows with bits per slot.
// Encoded should be be machine endian and first slot should start at least significant bit.
template <int bits>
struct TableDecoder {
    // Type to store in lookup table, depends on bit width per slot.
    using T = std::conditional_t<bits <= 8, int8_t, int16_t>;

    // Constant to constrain table size.
    static constexpr int kMaxTableSize = 1 << 13;

    static constexpr int shift = bits;

    // Number of values in this block
    static constexpr int iters = (60 / bits * bits + shift - 1) / shift;
    // Number of entries in lookup table
    static constexpr int entries = 1 << shift;
    // Bit mask to extract a single slot and to check for the missing bit pattern.
    static constexpr uint64_t mask = (1ull << bits) - 1;

    // Largest possible value that can be stored in this slot
    static constexpr int64_t kMaxSlotValue = Simple8bTypeUtil::decodeInt64(mask - 1);
    // Smallest possible value that can be stored in this slot
    static constexpr int64_t kMinSlotValue = Simple8bTypeUtil::decodeInt64(mask - 2);

    // Verify that lookup table is within size limit and that it can store our possible range of
    // values
    static_assert(entries <= kMaxTableSize, "lookup table too large");
    static_assert(kMaxSlotValue <= std::numeric_limits<T>::max(),
                  "lookup table cannot store full decoded value range");
    static_assert(kMinSlotValue >= std::numeric_limits<T>::min(),
                  "lookup table cannot store full decoded value range");

    T table[entries];

    // Initialize lookup table
    constexpr TableDecoder() : table() {
        for (unsigned i = 0; i < entries; ++i) {
            uint64_t slot = i;
            bool skip = slot == mask;
            if (!skip) {
                table[i] += Simple8bTypeUtil::decodeInt64(slot);
            }
        }
    }

    // Calculate the sum of all slots
    template <typename T>
    T sum(uint64_t encoded) const {
        T decoded = 0;
        for (int i = iters; i; --i) {
            decoded += table[encoded % entries];
            encoded >>= shift;
        };
        return decoded;
    }

    // Returns value of last slot. Treats missing as 0
    int64_t lastIgnoreSkip(uint64_t encoded) const {
        encoded >>= (bits * (iters - 1));
        return table[encoded];
    }
};

// Table-based decoder that uses a lookup table for decoding multiple unsigned integers into signed
// at once. Suitable for selectors with few bits per slot as the internal lookup table grows with
// bits per slot. Encoded should be be machine endian and first slot should start at least
// significant bit.
template <int bits>
struct ParallelTableDecoder {
    // Constant to constrain table size, 2^X.
    static constexpr int kMaxTableSizeExp = 13;
    static constexpr int kMaxTableSize = 1 << kMaxTableSizeExp;

    // Number of slots that we can decode together
    static constexpr int parallel = kMaxTableSizeExp / bits;

    // Number of shift to get to the next decoding iteration.
    static constexpr int shift = bits * parallel;
    // Number of decoding iterations in this block
    static constexpr int iters = (60 / bits * bits + shift - 1) / shift;
    // Number of entries in lookup table
    static constexpr int entries = 1 << shift;
    // Bit mask to extract a single slot and to check for the missing bit pattern.
    static constexpr uint64_t mask = (1ull << bits) - 1;

    // Largest possible value that can be stored in this slot
    static constexpr int64_t kMaxSlotValue = Simple8bTypeUtil::decodeInt64(mask - 1);
    // Smallest possible value that can be stored in this slot
    static constexpr int64_t kMinSlotValue = Simple8bTypeUtil::decodeInt64(mask - 2);

    // Verify that lookup table is within size limit and that it can store our possible range of
    // values
    static_assert(
        bits > 1,
        "simple8b slots needs to use at least 2 bits to be meaningful for parallel decoding");
    static_assert(parallel > 1, "bit size too large to fit in table for parallel decoding");
    static_assert(kMaxSlotValue * parallel <= std::numeric_limits<int8_t>::max(),
                  "lookup table cannot store full decoded value range");
    static_assert(kMinSlotValue * parallel >= std::numeric_limits<int8_t>::min(),
                  "lookup table cannot store full decoded value range");

    int8_t table[entries];

    // Initialize lookup table
    constexpr ParallelTableDecoder() : table() {
        for (unsigned i = 0; i < entries; ++i) {
            for (int j = 0; j < parallel; ++j) {
                uint64_t slot = (i >> (j * bits)) & mask;
                if (slot != mask) {
                    table[i] += Simple8bTypeUtil::decodeInt64(slot);
                }
            }
        }
    }

    // Calculate the sum of all slots
    template <typename T>
    T sum(uint64_t encoded) const {
        T decoded = 0;
        for (int i = iters; i; --i) {
            decoded = add(decoded, table[encoded % entries]);
            encoded >>= shift;
        };
        return decoded;
    }
};

// Special Simple8b decoder for decoding the extended selectors where the slot bits are split up in
// a value and count for a left shift. Encoded should be be machine endian and first slot should
// start at least significant bit.
template <int valueBits, int countBits, int countScale>
struct ExtendedDecoder {
    static constexpr int bits = valueBits + countBits;
    static constexpr int iters = 56 / bits;
    static constexpr uint64_t mask = (1ull << bits) - 1;
    static constexpr uint64_t valueMask = (1ull << valueBits) - 1;
    static constexpr uint64_t countMask = (1ull << countBits) - 1;

    // Calculate the sum of all slots
    template <typename T>
    T sum(uint64_t encoded) const {
        T decoded = 0;
        for (int i = iters; i; --i) {
            if ((encoded & mask) != mask) {
                uint64_t count = encoded & countMask;
                make_unsigned_t<T> value = (encoded >> countBits) & valueMask;

                decoded = add(decoded, Simple8bTypeUtil::decodeInt(value << (count * countScale)));
            }

            encoded >>= bits;
        };
        return decoded;
    }

    // Returns value of last slot. Treats missing as 0
    template <typename T>
    T lastIgnoreSkip(uint64_t encoded) const {
        encoded >>= (bits * (iters - 1));
        if ((encoded & mask) == mask)
            return 0;

        uint64_t count = encoded & countMask;
        make_unsigned_t<T> value = (encoded >> countBits) & valueMask;

        return Simple8bTypeUtil::decodeInt(value << (count * countScale));
    }
};

// Storage for all decoders that we need for our various selector types
static constexpr ParallelTableDecoder<2> decoderParallel2;
static constexpr ParallelTableDecoder<3> decoderParallel3;
static constexpr ParallelTableDecoder<4> decoderParallel4;
static constexpr ParallelTableDecoder<5> decoderParallel5;
static constexpr ParallelTableDecoder<6> decoderParallel6;
static constexpr TableDecoder<2> decoder2;
static constexpr TableDecoder<3> decoder3;
static constexpr TableDecoder<4> decoder4;
static constexpr TableDecoder<5> decoder5;
static constexpr TableDecoder<6> decoder6;
static constexpr TableDecoder<7> decoder7;
static constexpr TableDecoder<8> decoder8;
static constexpr TableDecoder<10> decoder10;
static constexpr TableDecoder<12> decoder12;
static constexpr SimpleDecoder<15> decoder15;
static constexpr SimpleDecoder<20> decoder20;
static constexpr SimpleDecoder<30> decoder30;
static constexpr SimpleDecoder<60> decoder60;
static constexpr ExtendedDecoder<2, 4, 1> decoderExtended7_1;
static constexpr ExtendedDecoder<3, 4, 1> decoderExtended7_2;
static constexpr ExtendedDecoder<4, 4, 1> decoderExtended7_3;
static constexpr ExtendedDecoder<5, 4, 1> decoderExtended7_4;
static constexpr ExtendedDecoder<7, 4, 1> decoderExtended7_5;
static constexpr ExtendedDecoder<10, 4, 1> decoderExtended7_6;
static constexpr ExtendedDecoder<14, 4, 1> decoderExtended7_7;
static constexpr ExtendedDecoder<24, 4, 1> decoderExtended7_8;
static constexpr ExtendedDecoder<52, 4, 1> decoderExtended7_9;
static constexpr ExtendedDecoder<4, 4, 4> decoderExtended8_1;
static constexpr ExtendedDecoder<5, 4, 4> decoderExtended8_2;
static constexpr ExtendedDecoder<7, 4, 4> decoderExtended8_3;
static constexpr ExtendedDecoder<10, 4, 4> decoderExtended8_4;
static constexpr ExtendedDecoder<14, 4, 4> decoderExtended8_5;
static constexpr ExtendedDecoder<24, 4, 4> decoderExtended8_6;
static constexpr ExtendedDecoder<52, 4, 4> decoderExtended8_7;
static constexpr ExtendedDecoder<4, 5, 4> decoderExtended8_8;
static constexpr ExtendedDecoder<6, 5, 4> decoderExtended8_9;
static constexpr ExtendedDecoder<9, 5, 4> decoderExtended8_10;
static constexpr ExtendedDecoder<13, 5, 4> decoderExtended8_11;
static constexpr ExtendedDecoder<23, 5, 4> decoderExtended8_12;
static constexpr ExtendedDecoder<51, 5, 4> decoderExtended8_13;

// Decodes last slot for simple8b block. Treats missing as 0.
template <typename T>
T decodeLastSlotIgnoreSkip(uint64_t encoded) {
    auto selector = encoded & simple8b_internal::kBaseSelectorMask;
    encoded >>= 4;
    switch (selector) {
        case 1:  // Only 0 or missing deltas
            break;
        case 2:
            return decoder2.lastIgnoreSkip(encoded);
        case 3:
            return decoder3.lastIgnoreSkip(encoded);
        case 4:
            return decoder4.lastIgnoreSkip(encoded);
        case 5:
            return decoder5.lastIgnoreSkip(encoded);
        case 6:
            return decoder6.lastIgnoreSkip(encoded);
        case 7: {

            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    return decoder7.lastIgnoreSkip(encoded);
                case 1:
                    return decoderExtended7_1.lastIgnoreSkip<T>(encoded);
                case 2:
                    return decoderExtended7_2.lastIgnoreSkip<T>(encoded);
                case 3:
                    return decoderExtended7_3.lastIgnoreSkip<T>(encoded);
                case 4:
                    return decoderExtended7_4.lastIgnoreSkip<T>(encoded);
                case 5:
                    return decoderExtended7_5.lastIgnoreSkip<T>(encoded);
                case 6:
                    return decoderExtended7_6.lastIgnoreSkip<T>(encoded);
                case 7:
                    return decoderExtended7_7.lastIgnoreSkip<T>(encoded);
                case 8:
                    return decoderExtended7_8.lastIgnoreSkip<T>(encoded);
                case 9:
                    return decoderExtended7_9.lastIgnoreSkip<T>(encoded);
                default:
                    invariant(false);  // invalid encoding
                    break;
            }
            break;
        }
        case 8: {
            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    return decoder8.lastIgnoreSkip(encoded);
                case 1:
                    return decoderExtended8_1.lastIgnoreSkip<T>(encoded);
                case 2:
                    return decoderExtended8_2.lastIgnoreSkip<T>(encoded);
                case 3:
                    return decoderExtended8_3.lastIgnoreSkip<T>(encoded);
                case 4:
                    return decoderExtended8_4.lastIgnoreSkip<T>(encoded);
                case 5:
                    return decoderExtended8_5.lastIgnoreSkip<T>(encoded);
                case 6:
                    return decoderExtended8_6.lastIgnoreSkip<T>(encoded);
                case 7:
                    return decoderExtended8_7.lastIgnoreSkip<T>(encoded);
                case 8:
                    return decoderExtended8_8.lastIgnoreSkip<T>(encoded);
                case 9:
                    return decoderExtended8_9.lastIgnoreSkip<T>(encoded);
                case 10:
                    return decoderExtended8_10.lastIgnoreSkip<T>(encoded);
                case 11:
                    return decoderExtended8_11.lastIgnoreSkip<T>(encoded);
                case 12:
                    return decoderExtended8_12.lastIgnoreSkip<T>(encoded);
                case 13:
                    return decoderExtended8_13.lastIgnoreSkip<T>(encoded);
                default:
                    invariant(false);  // invalid encoding
                    break;
            }
            break;
        }
        case 9:
            return decoder10.lastIgnoreSkip(encoded);
        case 10:
            return decoder12.lastIgnoreSkip(encoded);
        case 11:
            return decoder15.lastIgnoreSkip(encoded);
        case 12:
            return decoder20.lastIgnoreSkip(encoded);
        case 13:
            return decoder30.lastIgnoreSkip(encoded);
        case 14:
            return decoder60.lastIgnoreSkip(encoded);
        case 15:
            break;
        default:
            break;
    }
    return 0;
}

// Decodes and sums all slots in simple8b block, writes last encountered non-rle block in
// 'prevNonRLE'.
template <typename T>
T decodeAndSum(uint64_t encoded, uint64_t* prevNonRLE) {
    auto selector = encoded & simple8b_internal::kBaseSelectorMask;
    if (selector != simple8b_internal::kRleSelector) {
        *prevNonRLE = encoded;
    }
    encoded >>= 4;
    switch (selector) {
        case 1:  // Only 0 or missing deltas
            return 0;
        case 2:
            return decoderParallel2.sum<T>(encoded);
        case 3:
            return decoderParallel3.sum<T>(encoded);
        case 4:
            return decoderParallel4.sum<T>(encoded);
        case 5:
            return decoderParallel5.sum<T>(encoded);
        case 6:
            return decoderParallel6.sum<T>(encoded);
        case 7: {
            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    return decoder7.sum<T>(encoded);
                case 1:
                    return decoderExtended7_1.sum<T>(encoded);
                case 2:
                    return decoderExtended7_2.sum<T>(encoded);
                case 3:
                    return decoderExtended7_3.sum<T>(encoded);
                case 4:
                    return decoderExtended7_4.sum<T>(encoded);
                case 5:
                    return decoderExtended7_5.sum<T>(encoded);
                case 6:
                    return decoderExtended7_6.sum<T>(encoded);
                case 7:
                    return decoderExtended7_7.sum<T>(encoded);
                case 8:
                    return decoderExtended7_8.sum<T>(encoded);
                case 9:
                    return decoderExtended7_9.sum<T>(encoded);
                default:
                    break;
            }
            break;
        }
        case 8: {
            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    return decoder8.sum<T>(encoded);
                case 1:
                    return decoderExtended8_1.sum<T>(encoded);
                case 2:
                    return decoderExtended8_2.sum<T>(encoded);
                case 3:
                    return decoderExtended8_3.sum<T>(encoded);
                case 4:
                    return decoderExtended8_4.sum<T>(encoded);
                case 5:
                    return decoderExtended8_5.sum<T>(encoded);
                case 6:
                    return decoderExtended8_6.sum<T>(encoded);
                case 7:
                    return decoderExtended8_7.sum<T>(encoded);
                case 8:
                    return decoderExtended8_8.sum<T>(encoded);
                case 9:
                    return decoderExtended8_9.sum<T>(encoded);
                case 10:
                    return decoderExtended8_10.sum<T>(encoded);
                case 11:
                    return decoderExtended8_11.sum<T>(encoded);
                case 12:
                    return decoderExtended8_12.sum<T>(encoded);
                case 13:
                    return decoderExtended8_13.sum<T>(encoded);
                default:
                    break;
            }
            break;
        }
        case 9:
            return decoder10.sum<T>(encoded);
        case 10:
            return decoder12.sum<T>(encoded);
        case 11:
            return decoder15.sum<T>(encoded);
        case 12:
            return decoder20.sum<T>(encoded);
        case 13:
            return decoder30.sum<T>(encoded);
        case 14:
            return decoder60.sum<T>(encoded);
        case simple8b_internal::kRleSelector:
            return decodeLastSlotIgnoreSkip<T>(*prevNonRLE) * ((encoded & 0xf) + 1) *
                simple8b_internal::kRleMultiplier;
        default:
            break;
    }
    fassertFailed(8297100);
    return 0;
}

}  // namespace

template <typename T>
T sum(const char* buffer, size_t size, uint64_t& prevNonRLE) {
    invariant(size % 8 == 0);
    const char* end = buffer + size;
    T sum = 0;
    while (buffer != end) {
        uint64_t encoded = ConstDataView(buffer).read<LittleEndian<uint64_t>>();
        sum = add(sum, decodeAndSum<T>(encoded, &prevNonRLE));
        buffer += sizeof(uint64_t);
    }
    return sum;
}

// Explicit template instantiations for our supported types
template int64_t sum<int64_t>(const char*, size_t, uint64_t&);
template int128_t sum<int128_t>(const char*, size_t, uint64_t&);

}  // namespace mongo::simple8b
