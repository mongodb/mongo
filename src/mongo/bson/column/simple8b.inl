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

#include "mongo/bson/column/simple8b_type_util.h"
#include "mongo/util/modules.h"

#include <limits>

namespace mongo::simple8b {
// Sentinel to represent missing, this value is not encodable in simple8b
inline constexpr int64_t kMissing = std::numeric_limits<int64_t>::max();

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
    template <typename T, typename Visit, typename VisitZero, typename VisitMissing>
    inline size_t visitAll(uint64_t encoded,
                           const Visit& visit,
                           const VisitZero& visitZero,
                           const VisitMissing& visitMissing) const {
        for (int i = iters; i; --i) {
            uint64_t slot = encoded & mask;
            if (slot != mask)
                visit(Simple8bTypeUtil::decodeInt64(slot));
            else
                visitMissing();
            encoded >>= bits;
        };
        return iters;
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
    static int64_t lastDecoded(uint64_t encoded) {
        encoded >>= (bits * (iters - 1));
        if (encoded == mask)
            return kMissing;
        return Simple8bTypeUtil::decodeInt64(encoded);
    }

    // Returns value of last slot. 'kMissing' is returned for missing.
    static uint64_t lastEncoded(uint64_t encoded) {
        encoded >>= (bits * (iters - 1));
        if (encoded == mask)
            return kMissing;
        return encoded;
    }

    // Returns true if no slots contain a missing value.
    static bool dense(uint64_t encoded) {
        for (int i = iters; i; --i) {
            if ((encoded & mask) == mask)
                return false;
            encoded >>= bits;
        }
        return true;
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
    template <typename T, typename Visit, typename VisitZero, typename VisitMissing>
    inline size_t visitAll(uint64_t encoded,
                           const Visit& visit,
                           const VisitZero& visitZero,
                           const VisitMissing& visitMissing) const {
        for (int i = iters; i; --i) {
            const auto& entry = table[encoded % entries];
            if (entry.num)
                visit(entry.decoded);
            else
                visitMissing();
            encoded >>= shift;
        };
        return iters;
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
    int64_t lastDecoded(uint64_t encoded) const {
        encoded >>= (bits * (iters - 1));
        const auto& entry = table[encoded];
        if (!entry.num) {
            return kMissing;
        }
        return entry.decoded;
    }

    uint64_t lastEncoded(uint64_t encoded) const {
        encoded >>= (bits * (iters - 1));
        const auto& entry = table[encoded];
        if (!entry.num) {
            return kMissing;
        }
        return encoded;
    }

    // Returns true if no slots contain a missing value.
    //
    // A missing value is encoded as all-1s in a slot. This uses SWAR (SIMD Within A Register) to
    // detect that pattern across all slots in parallel, without loops or branches per slot.
    //
    // The idea: AND all positions of each slot together. If a slot is all-1s (missing), the LSB of
    // that slow will be 1; otherwise it will be 0. We mask out the non-LSB positions of each slot
    // to check the result.
    //
    // Example with four 3-bit slots (bits=3, iters=4):
    //                          V
    //   encoded:        101  111  010  110
    //   encoded >> 1:   010  111  101  011
    //   encoded >> 2:   001  011  110  101
    //   -------------   ---  ---  ---  ---
    //   allSet (AND):   000  011  000  000
    //
    //   lsbMask:        001  001  001  001
    //   allSet & mask:  000  001  000  000  => non-zero, so not dense
    //                          ^
    static bool dense(uint64_t encoded) {
        // AND together shifts of the encoded value such that the LSB of each slot will contain the
        // ANDed value of all the bits in the slot. There may be 1s in more significant bits, but
        // we mask them out below.
        uint64_t allSet = encoded;
        for (int b = 1; b < bits; ++b) {
            allSet &= (encoded >> b);
        }

        // Create a bit mask that has the LSB set to 1 for each slot.
        constexpr uint64_t lsbMask = [] {
            uint64_t m = 0;
            for (int i = 0; i < iters; ++i)
                m |= 1ull << (i * bits);
            return m;
        }();

        // If there are any 1s in the LSB for any slot, there were missing values.
        return (allSet & lsbMask) == 0;
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
    template <typename T, typename Visit, typename VisitZero, typename VisitMissing>
    inline size_t visitAll(uint64_t encoded,
                           const Visit& visit,
                           const VisitZero& visitZero,
                           const VisitMissing& visitMissing) const {
        for (int i = 0; i < values; ++i) {
            if (encoded % 2)
                visitMissing();
            else
                visitZero();
            encoded >>= 1;
        }
        return values;
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

    // Returns true if no slots contain a missing value.
    // In the 1-bit encoding, 0=zero and 1=missing, so dense iff all payload bits are clear.
    bool dense(uint64_t encoded) const {
        return encoded == 0;
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
    template <typename T, typename Visit, typename VisitZero, typename VisitMissing>
    inline size_t visitAll(uint64_t encoded,
                           const Visit& visit,
                           const VisitZero& visitZero,
                           const VisitMissing& visitMissing) const {
        for (int i = iters; i; --i) {
            if ((encoded & mask) != mask) {
                uint64_t count = encoded & countMask;
                make_unsigned_t<T> value = (encoded >> countBits) & valueMask;
                auto numZeroes = count * countScale;
                // UBSAN will complain if shift values are greater than bit length
                if constexpr (std::is_same<make_unsigned_t<T>, uint64_t>::value) {
                    numZeroes %= 64;
                }

                visit(Simple8bTypeUtil::decodeInt(value << numZeroes));
            } else {
                visitMissing();
            }

            encoded >>= bits;
        };
        return iters;
    }

    // Calculate the sum of all slots
    template <typename T>
    T sum(uint64_t encoded) const {
        T decoded = 0;
        for (int i = iters; i; --i) {
            if ((encoded & mask) != mask) {
                uint64_t count = encoded & countMask;
                make_unsigned_t<T> value = (encoded >> countBits) & valueMask;
                auto numZeroes = count * countScale;
                // UBSAN will complain if shift values are greater than bit length
                if constexpr (std::is_same<make_unsigned_t<T>, uint64_t>::value) {
                    numZeroes %= 64;
                }

                decoded = add(decoded, Simple8bTypeUtil::decodeInt(value << numZeroes));
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
                auto numZeroes = count * countScale;
                // UBSAN will complain if shift values are greater than bit length
                if constexpr (std::is_same<make_unsigned_t<T>, uint64_t>::value) {
                    numZeroes %= 64;
                }

                prefix = add(prefix, Simple8bTypeUtil::decodeInt(value << numZeroes));
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
        auto numZeroes = count * countScale;
        // UBSAN will complain if shift values are greater than bit length
        if constexpr (std::is_same<make_unsigned_t<T>, uint64_t>::value) {
            numZeroes %= 64;
        }

        return Simple8bTypeUtil::decodeInt(value << numZeroes);
    }

    // Returns value of last slot. 'kMissing' is returned for missing.
    template <typename T>
    T lastDecoded(uint64_t encoded) const {
        encoded >>= (bits * (iters - 1));
        if ((encoded & mask) == mask)
            return kMissing;

        uint64_t count = encoded & countMask;
        make_unsigned_t<T> value = (encoded >> countBits) & valueMask;
        auto numZeroes = count * countScale;
        // UBSAN will complain if shift values are greater than bit length
        if constexpr (std::is_same<make_unsigned_t<T>, uint64_t>::value) {
            numZeroes %= 64;
        }

        return Simple8bTypeUtil::decodeInt(value << numZeroes);
    }

    // Returns value of last slot. 'kMissing' is returned for missing.
    template <typename T>
    T lastEncoded(uint64_t encoded) const {
        encoded >>= (bits * (iters - 1));
        if ((encoded & mask) == mask)
            return kMissing;

        uint64_t count = encoded & countMask;
        T value = (encoded >> countBits) & valueMask;
        auto numZeroes = count * countScale;
        // UBSAN will complain if shift values are greater than bit length
        if constexpr (std::is_same<T, uint64_t>::value) {
            numZeroes %= 64;
        }

        return value << numZeroes;
    }

    // Returns true if no slots contain a missing value.
    //
    // Uses the same SWAR technique as TableDecoder::dense(). See there for a detailed explanation.
    bool dense(uint64_t encoded) const {
        constexpr uint64_t lsbMask = [] {
            uint64_t m = 0;
            for (int i = 0; i < iters; ++i)
                m |= 1ull << (i * bits);
            return m;
        }();

        uint64_t allSet = encoded;
        for (int b = 1; b < bits; ++b) {
            allSet &= (encoded >> b);
        }
        return (allSet & lsbMask) == 0;
    }
};

// Storage for all decoders that we need for our various selector types
inline constexpr ParallelTableDecoder<2> decoderParallel2;
inline constexpr ParallelTableDecoder<3> decoderParallel3;
inline constexpr ParallelTableDecoder<4> decoderParallel4;
inline constexpr ParallelTableDecoder<5> decoderParallel5;
inline constexpr ParallelTableDecoder<6> decoderParallel6;
inline constexpr OneDecoder decoder1;
inline constexpr TableDecoder<2> decoder2;
inline constexpr TableDecoder<3> decoder3;
inline constexpr TableDecoder<4> decoder4;
inline constexpr TableDecoder<5> decoder5;
inline constexpr TableDecoder<6> decoder6;
inline constexpr TableDecoder<7> decoder7;
inline constexpr TableDecoder<8> decoder8;
inline constexpr TableDecoder<10> decoder10;
inline constexpr SimpleDecoder<12> decoder12;
inline constexpr SimpleDecoder<15> decoder15;
inline constexpr SimpleDecoder<20> decoder20;
inline constexpr SimpleDecoder<30> decoder30;
inline constexpr SimpleDecoder<60> decoder60;
inline constexpr ExtendedDecoder<2, 4, 1> decoderExtended7_1;
inline constexpr ExtendedDecoder<3, 4, 1> decoderExtended7_2;
inline constexpr ExtendedDecoder<4, 4, 1> decoderExtended7_3;
inline constexpr ExtendedDecoder<5, 4, 1> decoderExtended7_4;
inline constexpr ExtendedDecoder<7, 4, 1> decoderExtended7_5;
inline constexpr ExtendedDecoder<10, 4, 1> decoderExtended7_6;
inline constexpr ExtendedDecoder<14, 4, 1> decoderExtended7_7;
inline constexpr ExtendedDecoder<24, 4, 1> decoderExtended7_8;
inline constexpr ExtendedDecoder<52, 4, 1> decoderExtended7_9;
inline constexpr ExtendedDecoder<4, 4, 4> decoderExtended8_1;
inline constexpr ExtendedDecoder<5, 4, 4> decoderExtended8_2;
inline constexpr ExtendedDecoder<7, 4, 4> decoderExtended8_3;
inline constexpr ExtendedDecoder<10, 4, 4> decoderExtended8_4;
inline constexpr ExtendedDecoder<14, 4, 4> decoderExtended8_5;
inline constexpr ExtendedDecoder<24, 4, 4> decoderExtended8_6;
inline constexpr ExtendedDecoder<52, 4, 4> decoderExtended8_7;
inline constexpr ExtendedDecoder<4, 5, 4> decoderExtended8_8;
inline constexpr ExtendedDecoder<6, 5, 4> decoderExtended8_9;
inline constexpr ExtendedDecoder<9, 5, 4> decoderExtended8_10;
inline constexpr ExtendedDecoder<13, 5, 4> decoderExtended8_11;
inline constexpr ExtendedDecoder<23, 5, 4> decoderExtended8_12;
inline constexpr ExtendedDecoder<51, 5, 4> decoderExtended8_13;

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
            uasserted(10065906, "Bad selector");
            break;
    }
    return 0;
}

template <typename T>
T lastDecoded(uint64_t encoded) {
    auto selector = encoded & simple8b_internal::kBaseSelectorMask;
    encoded >>= 4;
    switch (selector) {
        case 1:
            // Encoded and decoded value is the same for the 1 bit case
            return decoder1.last(encoded);
        case 2:
            return decoder2.lastDecoded(encoded);
        case 3:
            return decoder3.lastDecoded(encoded);
        case 4:
            return decoder4.lastDecoded(encoded);
        case 5:
            return decoder5.lastDecoded(encoded);
        case 6:
            return decoder6.lastDecoded(encoded);
        case 7: {

            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    return decoder7.lastDecoded(encoded);
                case 1:
                    return decoderExtended7_1.lastDecoded<T>(encoded);
                case 2:
                    return decoderExtended7_2.lastDecoded<T>(encoded);
                case 3:
                    return decoderExtended7_3.lastDecoded<T>(encoded);
                case 4:
                    return decoderExtended7_4.lastDecoded<T>(encoded);
                case 5:
                    return decoderExtended7_5.lastDecoded<T>(encoded);
                case 6:
                    return decoderExtended7_6.lastDecoded<T>(encoded);
                case 7:
                    return decoderExtended7_7.lastDecoded<T>(encoded);
                case 8:
                    return decoderExtended7_8.lastDecoded<T>(encoded);
                case 9:
                    return decoderExtended7_9.lastDecoded<T>(encoded);
                default:
                    uasserted(10065900, "Bad extended selector");
                    break;
            }
            break;
        }
        case 8: {
            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    return decoder8.lastDecoded(encoded);
                case 1:
                    return decoderExtended8_1.lastDecoded<T>(encoded);
                case 2:
                    return decoderExtended8_2.lastDecoded<T>(encoded);
                case 3:
                    return decoderExtended8_3.lastDecoded<T>(encoded);
                case 4:
                    return decoderExtended8_4.lastDecoded<T>(encoded);
                case 5:
                    return decoderExtended8_5.lastDecoded<T>(encoded);
                case 6:
                    return decoderExtended8_6.lastDecoded<T>(encoded);
                case 7:
                    return decoderExtended8_7.lastDecoded<T>(encoded);
                case 8:
                    return decoderExtended8_8.lastDecoded<T>(encoded);
                case 9:
                    return decoderExtended8_9.lastDecoded<T>(encoded);
                case 10:
                    return decoderExtended8_10.lastDecoded<T>(encoded);
                case 11:
                    return decoderExtended8_11.lastDecoded<T>(encoded);
                case 12:
                    return decoderExtended8_12.lastDecoded<T>(encoded);
                case 13:
                    return decoderExtended8_13.lastDecoded<T>(encoded);
                default:
                    uasserted(10065901, "Bad extended selector");
                    break;
            }
            break;
        }
        case 9:
            return decoder10.lastDecoded(encoded);
        case 10:
            return decoder12.lastDecoded(encoded);
        case 11:
            return decoder15.lastDecoded(encoded);
        case 12:
            return decoder20.lastDecoded(encoded);
        case 13:
            return decoder30.lastDecoded(encoded);
        case 14:
            return decoder60.lastDecoded(encoded);
        case 15:
            break;
        default:
            uasserted(10065905, "Bad selector");
            break;
    }
    return 0;
}

template <typename T>
T lastEncoded(uint64_t encoded) {
    auto selector = encoded & simple8b_internal::kBaseSelectorMask;
    encoded >>= 4;
    switch (selector) {
        case 1:
            // Encoded and decoded value is the same for the 1 bit case
            return decoder1.last(encoded);
        case 2:
            return decoder2.lastEncoded(encoded);
        case 3:
            return decoder3.lastEncoded(encoded);
        case 4:
            return decoder4.lastEncoded(encoded);
        case 5:
            return decoder5.lastEncoded(encoded);
        case 6:
            return decoder6.lastEncoded(encoded);
        case 7: {

            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    return decoder7.lastEncoded(encoded);
                case 1:
                    return decoderExtended7_1.lastEncoded<T>(encoded);
                case 2:
                    return decoderExtended7_2.lastEncoded<T>(encoded);
                case 3:
                    return decoderExtended7_3.lastEncoded<T>(encoded);
                case 4:
                    return decoderExtended7_4.lastEncoded<T>(encoded);
                case 5:
                    return decoderExtended7_5.lastEncoded<T>(encoded);
                case 6:
                    return decoderExtended7_6.lastEncoded<T>(encoded);
                case 7:
                    return decoderExtended7_7.lastEncoded<T>(encoded);
                case 8:
                    return decoderExtended7_8.lastEncoded<T>(encoded);
                case 9:
                    return decoderExtended7_9.lastEncoded<T>(encoded);
                default:
                    uasserted(10065902, "Bad extended selector");
                    break;
            }
            break;
        }
        case 8: {
            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    return decoder8.lastEncoded(encoded);
                case 1:
                    return decoderExtended8_1.lastEncoded<T>(encoded);
                case 2:
                    return decoderExtended8_2.lastEncoded<T>(encoded);
                case 3:
                    return decoderExtended8_3.lastEncoded<T>(encoded);
                case 4:
                    return decoderExtended8_4.lastEncoded<T>(encoded);
                case 5:
                    return decoderExtended8_5.lastEncoded<T>(encoded);
                case 6:
                    return decoderExtended8_6.lastEncoded<T>(encoded);
                case 7:
                    return decoderExtended8_7.lastEncoded<T>(encoded);
                case 8:
                    return decoderExtended8_8.lastEncoded<T>(encoded);
                case 9:
                    return decoderExtended8_9.lastEncoded<T>(encoded);
                case 10:
                    return decoderExtended8_10.lastEncoded<T>(encoded);
                case 11:
                    return decoderExtended8_11.lastEncoded<T>(encoded);
                case 12:
                    return decoderExtended8_12.lastEncoded<T>(encoded);
                case 13:
                    return decoderExtended8_13.lastEncoded<T>(encoded);
                default:
                    uasserted(10065903, "Bad extended selector");
                    break;
            }
            break;
        }
        case 9:
            return decoder10.lastEncoded(encoded);
        case 10:
            return decoder12.lastEncoded(encoded);
        case 11:
            return decoder15.lastEncoded(encoded);
        case 12:
            return decoder20.lastEncoded(encoded);
        case 13:
            return decoder30.lastEncoded(encoded);
        case 14:
            return decoder60.lastEncoded(encoded);
        case 15:
            break;
        default:
            uasserted(10065904, "Bad selector");
            break;
    }
    return 0;
}

// Returns true if no slots in simple8b block contain a missing value.
MONGO_COMPILER_ALWAYS_INLINE inline bool decodeDense(uint64_t encoded) {
    auto selector = encoded & simple8b_internal::kBaseSelectorMask;
    encoded >>= 4;
    switch (selector) {
        case 1:
            return decoder1.dense(encoded);
        case 2:
            return decoder2.dense(encoded);
        case 3:
            return decoder3.dense(encoded);
        case 4:
            return decoder4.dense(encoded);
        case 5:
            return decoder5.dense(encoded);
        case 6:
            return decoder6.dense(encoded);
        case 7: {
            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    return decoder7.dense(encoded);
                case 1:
                    return decoderExtended7_1.dense(encoded);
                case 2:
                    return decoderExtended7_2.dense(encoded);
                case 3:
                    return decoderExtended7_3.dense(encoded);
                case 4:
                    return decoderExtended7_4.dense(encoded);
                case 5:
                    return decoderExtended7_5.dense(encoded);
                case 6:
                    return decoderExtended7_6.dense(encoded);
                case 7:
                    return decoderExtended7_7.dense(encoded);
                case 8:
                    return decoderExtended7_8.dense(encoded);
                case 9:
                    return decoderExtended7_9.dense(encoded);
                default:
                    uasserted(ErrorCodes::InvalidBSONColumn,
                              "Bad extended selector during decodeDense for selector 7");
            }
        }
        case 8: {
            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    return decoder8.dense(encoded);
                case 1:
                    return decoderExtended8_1.dense(encoded);
                case 2:
                    return decoderExtended8_2.dense(encoded);
                case 3:
                    return decoderExtended8_3.dense(encoded);
                case 4:
                    return decoderExtended8_4.dense(encoded);
                case 5:
                    return decoderExtended8_5.dense(encoded);
                case 6:
                    return decoderExtended8_6.dense(encoded);
                case 7:
                    return decoderExtended8_7.dense(encoded);
                case 8:
                    return decoderExtended8_8.dense(encoded);
                case 9:
                    return decoderExtended8_9.dense(encoded);
                case 10:
                    return decoderExtended8_10.dense(encoded);
                case 11:
                    return decoderExtended8_11.dense(encoded);
                case 12:
                    return decoderExtended8_12.dense(encoded);
                case 13:
                    return decoderExtended8_13.dense(encoded);
                default:
                    uasserted(ErrorCodes::InvalidBSONColumn,
                              "Bad extended selector during decodeDense for selector 8");
            }
        }
        case 9:
            return decoder10.dense(encoded);
        case 10:
            return decoder12.dense(encoded);
        case 11:
            return decoder15.dense(encoded);
        case 12:
            return decoder20.dense(encoded);
        case 13:
            return decoder30.dense(encoded);
        case 14:
            return decoder60.dense(encoded);
        case simple8b_internal::kRleSelector:
            // If the last value in the previous non-RLE block was missing, we would have returned
            // early, so if we arrive here we are still dense.
            return true;
        default:
            uasserted(ErrorCodes::InvalidBSONColumn, "Bad selector during decodeDense");
    }
    MONGO_UNREACHABLE;
}

// Decodes and visits all slots in simple8b block.
template <typename T, typename Visit, typename VisitZero, typename VisitMissing>
MONGO_COMPILER_ALWAYS_INLINE_GCC14 inline size_t decodeAndVisit(uint64_t encoded,
                                                                uint64_t* prevNonRLE,
                                                                const Visit& visit,
                                                                const VisitZero& visitZero,
                                                                const VisitMissing& visitMissing) {
    auto selector = encoded & simple8b_internal::kBaseSelectorMask;
    if (selector != simple8b_internal::kRleSelector) {
        *prevNonRLE = encoded;
    }
    encoded >>= 4;
    switch (selector) {
        case 1:  // Only 0 or missing deltas
            return decoder1.visitAll<T>(encoded, visit, visitZero, visitMissing);
            break;
        case 2:
            return decoder2.visitAll<T>(encoded, visit, visitZero, visitMissing);
            break;
        case 3:
            return decoder3.visitAll<T>(encoded, visit, visitZero, visitMissing);
            break;
        case 4:
            return decoder4.visitAll<T>(encoded, visit, visitZero, visitMissing);
            break;
        case 5:
            return decoder5.visitAll<T>(encoded, visit, visitZero, visitMissing);
            break;
        case 6:
            return decoder6.visitAll<T>(encoded, visit, visitZero, visitMissing);
            break;
        case 7: {
            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    return decoder7.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 1:
                    return decoderExtended7_1.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 2:
                    return decoderExtended7_2.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 3:
                    return decoderExtended7_3.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 4:
                    return decoderExtended7_4.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 5:
                    return decoderExtended7_5.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 6:
                    return decoderExtended7_6.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 7:
                    return decoderExtended7_7.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 8:
                    return decoderExtended7_8.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 9:
                    return decoderExtended7_9.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                default:
                    uasserted(ErrorCodes::InvalidBSONColumn,
                              "Bad extended selector during decodeAndVisit for selector 7");
                    break;
            }
            break;
        }
        case 8: {
            auto extended = encoded & simple8b_internal::kBaseSelectorMask;
            encoded >>= 4;
            switch (extended) {
                case 0:
                    return decoder8.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 1:
                    return decoderExtended8_1.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 2:
                    return decoderExtended8_2.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 3:
                    return decoderExtended8_3.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 4:
                    return decoderExtended8_4.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 5:
                    return decoderExtended8_5.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 6:
                    return decoderExtended8_6.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 7:
                    return decoderExtended8_7.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 8:
                    return decoderExtended8_8.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 9:
                    return decoderExtended8_9.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 10:
                    return decoderExtended8_10.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 11:
                    return decoderExtended8_11.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 12:
                    return decoderExtended8_12.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                case 13:
                    return decoderExtended8_13.visitAll<T>(encoded, visit, visitZero, visitMissing);
                    break;
                default:
                    uasserted(ErrorCodes::InvalidBSONColumn,
                              "Bad extended selector during decodeAndVisit for selector 8");
                    break;
            }
            break;
        }
        case 9:
            return decoder10.visitAll<T>(encoded, visit, visitZero, visitMissing);
            break;
        case 10:
            return decoder12.visitAll<T>(encoded, visit, visitZero, visitMissing);
            break;
        case 11:
            return decoder15.visitAll<T>(encoded, visit, visitZero, visitMissing);
            break;
        case 12:
            return decoder20.visitAll<T>(encoded, visit, visitZero, visitMissing);
            break;
        case 13:
            return decoder30.visitAll<T>(encoded, visit, visitZero, visitMissing);
            break;
        case 14:
            return decoder60.visitAll<T>(encoded, visit, visitZero, visitMissing);
            break;
        case simple8b_internal::kRleSelector: {
            const T lastValue = lastDecoded<T>(*prevNonRLE);
            size_t count = ((encoded & 0xf) + 1) * simple8b_internal::kRleMultiplier;
            if (lastValue == kMissing) {
                for (size_t i = 0; i < count; ++i) {
                    visitMissing();
                }
            } else if (lastValue == 0) {
                for (size_t i = 0; i < count; ++i) {
                    visitZero();
                }
            } else {
                for (size_t i = 0; i < count; ++i) {
                    visit(lastValue);
                }
            }
            return count;
            break;
        }
        default:
            uasserted(ErrorCodes::InvalidBSONColumn, "Bad selector during decodeAndVisit");
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
            // We cast to unsigned and back to avoid undefined behavior.  In C++,
            // overflow of an unsigned integer is defined to exhibit wrap-around
            // behavior which is what we want to preserve large deltas that may
            // sum to smaller deltas, whereas overflow of a signed integer is
            // undefined (although commonly the same in many implementations).
            // This casting will enforce wrap-around behavior for
            // signed values, and will be treated the same by the compiler.
            return static_cast<T>(
                static_cast<make_unsigned_t<T>>(decodeLastSlotIgnoreSkip<T>(*prevNonRLE)) *
                ((encoded & 0xf) + 1) * simple8b_internal::kRleMultiplier);
        default:
            break;
    }
    uasserted(ErrorCodes::InvalidBSONColumn, "Bad selector during decodeAndSum");
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
            T last = lastDecoded<T>(*prevNonRLE);
            if (last == kMissing)
                return 0;

            // Number of repeated values
            auto num = ((encoded & 0xf) + 1) * simple8b_internal::kRleMultiplier;
            // We can calculate prefix sum like this because num is always even and value is always
            // the same for RLE.
            // We cast to unsigned and back to avoid undefined behavior.  In C++,
            // overflow of an unsigned integer is defined to exhibit wrap-around
            // behavior which is what we want to preserve large deltas that may
            // sum to smaller deltas, whereas overflow of a signed integer is
            // undefined (although commonly the same in many implementations).
            // This casting will enforce wrap-around behavior for
            // signed values, and will be treated the same by the compiler.

            T sum = static_cast<T>((static_cast<make_unsigned_t<T>>(prefix) * num) +
                                   (static_cast<make_unsigned_t<T>>(last) * (num + 1) * (num / 2)));
            prefix = static_cast<T>(static_cast<make_unsigned_t<T>>(prefix) +
                                    static_cast<make_unsigned_t<T>>(last) * num);
            return sum;
        }
        default:
            break;
    }
    uasserted(ErrorCodes::InvalidBSONColumn, "Bad selector during decodeAndPrefixSum");
    return 0;
}

template <typename T, typename Visit, typename VisitZero, typename VisitMissing>
MONGO_COMPILER_ALWAYS_INLINE_GCC14 size_t visitAll(const char* buffer,
                                                   size_t size,
                                                   uint64_t& prevNonRLE,
                                                   const Visit& visit,
                                                   const VisitZero& visitZero,
                                                   const VisitMissing& visitMissing) {
    size_t numVisited = 0;
    invariant(size % 8 == 0);
    const char* end = buffer + size;
    while (buffer != end) {
        uint64_t encoded = ConstDataView(buffer).read<LittleEndian<uint64_t>>();
        numVisited += decodeAndVisit<T>(encoded, &prevNonRLE, visit, visitZero, visitMissing);
        buffer += sizeof(uint64_t);
    }
    return numVisited;
}

inline size_t count(const char* buffer, size_t size) {
    invariant(size % 8 == 0);
    const char* end = buffer + size;
    size_t numElements = 0;
    while (buffer != end) {
        uint64_t currentBlock = ConstDataView(buffer).read<LittleEndian<uint64_t>>();
        numElements += simple8b_internal::blockCount(currentBlock);
        buffer += sizeof(uint64_t);
    }
    return numElements;
}

inline bool dense(const char* buffer, size_t size) {
    invariant(size % 8 == 0);
    const char* end = buffer + size;

    while (buffer != end) {
        uint64_t currentBlock = ConstDataView(buffer).read<LittleEndian<uint64_t>>();
        if (!decodeDense(currentBlock))
            return false;
        buffer += sizeof(uint64_t);
    }
    return true;
}

template <typename T>
boost::optional<T> last(const char* buffer, size_t size, uint64_t& prevNonRLE) {
    invariant(size % 8 == 0);
    const char* end = buffer + size;
    while (buffer != end) {
        uint64_t encoded = ConstDataView(buffer).read<LittleEndian<uint64_t>>();
        auto selector = encoded & simple8b_internal::kBaseSelectorMask;
        if (selector != simple8b_internal::kRleSelector) {
            prevNonRLE = encoded;
        }

        buffer += sizeof(uint64_t);
    }

    if constexpr (std::is_same_v<T, uint64_t> || std::is_same_v<T, uint128_t>) {
        T encoded = lastEncoded<T>(prevNonRLE);
        return encoded == kMissing ? boost::optional<T>{} : boost::optional<T>{encoded};
    } else {
        T decoded = lastDecoded<T>(prevNonRLE);
        return decoded == kMissing ? boost::optional<T>{} : boost::optional<T>{decoded};
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
