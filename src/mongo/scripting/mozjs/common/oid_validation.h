// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {
namespace mozjs {

/**
 * Validates that a string is a valid ObjectId string (24 hex characters).
 * Extracted from Scope::validateObjectIdString for WASI builds.
 */
inline void validateObjectIdString(std::string_view str) {
    uassert(11542200, "invalid object id: length", str.size() == 24);
    auto isAllHex = [](std::string_view s) {
        return std::all_of(s.begin(), s.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        });
    };
    uassert(11542201, "invalid object id: not hex", isAllHex(str));
}

}  // namespace mozjs
}  // namespace mongo
