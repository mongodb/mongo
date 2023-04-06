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

#include "mongo/util/indexed_string_vector.h"

#include "mongo/util/str.h"

namespace mongo {
// This method returns an array to be stored in  '_fastHt'. This method also has the side-effect
// of initializing _stringToIndexMap. (This method assumes the default ctor for '_stringToIndexMap'
// has already executed.)
std::array<uint8_t, 128> IndexedStringVector::buildFastHash() {
    _stringToIndexMap.clear();

    // Initialize 'bf' to all zeros.
    std::array<uint8_t, 128> fastHt = {{0}};

    if (useFastHash()) {
        for (size_t idx = 0; idx < _strings.size(); ++idx) {
            auto& p = _strings[idx];
            auto fastHash1 = computeFastHash1(p.data(), p.size());
            auto fastHash2 = computeFastHash2(p.data(), p.size(), fastHash1);
            size_t encodedIdx = idx + 2;

            // Update 'fastHt[hash]' to store 'fieldIdx', or, in the case of a collision, '1'.
            fastHt[fastHash1] = !fastHt[fastHash1] ? uint8_t(encodedIdx) : uint8_t(1);
            fastHt[fastHash2] = !fastHt[fastHash2] ? uint8_t(encodedIdx) : uint8_t(1);
        }
    }

    for (size_t idx = 0; idx < _strings.size(); ++idx) {
        auto& p = _strings[idx];

        if (useFastHash()) {
            auto fastHash1 = computeFastHash1(p.data(), p.size());
            auto fastHash2 = computeFastHash2(p.data(), p.size(), fastHash1);
            size_t encodedIdx = idx + 2;

            // If 'fastHt' can answer all queries about '_strings[idx]', then we don't need to
            // add '_strings[idx]' to _stringToIndexMap.
            if (fastHt[fastHash1] == encodedIdx || fastHt[fastHash2] == encodedIdx) {
                continue;
            }
        }

        auto [_, inserted] = _stringToIndexMap.emplace(StringData(p), idx);
        tassert(
            7582300,
            str::stream()
                << "Input vector to IndexedStringVector contained multiple occurrences of string: "
                << p,
            inserted);
    }

    return fastHt;
}

size_t IndexedStringVector::findInMapImpl(StringData str) const {
    auto it = _stringToIndexMap.find(str);
    return it != _stringToIndexMap.end() ? it->second : npos;
}
}  // namespace mongo
