// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <utility>

namespace mongo::analyze_regex {

/**
 * Analyzes the regular expression given by 'regex' and 'flags' and returns a pair containing the
 * following:
 *  - A (possibly empty) string which must be the prefix of all strings which match the regex. For
 *  example, for /^bar?/ this would be the string "bar".
 *  - A boolean which indicates whether the regex can be converted to simple prefix match. For
 *  instance, /^bar/ means "starts with 'bar'" and would return the pair ("bar", true). On the other
 *  hand, /^bar?/ does not match all strings that start with "bar" and therefore would return
 *  ("bar", false).
 */
std::pair<std::string, bool> getRegexPrefixMatch(const char* regex, const char* flags);

}  // namespace mongo::analyze_regex
