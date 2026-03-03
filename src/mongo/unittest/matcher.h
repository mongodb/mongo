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
#include "mongo/util/active_exception_witness.h"
#include "mongo/util/assert_util.h"
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

using namespace ::testing;
using ::testing::Matcher;

namespace MONGO_MOD_FILE_PRIVATE match_details {
inline std::string maybeNegate(bool negation, std::string str) {
    if (negation)
        return fmt::format("not ({})", str);
    return str;
}

/**
 * Implementation of the `Throws` matcher.
 *
 * Verifies a callable throws a specific exception type. Provides diagnostic output with the
 * demangled type name and the exception information via the ActiveExceptionWitness.
 *
 * Expects a callable (function, lambda, etc.) with no parameters. If testing a function
 * with parameters, wrap the call in a lambda.
 */
template <typename Exception>
class ThrowsMatcherImpl {
public:
    explicit ThrowsMatcherImpl(Matcher<const Exception&> matcher) : _matcher(std::move(matcher)) {}

    void DescribeTo(std::ostream* os) const {
        *os << fmt::format("throws a {} which ", demangleName(typeid(Exception)));
        _matcher.DescribeTo(os);
    }

    void DescribeNegationTo(std::ostream* os) const {
        *os << fmt::format("throws an exception which is not a {} which ",
                           demangleName(typeid(Exception)));
        _matcher.DescribeNegationTo(os);
    }

    template <typename T>
    bool MatchAndExplain(T&& x, MatchResultListener* listener) const {
        auto describeThrow = [&] {
            *listener << "throws an exception";
            if (const auto& info = activeExceptionInfo()) {
                *listener << " of type " + demangleName(*info->type);
                *listener << " with value " << info->description;
            }
        };

        try {
            (void)std::forward<T>(x)();
        } catch (const Exception& ex) {
            describeThrow();
            *listener << ", which is a " << demangleName(typeid(Exception)) << " ";
            return _matcher.MatchAndExplain(ex, listener);
        } catch (...) {
            describeThrow();
            return false;
        }

        *listener << "does not throw";
        return false;
    }

    Matcher<const Exception&> _matcher;
};
}  // namespace MONGO_MOD_FILE_PRIVATE match_details

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
MATCHER_P2(StatusIs,
           code,
           reason,
           match_details::maybeNegate(negation,
                                      fmt::format("has code which {} and reason which {}",
                                                  testing::DescribeMatcher<ErrorCodes::Error>(code),
                                                  testing::DescribeMatcher<std::string>(reason)))) {
    return ExplainMatchResult(
        AllOf(Property("code", &Status::code, code), Property("reason", &Status::reason, reason)),
        arg,
        result_listener);
}

/**
 * `StatusIsOK()` matches if a `Status` is OK.
 *
 * Example:
 *  ASSERT_THAT(status, StatusIsOK());
 */
MATCHER(StatusIsOK, negation ? "is not OK" : "is OK") {
    return arg.isOK();
}

/**
 * `StatusWithHasValue(value)` matches a `StatusWith` that is OK and whose
 * value matches the given matcher.
 *
 * Example:
 *  StatusWith<int> sw16{16};
 *  StatusWith<int> swError{Status{ErrorCodes::CommandFailed, "error"}};
 *  ASSERT_THAT(sw16, StatusWithHasValue(16));
 *  ASSERT_THAT(swError, Not(StatusWithHasValue(16)));
 */
MATCHER_P(
    StatusWithHasValue,
    value,
    match_details::maybeNegate(
        negation,
        fmt::format(
            "has an OK status with value which {}",
            testing::DescribeMatcher<typename std::remove_cvref_t<arg_type>::value_type>(value)))) {
    if (!arg.isOK())
        return false;

    return ExplainMatchResult(value, arg.getValue(), result_listener);
}


/**
 * `StatusWithHasStatus(status)` matches a `StatusWith` whose status matches
 * the given matcher.
 *
 * Example:
 *  StatusWith<int> sw13{13};
 *  StatusWith<int> swError{Status{ErrorCodes::CommandFailed, "error"}};
 *  ASSERT_THAT(sw13, StatusWithHasStatus(StatusIsOK()));
 *  ASSERT_THAT(swError, StatusWithHasStatus(StatusIs(ErrorCodes::CommandFailed, _)));
 */
MATCHER_P(StatusWithHasStatus,
          status,
          fmt::format("{} a StatusWith whose status {}",
                      negation ? "isn't" : "is",
                      testing::DescribeMatcher<Status>(status))) {
    return ExplainMatchResult(status, arg.getStatus(), result_listener);
}

/**
 * `Throws<E>(m)`:  The `argument` is a callable object that, when called,
 * throws an exception of type `E` that satisfies the matcher `m`.
 *
 * Uses the `ActiveExceptionWitness` to provide better diagnostic message than `testing::Throws`.
 *
 * Example:
 *   auto func = [] { throw std::runtime_error("error msg"); };
 *   ASSERT_THAT(func, Throws<std::runtime_error>(Property(&std::exception::what, "error msg")));
 */
template <typename Exception, typename Matcher>
inline auto Throws(const Matcher& exceptionMatcher) {
    return MakePolymorphicMatcher(match_details::ThrowsMatcherImpl<Exception>{
        SafeMatcherCast<const Exception&>(exceptionMatcher)});
}

/**
 * `Throws<E>()` : The `argument` is a callable object that, when called, throws
 * an exception of the expected type `E`.
 *
 * Equivalent to Throws<E>(A<E>())
 *
 * Example:
 *   auto func = [] { throw std::runtime_error("error msg"); };
 *   ASSERT_THAT(func, Throws<std::runtime_error>());
 */
template <typename Exception>
inline auto Throws() {
    return match::Throws<Exception>(A<Exception>());
}
}  // namespace mongo::unittest::match
