// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

// IWYU pragma: private, include "mongo/unittest/unittest.h"
// IWYU pragma: friend "mongo/unittest/.*"

#include "mongo/base/status.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/active_exception_witness.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/pcre.h"

#include <string_view>

#include <fmt/format.h>

[[MONGO_MOD_PUBLIC]];

/**
 * Defines a basic set of matchers to be used with the ASSERT_THAT macro (see
 * `assert_that.h`). It's intended that matchers to support higher-level
 * components will be defined alongside that component's other unit testing
 * support classes, rather than in this file.
 */
namespace mongo::unittest::match {

using namespace ::testing;
using ::testing::Matcher;

namespace [[MONGO_MOD_FILE_PRIVATE]] match_details {
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
}  // namespace match_details

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
 * `AsStringView(value)` matches a string view constructed from value against `matcher`.
 *
 * Useful when an api or a property is of type `char const*` and a string-like object is required
 * for the matcher.
 *
 * Example:
 *  std::string s = "string";
 *  ASSERT_THAT("string literal", AsStringView(StartsWith(s)));
 */
MATCHER_P(AsStringView,
          matcher,
          fmt::format("as string view {}",
                      testing::DescribeMatcher<std::string_view>(matcher, negation))) {
    return ExplainMatchResult(matcher, std::string_view(arg), result_listener);
}

/**
 * `CodeIs(matcher)` matches the result of code() against `matcher`.
 *
 * Example:
 *  AssertionException& ex = ...;
 *  ASSERT_THAT(ex, CodeIs(ErrorCodes::InternalError));
 */
MATCHER_P(
    CodeIs,
    matcher,
    fmt::format("{} an object whose code() result {}",
                negation ? "isn't" : "is",
                testing::DescribeMatcher<decltype(std::declval<arg_type>().code())>(matcher))) {
    return ExplainMatchResult(matcher, arg.code(), result_listener);
}


/**
 * `WhatIs(matcher)` matches the result of what() against `matcher`.
 *
 * Example:
 *  AssertionException& ex = ...;
 *  ASSERT_THAT(ex, WhatIs(StrEq("exception message")));
 */
MATCHER_P(
    WhatIs,
    matcher,
    fmt::format("{} an object whose what() result {}",
                negation ? "isn't" : "is",
                testing::DescribeMatcher<decltype(std::declval<arg_type>().what())>(matcher))) {
    return ExplainMatchResult(matcher, arg.what(), result_listener);
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
