// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <string_view>

namespace mongo {

/**
 * Validates that a string is a valid ObjectId string (24 hex characters).
 *
 * This function was extracted from Scope::validateObjectIdString to decouple
 * ObjectId validation from the Scope class and engine.h dependencies.
 */
inline void validateObjectIdString(std::string_view str) {
    uassert(10448, "invalid object id: length", str.size() == 24);
    auto isAllHex = [](std::string_view s) {
        return std::all_of(s.begin(), s.end(), [](char c) { return mongo::ctype::isXdigit(c); });
    };
    uassert(10430, "invalid object id: not hex", isAllHex(str));
}

}  // namespace mongo
