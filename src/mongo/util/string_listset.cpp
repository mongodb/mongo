// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/string_listset.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <sstream>
#include <string_view>

#include <absl/container/flat_hash_map.h>

namespace mongo {
// This method returns an array to be stored in  '_fastHt'. This method also has the side-effect
// of initializing _stringToIndexMap. (This method assumes the default ctor for '_stringToIndexMap'
// has already executed.)
std::array<uint8_t, 128> StringListSet::buildFastHash() {
    _stringToIndexMap.clear();

    // Initialize 'bf' to all zeros.
    std::array<uint8_t, 128> fastHt = {{0}};

    if (useFastHash()) {
        for (size_t idx = 0; idx < _strings.size(); ++idx) {
            auto& p = _strings[idx];
            auto fastHash1 = computeFastHash1(p);
            auto fastHash2 = computeFastHash2(p, fastHash1);
            size_t encodedIdx = idx + 2;

            // Update 'fastHt[hash]' to store 'fieldIdx', or, in the case of a collision, '1'.
            fastHt[fastHash1] = !fastHt[fastHash1] ? uint8_t(encodedIdx) : uint8_t(1);
            fastHt[fastHash2] = !fastHt[fastHash2] ? uint8_t(encodedIdx) : uint8_t(1);
        }
    }

    for (size_t idx = 0; idx < _strings.size(); ++idx) {
        auto& p = _strings[idx];

        if (useFastHash()) {
            auto fastHash1 = computeFastHash1(p);
            auto fastHash2 = computeFastHash2(p, fastHash1);
            size_t encodedIdx = idx + 2;

            // If 'fastHt' can answer all queries about '_strings[idx]', then we don't need to
            // add '_strings[idx]' to _stringToIndexMap.
            if (fastHt[fastHash1] == encodedIdx || fastHt[fastHash2] == encodedIdx) {
                continue;
            }
        }

        auto [_, inserted] = _stringToIndexMap.emplace(std::string_view(p), idx);
        tassert(7582300,
                str::stream()
                    << "Input vector to StringListSet contained multiple occurrences of string: "
                    << p,
                inserted);
    }

    return fastHt;
}

size_t StringListSet::findInMapImpl(std::string_view str) const {
    auto it = _stringToIndexMap.find(str);
    return it != _stringToIndexMap.end() ? it->second : npos;
}

std::string StringListSet::toString() const {
    std::stringstream ss;
    for (size_t i = 0; i < _strings.size(); ++i) {
        ss << (i > 0 ? ", " : "") << _strings[i];
    }

    return ss.str();
}
}  // namespace mongo
