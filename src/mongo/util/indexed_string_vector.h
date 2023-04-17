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
#include <limits>
#include <string>
#include <vector>

#include "mongo/util/string_map.h"

namespace mongo {
class IndexedStringVector {
public:
    static constexpr size_t npos = std::numeric_limits<size_t>::max();

private:
    inline static size_t getLowestNBits(size_t val, size_t n) {
        return val & ((1u << n) - 1u);
    }

    // This function assumes that 'length' is not 0.
    inline static size_t computeFastHash1(const char* str, size_t len) {
        // The lowest 5 bits of 'str[len - 1]' and the lowest 2 bits of 'len' are decent sources
        // of entropy. Combine them to generate a pseudo-random number 'h' where 0 <= h <= 127.
        size_t h = getLowestNBits(size_t(str[len - 1]) + (len << 5u), 7);
        return h;
    }

    // This function assumes that 'length' is not 0.
    inline static size_t computeFastHash2(const char* str, size_t len, size_t fastHash1) {
        // The lowest 5 bits of 'str[0]' are a decent source of entropy. Using 'str[0]' and
        // 'fastHash1', generate a pseudo-random number 'h' where 0 <= h <= 127 and where
        // h != fastHash1.
        size_t h = getLowestNBits(
            fastHash1 + size_t(str[0]) + getLowestNBits(~size_t(str[0]) >> 4u, 1), 7);
        return h;
    }

public:
    explicit IndexedStringVector(std::vector<std::string> strings)
        : _strings(std::move(strings)), _stringToIndexMap(), _fastHt(buildFastHash()) {}

    IndexedStringVector(const IndexedStringVector& other)
        : _strings(other._strings), _stringToIndexMap(), _fastHt(buildFastHash()) {}

    IndexedStringVector(IndexedStringVector&& other)
        : _strings(std::move(other._strings)), _stringToIndexMap(), _fastHt(buildFastHash()) {}

    IndexedStringVector& operator=(const IndexedStringVector& other) {
        if (&other != this) {
            _strings = other._strings;
            _fastHt = buildFastHash();
        }
        return *this;
    }

    IndexedStringVector& operator=(IndexedStringVector&& other) {
        if (&other != this) {
            _strings = std::move(other._strings);
            _fastHt = buildFastHash();
        }
        return *this;
    }

    inline size_t size() const {
        return _strings.size();
    }

    inline std::vector<std::string>::const_iterator begin() const {
        return _strings.cbegin();
    }
    inline std::vector<std::string>::const_iterator cbegin() const {
        return _strings.cbegin();
    }
    inline std::vector<std::string>::const_iterator end() const {
        return _strings.cend();
    }
    inline std::vector<std::string>::const_iterator cend() const {
        return _strings.cend();
    }

    inline const std::string& operator[](size_t idx) const {
        return _strings[idx];
    }
    inline const std::string& at(size_t idx) const {
        return _strings.at(idx);
    }

    inline size_t findPos(StringData str) const {
        size_t len = str.size();
        size_t fastHash = computeFastHash1(str.rawData(), len);
        size_t encodedIdx = 0;

        if (useFastHash()) {
            size_t numAttempts = 0;

            // This 'for' loop will iterate at most two times. There is logic near the end of
            // the loop body that breaks out of the loop once 'numAttempts' reaches 2.
            for (;;) {
                encodedIdx = _fastHt[fastHash];

                if (encodedIdx > 1) {
                    // If encodedIdx >= 2, then there is a single string in _strings to compare
                    // with. Compare 'str' with this single string.
                    if (len != _strings[encodedIdx - 2].size() ||
                        memcmp(str.rawData(), _strings[encodedIdx - 2].data(), len) != 0) {
                        return npos;
                    }

                    return encodedIdx - 2;
                } else if (MONGO_likely(encodedIdx < 1)) {
                    // If encodedIdx == 0, then we know 'str' is not present in _strings.
                    return npos;
                }

                ++numAttempts;

                if (numAttempts >= 2) {
                    // If this was our second attempt, we give up and search for 'str' in
                    // 'stringToIndexMap'. We break out of the loop early to avoid calling
                    // computeFastHash2() again.
                    break;
                }

                // If this was our first attempt, try again using computeFastHash2().
                fastHash = computeFastHash2(str.rawData(), len, fastHash);
            }
        }

        return findInMapImpl(str);
    }

    inline std::vector<std::string>::const_iterator find(StringData str) const {
        auto pos = findPos(str);
        return pos != npos ? _strings.cbegin() + pos : _strings.cend();
    }

    const auto& getUnderlyingVector() const {
        return _strings;
    }

    const auto& getUnderlyingMap() const {
        return _stringToIndexMap;
    }

private:
    bool useFastHash() const {
        // If there are more than 64 strings in '_strings', don't bother with '_fastHt'.
        return _strings.size() <= 64;
    }

    std::array<uint8_t, 128> buildFastHash();

    size_t findInMapImpl(StringData str) const;

    std::vector<std::string> _strings;
    StringDataMap<size_t> _stringToIndexMap;
    std::array<uint8_t, 128> _fastHt;
};
}  // namespace mongo
