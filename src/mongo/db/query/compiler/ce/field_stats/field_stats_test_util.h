// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/ce/field_stats/field_stats.h"
#include "mongo/db/query/compiler/stats/value_utils.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

/**
 * Helpers shared by the unit tests that build SingleFieldStats fixtures.
 */
namespace mongo::ce {

inline stats::SBEValue intValue(int32_t v) {
    return stats::SBEValue{stats::makeInt32Value(v)};
}

inline McvMap mcvMap(std::vector<std::pair<stats::SBEValue, McvEntryStats>> entries) {
    McvMap map;
    for (auto& [value, mcvStats] : entries) {
        map.emplace(std::move(value), mcvStats);
    }
    return map;
}

/**
 * Returns an MCV list of 'numEntries' exact entries for the values 0 .. numEntries - 1, each with
 * frequency 'estFreq'.
 */
inline McvMap mcvWithIntKeys(int32_t numEntries, size_t estFreq) {
    std::vector<std::pair<stats::SBEValue, McvEntryStats>> entries;
    for (int32_t i = 0; i < numEntries; ++i) {
        entries.emplace_back(intValue(i), McvEntryStats{estFreq, 0});
    }
    return mcvMap(std::move(entries));
}

}  // namespace mongo::ce
