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

#include <tuple>

#include "mongo/unittest/assert.h"
#include "mongo/unittest/matcher.h"
#include "mongo/unittest/matcher_core.h"

/**
 * unittest-style ASSERT that an `expr` successfully matches a `matcher`.
 *
 * Like most other ASSERT macros, can accept further information via a trailing
 * stream `<<` operation.
 *
 * Example (assumed to be enclosed in namespace mongo):
 *
 *    ASSERT_THAT(std::sqrt(4.0), unittest::match::Eq(2.0));
 *
 *    namespace m = unittest::match;
 *    ASSERT_THAT(std::sqrt(4.0), m::Eq(2.0)) << "std::sqrt must be reasonable";
 *
 *    // Combine several matchers on the same value into nice one-liners.
 *    using namespace unittest::match;
 *    ASSERT_THAT(getGreeting(),
 *                AllOf(ContainsRegex("^Hello, "),
 *                      Not(ContainsRegex("bye"))));
 *
 * See https://google.github.io/googletest/reference/matchers.html for inspiration.
 */
#define ASSERT_THAT(expr, matcher)                                                             \
    if (auto args_ = ::mongo::unittest::match::detail::MatchAssertion{expr, matcher, #expr}) { \
    } else                                                                                     \
        FAIL(args_.failMsg())
