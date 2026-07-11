// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>

namespace mongo {
typedef uint64_t (*FieldNameBloomFilterHasher)(const char*, size_t);
inline uint64_t fieldNameBloomFilterDefaultHash(const char* str, size_t len) {
    const auto shift = static_cast<unsigned char>(str[len / 2]) & 63u;
    return uint64_t{1} << shift;
}

/*
 * Tiny bloom filter intended to be used when scanning a list of [key, value]
 * pairs (i.e. bson).
 */
template <FieldNameBloomFilterHasher HashFn = fieldNameBloomFilterDefaultHash>
class FieldNameBloomFilter {
public:
    /**
     * Inserts a string to the bloom filter.
     */
    void insert(const char* str, size_t len) {
        _data = _data | HashFn(str, len);
    }

    /**
     * If false is returned, the given string was definitely not inserted.
     * If true is returned, the given string may have been inserted.
     */
    bool maybeContains(const char* str, size_t len) const {
        return static_cast<bool>(_data & HashFn(str, len));
    }

    /**
     * Similar to maybeContains() but the caller can compute a hash themselves and check
     * whether anything with that hash was maybe-inserted.
     */
    bool maybeContainsHash(uint64_t hash) {
        return _data & hash;
    }

private:
    uint64_t _data = 0;
};
}  // namespace mongo
