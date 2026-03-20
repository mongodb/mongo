/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/util/modules.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mongo::sbe {


/**
 * Split Block Bloom Filter is a Bloom Filter variant designed for cache-friendly SIMD-optimized
 * operations. The basic idea is to hash the item to a tiny 32-byte sized bloom filter which fits in
 * a single cache line and can utilize simd instructions. https://arxiv.org/pdf/2101.01719
 */
class SplitBlockBloomFilter {
public:
    static size_t optimalNumBytes(size_t expectedElementsNum, double falsePositiveRate) {
        /*
        The standard approximation of the false positive rate (p) in standard bloom filter is
        p ≈ (1 - e^(-kn/m))^k
            where
            m = size (in bits) of the set
            n = number of items inserted
            k = number of hash functions
        For SBBF, we use k=8 hash functions so that we can leverage simd instructions.
        SBBF is slightly less space efficient than standard bloom filter but for simplicity we will
        reuse this formula.
        */
        size_t numBits = static_cast<size_t>(-8.0 * expectedElementsNum /
                                             std::log(1 - std::pow(falsePositiveRate, 1.0 / 8)));
        return numBits >> 3;
    }

    explicit SplitBlockBloomFilter(size_t numBytes) {
        size_t blocksNeeded = (numBytes + 31) / 32;

        // Ensure strictly power of 2 for faster modulo (masking) operations
        _numBlocks = 1;
        while (_numBlocks < blocksNeeded) {
            _numBlocks <<= 1;
        }
        _blocks.resize(_numBlocks);
    }

    void insert(size_t h) {
        uint32_t blockIdx = (h >> 32) & (_numBlocks - 1);
        uint32_t hashLow = (uint32_t)h;

        for (int i = 0; i < 8; ++i) {
            uint32_t mixed = hashLow * SALT[i];
            uint32_t bitPos = mixed >> 27;
            uint32_t mask = (1U << bitPos);
            _blocks[blockIdx].words[i] |= mask;
        }
    }

    [[nodiscard]] bool maybeContains(size_t h) const {
        uint32_t block_idx = (h >> 32) & (_numBlocks - 1);
        uint32_t hash_low = (uint32_t)h;

        uint32_t misses = 0;
        for (int i = 0; i < 8; ++i) {
            uint32_t mixed = hash_low * SALT[i];
            uint32_t bitPos = mixed >> 27;
            uint32_t mask = (1U << bitPos);
            misses |= (mask & ~_blocks[block_idx].words[i]);
        }

        // If misses is 0, it means all requested bits were found.
        return misses == 0;
    }

    size_t memoryUsage() const {
        return _blocks.size() * sizeof(Block);
    }

    void clear() {
        std::memset(_blocks.data(), 0, _blocks.size() * sizeof(Block));
    }

private:
    // Constants for the hash function. Aligned so they can be loaded as a single vector
    alignas(32) static constexpr uint32_t SALT[8] = {0x47b6137bU,
                                                     0x44974d91U,
                                                     0x8824ad5bU,
                                                     0xa2b7289dU,
                                                     0x705495c7U,
                                                     0x2df1424bU,
                                                     0x9efc4947U,
                                                     0x5c6bfb31U};

    // Align struct to 32 bytes (256 bits) to fit 32-byte simd registers and aligned to a Cache Line
    struct alignas(32) Block {
        uint32_t words[8];  // 8 words * 32 bits = 256 bits
    };

    std::vector<Block> _blocks;
    size_t _numBlocks;
};

}  // namespace mongo::sbe
