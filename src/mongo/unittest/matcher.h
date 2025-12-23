/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

// IWYU pragma: private, include "mongo/unittest/unittest.h"
// IWYU pragma: friend "mongo/unittest/.*"

#include "mongo/base/status.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/modules.h"
#include "mongo/util/pcre.h"

#include <fmt/format.h>

MONGO_MOD_PUBLIC;

/**
 * Defines a basic set of matchers to be used with the ASSERT_THAT macro (see
 * `assert_that.h`). It's intended that matchers to support higher-level
 * components will be defined alongside that component's other unit testing
 * support classes, rather than in this file.
 */
namespace mongo::unittest::match {

using namespace testing;

inline auto Any() {
    return _;
}

MATCHER_P(MatchesPcreRegex,
          pattern,
          fmt::format("{} PCRE pattern: /{}/", negation ? "doesn't match" : "matches", pattern)) {
    pcre::Regex regex(pattern);
    if (!regex) {
        EXPECT_TRUE(regex) << "Invalid regex pattern /" << pattern << "/";
        return false;
    }
    return !!regex.matchView(arg);
}

/**
 * `StatusIs(code, reason)` matches a `Status` against matchers
 * for its code and its reason string.
 *
 * Example:
 *  ASSERT_THAT(status, StatusIs(Eq(ErrorCodes::InternalError), ContainsRegex("ouch")));
 */
MATCHER_P2(StatusIs, code, reason, "") {
    return ExplainMatchResult(
        AllOf(Property("code", &Status::code, code), Property("reason", &Status::reason, reason)),
        arg,
        result_listener);
}

}  // namespace mongo::unittest::match
