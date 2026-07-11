// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::substr_utils {

// Returns the substring of a string. The substring starts with the character at the specified UTF-8
// code point (CP) index (zero-based) in the string for the number of code points specified.
std::string_view getSubstringCP(std::string_view input, int startingPos, int len);

}  // namespace mongo::substr_utils
