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

#include "mongo/bson/util/simple8b_type_util.h"

#include <limits>

namespace mongo::simple8b {
namespace {

// Sentinel to represent missing, this value is not encodable in simple8b
static constexpr int64_t kMissing = std::numeric_limits<int64_t>::max();

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

    // Visit all values
    template <typename T,
              typename Visit,
              typename VisitMissing>
    inline void visitAll(uint64_t encoded,
                  const Visit& visit,
                  const VisitMissing& visitMissing) const {
        for (int i = iters; i; --i) {
            uint64_t slot = encoded & mask;
            if (slot != mask)
                visit(Simple8bTypeUtil::decodeInt64(slot));
            else
                visitMissing();
            encoded >>= bits;
        };
    }

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

    // Calculate the prefix sum of all slots.
    template <typename T>
    static T prefixSum(uint64_t encoded, T& prefix) {
        T decoded = 0;
        for (int i = iters; i; --i) {
            uint64_t slot = encoded & mask;
            if (slot != mask) {
                prefix = add(prefix, Simple8bTypeUtil::decodeInt64(slot));
                decoded = add(decoded, prefix);
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

    // Returns value of last slot. 'kMissing' is returned for missing.
    static int64_t last(uint64_t encoded) {
        encoded >>= (bits * (iters - 1));
        if (encoded == mask)
            return kMissing;
        return Simple8bTypeUtil::decodeInt64(encoded);
    }
};

// Table-based decoder that uses a lookup table for decoding unsigned integers into signed. Suitable
// for selectors with few bits per slot as the internal lookup table grows with bits per slot.
// Encoded should be be machine endian and first slot should start at least significant bit.
template <int bits>
struct TableDecoder {
    // Type to store in lookup table, depends on bit width per slot.
    using StorageT = std::conditional_t<bits <= 8, int8_t, int16_t>;

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
    static_assert(kMaxSlotValue <= std::numeric_limits<StorageT>::max(),
                  "lookup table cannot store full decoded value range");
    static_assert(kMinSlotValue >= std::numeric_limits<StorageT>::min(),
                  "lookup table cannot store full decoded value range");

    struct TableEntry {
        // Decoded signed value for this slot.
        StorageT decoded = 0;
        // Number of non-missing entries. Can be 0 or 1.
        int8_t num = 0;
    };
    TableEntry table[entries];

    // Initialize lookup table
    constexpr TableDecoder() : table() {
        for (unsigned i = 0; i < entries; ++i) {
            uint64_t slot = i;
            bool skip = slot == mask;
            if (!skip) {
                table[i].decoded = Simple8bTypeUtil::decodeInt64(slot);
                ++table[i].num;
            }
        }
    }

    // Visit all values
    template <typename T,
              typename Visit,
              typename VisitMissing>
    inline void visitAll(uint64_t encoded,
                  const Visit& visit,
                  const VisitMissing& visitMissing) const {
        for (int i = iters; i; --i) {
            const auto& entry = table[encoded % entries];
            if (entry.num)
                visit(entry.decoded);
            else
                visitMissing();
            encoded >>= shift;
        };
    }

    // Calculate the sum of all slots
    template <typename T>
    T sum(uint64_t encoded) const {
        T decoded = 0;
        for (int i = iters; i; --i) {
            decoded += table[encoded % entries].decoded;
            encoded >>= shift;
        };
        return decoded;
    }

    // Calculate the prefix sum of all slots.
    template <typename T>
    T prefixSum(uint64_t encoded, T& prefix) const {
        T decoded = 0;
        for (int i = iters; i; --i) {
            const auto& entry = table[encoded % entries];
            prefix = add(prefix, entry.decoded);
            decoded = add(decoded, prefix * entry.num);
            encoded >>= shift;
        };
        return decoded;
    }

    // Returns value of last slot. Treats missing as 0
    int64_t lastIgnoreSkip(uint64_t encoded) const {
        encoded >>= (bits * (iters - 1));
        return table[encoded].decoded;
    }

    // Returns value of last slot. 'kMissing' is returned for missing.
    int64_t last(uint64_t encoded) const {
        encoded >>= (bits * (iters - 1));
        const auto& entry = table[encoded];
        if (!entry.num) {
            return kMissing;
        }
        return entry.decoded;
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

    // Calculate number of values that needs to be stored for prefix sum
    static constexpr int numValuesForParallelPrefixSum(int parallel) {
        int sum = 0;
        for (int i = parallel; i; --i) {
            sum += i;
        }
        return sum;
    }

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
    static_assert(kMaxSlotValue * numValuesForParallelPrefixSum(parallel) <=
                      std::numeric_limits<int8_t>::max(),
                  "lookup table cannot store full decoded value range");
    static_assert(kMinSlotValue * numValuesForParallelPrefixSum(parallel) >=
                      std::numeric_limits<int8_t>::min(),
                  "lookup table cannot store full decoded value range");

    struct TableEntry {
        int8_t sum = 0;
        int8_t prefixSum = 0;
        int8_t num = 0;
    };
    TableEntry table[entries];

    // Initialize lookup table
    constexpr ParallelTableDecoder() : table() {
        for (unsigned i = 0; i < entries; ++i) {
            for (int j = 0; j < parallel; ++j) {
                uint64_t slot = (i >> (j * bits)) & mask;
                if (slot != mask) {
                    ++table[i].num;
                }
            }
            int8_t num = table[i].num;
            for (int j = 0; j < parallel; ++j) {
                uint64_t slot = (i >> (j * bits)) & mask;
                if (slot != mask) {
                    int64_t decoded = Simple8bTypeUtil::decodeInt64(slot);
                    table[i].sum += decoded;
                    table[i].prefixSum += decoded * (num--);
                }
            }
        }
    }

    // Calculate the sum of all slots
    template <typename T>
    T sum(uint64_t encoded) const {
        T decoded = 0;
        for (int i = iters; i; --i) {
            decoded = add(decoded, table[encoded % entries].sum);
            encoded >>= shift;
        };
        return decoded;
    }

    // Calculate the prefix sum of all slots.
    template <typename T>
    T prefixSum(uint64_t encoded, T& prefix) const {
        T decoded = 0;
        for (int i = iters; i; --i) {
            auto index = encoded % entries;
            const auto& entry = table[index];

            decoded = add(decoded, prefix * entry.num + entry.prefixSum);
            prefix = add(prefix, entry.sum);

            encoded >>= shift;
        };
        return decoded;
    }
};

// Special table-based decoder for the special 1 bit per slot case. The only two representable
// values is '0' or 'missing'. We can use this to take some shortcuts.
struct OneDecoder {
    // Constant to constrain table size, 2^X.
    static constexpr int kMaxTableSizeExp = 12;
    static constexpr int kMaxTableSize = 1 << kMaxTableSizeExp;

    static constexpr int bits = 1;
    static constexpr int values = 60;

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

    // Table contains number of non-skipped entries
    int8_t table[entries];

    // Initialize lookup table
    constexpr OneDecoder() : table() {
        for (unsigned i = 0; i < entries; ++i) {
            for (int j = 0; j < parallel; ++j) {
                uint64_t slot = (i >> (j * bits)) & mask;
                if (slot != mask) {
                    ++table[i];
                }
            }
        }
    }

    // Visit all values
    template <typename T,
              typename Visit,
              typename VisitMissing>
    inline void visitAll(uint64_t encoded,
                  const Visit& visit,
                  const VisitMissing& visitMissing) const {
        for (int i = 0; i < values; ++i) {
            if (encoded % 2)
                visitMissing();
            else
                visit(0);
            encoded >>= 1;
        }
    }

    // Calculate the prefix sum of all slots.
    template <typename T>
    T prefixSum(uint64_t encoded, T& prefix) const {
        T decoded = 0;
        for (int i = iters; i; --i) {
            auto num = table[encoded % entries];

            decoded = add(decoded, prefix * num);

            encoded >>= shift;
        };
        return decoded;
    }

    // Returns value of last slot. 'kMissing' is returned for missing.
    int64_t last(uint64_t encoded) const {
        encoded >>= (iters * parallel - 1);
        if (encoded == mask) {
            return kMissing;
        }
        return 0;
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

    // Visit all values
    template <typename T,
              typename Visit,
              typename VisitMissing>
    inline void visitAll(uint64_t encoded,
                  const Visit& visit,
                  const VisitMissing& visitMissing) const {
        for (int i = iters; i; --i) {
            if ((encoded & mask) != mask) {
                uint64_t count = encoded & countMask;
                make_unsigned_t<T> value = (encoded >> countBits) & valueMask;

                visit(Simple8bTypeUtil::decodeInt(value << (count * countScale)));
            } else {
                visitMissing();
            }

            encoded >>= bits;
        };
    }

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

    // Calculate the prefix sum of all slots.
    template <typename T>
    T prefixSum(uint64_t encoded, T& prefix) const {
        T decoded = 0;
        for (int i = iters; i; --i) {
            if ((encoded & mask) != mask) {
                uint64_t count = encoded & countMask;
                make_unsigned_t<T> value = (encoded >> countBits) & valueMask;

                prefix = add(prefix, Simple8bTypeUtil::decodeInt(value << (count * countScale)));
                decoded = add(decoded, prefix);
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

    // Returns value of last slot. 'kMissing' is returned for missing.
    template <typename T>
    T last(uint64_t encoded) const {
        encoded >>= (bits * (iters - 1));
        if ((encoded & mask) == mask)
            return kMissing;

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
static constexpr OneDecoder decoder1;
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

template <typename T>
T decodeLastSlot(uint64_t encoded) {
    auto selector = encoded & simple8b_internal::kBaseSelectorMask;
    encoded >>= 4;
    switch (selector) {
        case 1:
            return decoder1.last(encoded);
        case 2:
            return decoder2.last(encoded);
        case 3:
            return decoder3.last(encoded);
        case 4:
            return decoder4.last(encoded);
        case 5:
            return decoder5.last(encoded);
        case 6:
            return decoder6.last(encoded);
        case 7: {

            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    return decoder7.last(encoded);
                case 1:
                    return decoderExtended7_1.last<T>(encoded);
                case 2:
                    return decoderExtended7_2.last<T>(encoded);
                case 3:
                    return decoderExtended7_3.last<T>(encoded);
                case 4:
                    return decoderExtended7_4.last<T>(encoded);
                case 5:
                    return decoderExtended7_5.last<T>(encoded);
                case 6:
                    return decoderExtended7_6.last<T>(encoded);
                case 7:
                    return decoderExtended7_7.last<T>(encoded);
                case 8:
                    return decoderExtended7_8.last<T>(encoded);
                case 9:
                    return decoderExtended7_9.last<T>(encoded);
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
                    return decoder8.last(encoded);
                case 1:
                    return decoderExtended8_1.last<T>(encoded);
                case 2:
                    return decoderExtended8_2.last<T>(encoded);
                case 3:
                    return decoderExtended8_3.last<T>(encoded);
                case 4:
                    return decoderExtended8_4.last<T>(encoded);
                case 5:
                    return decoderExtended8_5.last<T>(encoded);
                case 6:
                    return decoderExtended8_6.last<T>(encoded);
                case 7:
                    return decoderExtended8_7.last<T>(encoded);
                case 8:
                    return decoderExtended8_8.last<T>(encoded);
                case 9:
                    return decoderExtended8_9.last<T>(encoded);
                case 10:
                    return decoderExtended8_10.last<T>(encoded);
                case 11:
                    return decoderExtended8_11.last<T>(encoded);
                case 12:
                    return decoderExtended8_12.last<T>(encoded);
                case 13:
                    return decoderExtended8_13.last<T>(encoded);
                default:
                    invariant(false);  // invalid encoding
                    break;
            }
            break;
        }
        case 9:
            return decoder10.last(encoded);
        case 10:
            return decoder12.last(encoded);
        case 11:
            return decoder15.last(encoded);
        case 12:
            return decoder20.last(encoded);
        case 13:
            return decoder30.last(encoded);
        case 14:
            return decoder60.last(encoded);
        case 15:
            break;
        default:
            break;
    }
    return 0;
}

// Decodes and visits all slots in simple8b block.
template <typename T, typename Visit, typename VisitMissing>
inline void decodeAndVisit(uint64_t encoded,
                    uint64_t* prevNonRLE,
                    const Visit& visit,
                    const VisitMissing& visitMissing) {
    auto selector = encoded & simple8b_internal::kBaseSelectorMask;
    if (selector != simple8b_internal::kRleSelector) {
        *prevNonRLE = encoded;
    }
    encoded >>= 4;
    switch (selector) {
        case 1:  // Only 0 or missing deltas
            decoder1.visitAll<T>(encoded, visit, visitMissing);
            break;
        case 2:
            decoder2.visitAll<T>(encoded, visit, visitMissing);
            break;
        case 3:
            decoder3.visitAll<T>(encoded, visit, visitMissing);
            break;
        case 4:
            decoder4.visitAll<T>(encoded, visit, visitMissing);
            break;
        case 5:
            decoder5.visitAll<T>(encoded, visit, visitMissing);
            break;
        case 6:
            decoder6.visitAll<T>(encoded, visit, visitMissing);
            break;
        case 7: {
            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    decoder7.visitAll<T>(encoded, visit, visitMissing);
                    break;
                case 1:
                    decoderExtended7_1.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 2:
                    decoderExtended7_2.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 3:
                    decoderExtended7_3.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 4:
                    decoderExtended7_4.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 5:
                    decoderExtended7_5.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 6:
                    decoderExtended7_6.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 7:
                    decoderExtended7_7.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 8:
                    decoderExtended7_8.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 9:
                    decoderExtended7_9.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
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
                    decoder8.visitAll<T>(encoded, visit, visitMissing);
                    break;
                case 1:
                    decoderExtended8_1.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 2:
                    decoderExtended8_2.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 3:
                    decoderExtended8_3.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 4:
                    decoderExtended8_4.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 5:
                    decoderExtended8_5.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 6:
                    decoderExtended8_6.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 7:
                    decoderExtended8_7.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 8:
                    decoderExtended8_8.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 9:
                    decoderExtended8_9.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 10:
                    decoderExtended8_10.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 11:
                    decoderExtended8_11.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 12:
                    decoderExtended8_12.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                case 13:
                    decoderExtended8_13.visitAll<T>(
                        encoded, visit, visitMissing);
                    break;
                default:
                    break;
            }
            break;
        }
        case 9:
            decoder10.visitAll<T>(encoded, visit, visitMissing);
            break;
        case 10:
            decoder12.visitAll<T>(encoded, visit, visitMissing);
            break;
        case 11:
            decoder15.visitAll<T>(encoded, visit, visitMissing);
            break;
        case 12:
            decoder20.visitAll<T>(encoded, visit, visitMissing);
            break;
        case 13:
            decoder30.visitAll<T>(encoded, visit, visitMissing);
            break;
        case 14:
            decoder60.visitAll<T>(encoded, visit, visitMissing);
            break;
        case simple8b_internal::kRleSelector: {
            const T lastValue = decodeLastSlot<T>(*prevNonRLE);
            size_t count = ((encoded & 0xf) + 1) * simple8b_internal::kRleMultiplier;
            if (lastValue == kMissing) {
                for (size_t i = 0; i < count; ++i) {
                    visitMissing();
                }
            } else {
                for (size_t i = 0; i < count; ++i) {
                    visit(lastValue);
                }
            }
            break;
        }
        default:
            fassertFailed(8586000);
            break;
    }
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

// Decodes and sums all slots in simple8b block, writes last encountered non-rle block in
// 'prevNonRLE'.
template <typename T>
T decodeAndPrefixSum(uint64_t encoded, T& prefix, uint64_t* prevNonRLE) {
    auto selector = encoded & simple8b_internal::kBaseSelectorMask;
    if (selector != simple8b_internal::kRleSelector) {
        *prevNonRLE = encoded;
    }
    encoded >>= 4;
    switch (selector) {
        case 1:
            return decoder1.prefixSum<T>(encoded, prefix);
        case 2:
            return decoderParallel2.prefixSum<T>(encoded, prefix);
        case 3:
            return decoderParallel3.prefixSum<T>(encoded, prefix);
        case 4:
            return decoderParallel4.prefixSum<T>(encoded, prefix);
        case 5:
            return decoderParallel5.prefixSum<T>(encoded, prefix);
        case 6:
            return decoderParallel6.prefixSum<T>(encoded, prefix);
        case 7: {
            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    return decoder7.prefixSum<T>(encoded, prefix);
                case 1:
                    return decoderExtended7_1.prefixSum<T>(encoded, prefix);
                case 2:
                    return decoderExtended7_2.prefixSum<T>(encoded, prefix);
                case 3:
                    return decoderExtended7_3.prefixSum<T>(encoded, prefix);
                case 4:
                    return decoderExtended7_4.prefixSum<T>(encoded, prefix);
                case 5:
                    return decoderExtended7_5.prefixSum<T>(encoded, prefix);
                case 6:
                    return decoderExtended7_6.prefixSum<T>(encoded, prefix);
                case 7:
                    return decoderExtended7_7.prefixSum<T>(encoded, prefix);
                case 8:
                    return decoderExtended7_8.prefixSum<T>(encoded, prefix);
                case 9:
                    return decoderExtended7_9.prefixSum<T>(encoded, prefix);
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
                    return decoder8.prefixSum<T>(encoded, prefix);
                case 1:
                    return decoderExtended8_1.prefixSum<T>(encoded, prefix);
                case 2:
                    return decoderExtended8_2.prefixSum<T>(encoded, prefix);
                case 3:
                    return decoderExtended8_3.prefixSum<T>(encoded, prefix);
                case 4:
                    return decoderExtended8_4.prefixSum<T>(encoded, prefix);
                case 5:
                    return decoderExtended8_5.prefixSum<T>(encoded, prefix);
                case 6:
                    return decoderExtended8_6.prefixSum<T>(encoded, prefix);
                case 7:
                    return decoderExtended8_7.prefixSum<T>(encoded, prefix);
                case 8:
                    return decoderExtended8_8.prefixSum<T>(encoded, prefix);
                case 9:
                    return decoderExtended8_9.prefixSum<T>(encoded, prefix);
                case 10:
                    return decoderExtended8_10.prefixSum<T>(encoded, prefix);
                case 11:
                    return decoderExtended8_11.prefixSum<T>(encoded, prefix);
                case 12:
                    return decoderExtended8_12.prefixSum<T>(encoded, prefix);
                case 13:
                    return decoderExtended8_13.prefixSum<T>(encoded, prefix);
                default:
                    break;
            }
            break;
        }
        case 9:
            return decoder10.prefixSum<T>(encoded, prefix);
        case 10:
            return decoder12.prefixSum<T>(encoded, prefix);
        case 11:
            return decoder15.prefixSum<T>(encoded, prefix);
        case 12:
            return decoder20.prefixSum<T>(encoded, prefix);
        case 13:
            return decoder30.prefixSum<T>(encoded, prefix);
        case 14:
            return decoder60.prefixSum<T>(encoded, prefix);
        case simple8b_internal::kRleSelector: {
            T last = decodeLastSlot<T>(*prevNonRLE);
            if (last == kMissing)
                return 0;

            // Number of repeated values
            auto num = ((encoded & 0xf) + 1) * simple8b_internal::kRleMultiplier;
            // We can calculate prefix sum like this because num is always even and value is always
            // the same for RLE.
            T sum = add(prefix * num, last * (num + 1) * (num / 2));
            prefix = add(prefix, last * num);
            return sum;
        }
        default:
            break;
    }
    fassertFailed(8297300);
    return 0;
}

}  // namespace

template <typename T, typename Visit, typename VisitMissing>
inline void visitAll(const char* buffer,
              size_t size,
              uint64_t& prevNonRLE,
              const Visit& visit,
              const VisitMissing& visitMissing) {
    invariant(size % 8 == 0);
    const char* end = buffer + size;
    while (buffer != end) {
        uint64_t encoded = ConstDataView(buffer).read<LittleEndian<uint64_t>>();
        decodeAndVisit<T>(encoded, &prevNonRLE, visit, visitMissing);
        buffer += sizeof(uint64_t);
    }
}

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

template <typename T>
T prefixSum(const char* buffer, size_t size, T& prefix, uint64_t& prevNonRLE) {
    invariant(size % 8 == 0);
    const char* end = buffer + size;
    T sum = 0;
    while (buffer != end) {
        uint64_t encoded = ConstDataView(buffer).read<LittleEndian<uint64_t>>();
        sum = add(sum, decodeAndPrefixSum<T>(encoded, prefix, &prevNonRLE));
        buffer += sizeof(uint64_t);
    }
    return sum;
}

}  // namespace mongo::simple8b
