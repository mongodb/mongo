/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
