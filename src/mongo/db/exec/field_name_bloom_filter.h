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
