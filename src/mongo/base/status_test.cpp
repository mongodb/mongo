/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/status.h"

#include "mongo/bson/json.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <functional>
#include <stdexcept>
#include <string>

#include <boost/exception/exception.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {


static constexpr const char* kReason = "reason";
static const std::string& kReasonString = *new std::string{kReason};

// Check a reason argument specified in various types.
template <typename R>
void checkReason(R&& r, std::string expected = kReasonString) {
    ASSERT_EQUALS(Status(ErrorCodes::MaxError, std::forward<R>(r)).reason(), expected)
        << fmt::format("type {}", demangleName(typeid(decltype(r))));
};

struct CanString {
    operator std::string() const {
        return kReason;
    }
};

struct CanStringExplicit {
    explicit operator std::string() const {
        return kReason;
    }
};

struct CanStringOrStringData {
    operator StringData() const {
        return "bad choice"_sd;
    }
    operator std::string() const {
        return "good choice";
    }
};

struct CanStringRef {
    operator const std::string&() const {
        return kReasonString;
    }
};

template <typename... Args>
constexpr bool usableStatusArgs = std::is_constructible_v<Status, Args...>;

TEST(Status, ReasonStrings) {
    // Try several types of `reason` arguments.
    checkReason(kReason);
    checkReason(kReasonString);
    checkReason(std::string_view{kReason});  // NOLINT
    checkReason(std::string{kReason});
    checkReason(StringData{kReason});
    checkReason(str::stream{} << kReason);
    checkReason(CanStringOrStringData{}, "good choice");
    checkReason(CanStringRef{});

    ASSERT((usableStatusArgs<ErrorCodes::Error, std::string>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, std::string&>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, const std::string&>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, std::string&&>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, StringData>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, StringData&>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, const StringData&>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, StringData&&>));
    ASSERT((!usableStatusArgs<ErrorCodes::Error, boost::optional<std::string>>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, CanString>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, CanStringExplicit>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, CanStringOrStringData>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, CanStringRef>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, std::reference_wrapper<std::string>>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, std::reference_wrapper<const std::string>>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, std::reference_wrapper<const char*>>));
    ASSERT((usableStatusArgs<ErrorCodes::Error, std::reference_wrapper<const char*>>));
}

TEST(Status, Accessors) {
    Status status(ErrorCodes::MaxError, "error");
    ASSERT_EQUALS(status.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(status.reason(), "error");
}

TEST(Status, IsA) {
    ASSERT(!Status(ErrorCodes::BadValue, "").isA<ErrorCategory::NetworkError>());
    ASSERT(Status(ErrorCodes::HostUnreachable, "").isA<ErrorCategory::NetworkError>());
    ASSERT(!Status(ErrorCodes::HostUnreachable, "").isA<ErrorCategory::ShutdownError>());
}

TEST(Status, OKIsAValidStatus) {
    Status status = Status::OK();
    ASSERT_EQUALS(status.code(), ErrorCodes::OK);
}

TEST(Status, Compare) {
    Status errMax(ErrorCodes::MaxError, "error");
    ASSERT_EQ(errMax, errMax);
    ASSERT_NE(errMax, Status::OK());
}

TEST(Status, WithReason) {
    const Status orig(ErrorCodes::MaxError, "error");

    const auto copy = orig.withReason("reason");
    ASSERT_EQ(copy.code(), ErrorCodes::MaxError);
    ASSERT_EQ(copy.reason(), "reason");

    ASSERT_EQ(orig.code(), ErrorCodes::MaxError);
    ASSERT_EQ(orig.reason(), "error");
}

TEST(Status, WithContext) {
    const Status orig(ErrorCodes::MaxError, "error");

    const auto copy = orig.withContext("context");
    ASSERT_EQ(copy.code(), ErrorCodes::MaxError);
    ASSERT_STRING_SEARCH_REGEX(copy.reason(), "^context .* error$");

    ASSERT_EQ(orig.code(), ErrorCodes::MaxError);
    ASSERT_EQ(orig.reason(), "error");
}

TEST(Status, CloningCopy) {
    Status orig(ErrorCodes::MaxError, "error");
    Status dest(orig);
    ASSERT_EQUALS(dest.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(dest.reason(), "error");
}

TEST(Status, CloningMoveConstructOK) {
    Status orig = Status::OK();
    ASSERT_TRUE(orig.isOK());

    Status dest(std::move(orig));

    ASSERT_TRUE(orig.isOK());  // NOLINT(bugprone-use-after-move)
    ASSERT_TRUE(dest.isOK());
}

TEST(Status, CloningMoveConstructError) {
    Status orig(ErrorCodes::MaxError, "error");

    Status dest(std::move(orig));

    ASSERT_TRUE(orig.isOK());  // NOLINT(bugprone-use-after-move)
    ASSERT_FALSE(dest.isOK());
    ASSERT_EQUALS(dest.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(dest.reason(), "error");
}

TEST(Status, CloningMoveAssignOKToOK) {
    Status orig = Status::OK();
    Status dest = Status::OK();

    dest = std::move(orig);

    ASSERT_TRUE(orig.isOK());  // NOLINT(bugprone-use-after-move)
    ASSERT_TRUE(dest.isOK());
}

TEST(Status, CloningMoveAssignErrorToError) {
    Status orig = Status(ErrorCodes::MaxError, "error");
    Status dest = Status(ErrorCodes::InternalError, "error2");

    dest = std::move(orig);

    ASSERT_TRUE(orig.isOK());  // NOLINT(bugprone-use-after-move)
    ASSERT_FALSE(dest.isOK());
    ASSERT_EQUALS(dest.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(dest.reason(), "error");
}

TEST(Status, CloningMoveAssignErrorToOK) {
    Status orig = Status(ErrorCodes::MaxError, "error");
    Status dest = Status::OK();

    dest = std::move(orig);

    ASSERT_TRUE(orig.isOK());  // NOLINT(bugprone-use-after-move)
    ASSERT_FALSE(dest.isOK());
    ASSERT_EQUALS(dest.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(dest.reason(), "error");
}

TEST(Status, CloningMoveAssignOKToError) {
    Status orig = Status::OK();
    Status dest = Status(ErrorCodes::MaxError, "error");

    orig = std::move(dest);

    ASSERT_FALSE(orig.isOK());
    ASSERT_EQUALS(orig.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(orig.reason(), "error");

    ASSERT_TRUE(dest.isOK());  // NOLINT(bugprone-use-after-move)
}

TEST(Status, ParsingCodeToEnum) {
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, ErrorCodes::Error(int(ErrorCodes::TypeMismatch)));
    ASSERT_EQUALS(ErrorCodes::UnknownError, ErrorCodes::Error(int(ErrorCodes::UnknownError)));
    ASSERT_EQUALS(ErrorCodes::MaxError, ErrorCodes::Error(int(ErrorCodes::MaxError)));
    ASSERT_EQUALS(ErrorCodes::OK, ErrorCodes::duplicateCodeForTest(0));
}

TEST(Status, ExceptionToStatus) {
    using mongo::DBException;
    using mongo::exceptionToStatus;

    auto reason = "oh no";

    Status fromDBExcept = [=]() {
        try {
            uasserted(ErrorCodes::TypeMismatch, reason);
        } catch (...) {
            return exceptionToStatus();
        }
    }();

    ASSERT_NOT_OK(fromDBExcept);
    ASSERT_EQUALS(fromDBExcept.reason(), reason);
    ASSERT_EQUALS(fromDBExcept.code(), ErrorCodes::TypeMismatch);

    Status fromStdExcept = [=]() {
        try {
            throw std::out_of_range(reason);
        } catch (...) {
            return exceptionToStatus();
        }
    }();

    ASSERT_NOT_OK(fromStdExcept);
    // we don't check the exact error message because the type name of the exception
    // isn't demangled on windows.
    ASSERT_TRUE(fromStdExcept.reason().find(reason) != std::string::npos);
    ASSERT_EQUALS(fromStdExcept.code(), ErrorCodes::UnknownError);

    class bar : public boost::exception {};

    Status fromBoostExcept = [=]() {
        try {
            throw bar();
        } catch (...) {
            return exceptionToStatus();
        }
    }();

    ASSERT_NOT_OK(fromBoostExcept);
    ASSERT_EQUALS(fromBoostExcept, ErrorCodes::UnknownError);
    // Reason should include that it was a boost::exception
    ASSERT_TRUE(fromBoostExcept.reason().find("boost::exception") != std::string::npos);

    Status fromUnknownExceptionType = [=]() {
        try {
            throw 7;
        } catch (...) {
            return exceptionToStatus();
        }
    }();

    ASSERT_NOT_OK(fromUnknownExceptionType);
    ASSERT_EQUALS(fromUnknownExceptionType, ErrorCodes::UnknownError);
    // Reason should include that it was an unknown exception
    ASSERT_TRUE(fromUnknownExceptionType.reason().find("unknown type") != std::string::npos);
}

DEATH_TEST_REGEX(ErrorExtraInfo, InvariantAllRegistered, "Invariant failure.*parsers::") {
    ErrorExtraInfo::invariantHaveAllParsers();
}

#ifdef MONGO_CONFIG_DEBUG_BUILD
DEATH_TEST_REGEX(ErrorExtraInfo, DassertShouldHaveExtraInfo, "Fatal assertion.*40680") {
    (void)Status(ErrorCodes::ForTestingErrorExtraInfo, "");
}
#else
TEST(ErrorExtraInfo, ConvertCodeOnMissingExtraInfo) {
    auto status = Status(ErrorCodes::ForTestingErrorExtraInfo, "");
    ASSERT_EQ(status, ErrorCodes::duplicateCodeForTest(40671));
}
#endif

TEST(ErrorExtraInfo, StatusCtorExtraAndReason) {
    using Extra = OptionalErrorExtraInfoExample;
    // Check another ctor
    ASSERT((usableStatusArgs<Extra, std::string>));
    ASSERT((usableStatusArgs<Extra, std::string&>));
    ASSERT((usableStatusArgs<Extra, const std::string&>));
    ASSERT((usableStatusArgs<Extra, std::string&&>));
    ASSERT((usableStatusArgs<Extra, StringData>));
    ASSERT((usableStatusArgs<Extra, StringData&>));
    ASSERT((usableStatusArgs<Extra, const StringData&>));
    ASSERT((usableStatusArgs<Extra, StringData&&>));
    ASSERT((!usableStatusArgs<Extra, boost::optional<std::string>>));
    ASSERT((usableStatusArgs<Extra, CanString>));
    ASSERT((usableStatusArgs<Extra, CanStringExplicit>));
    ASSERT((usableStatusArgs<Extra, CanStringOrStringData>));
    ASSERT((usableStatusArgs<Extra, CanStringRef>));
    ASSERT((usableStatusArgs<Extra, std::reference_wrapper<std::string>>));
    ASSERT((usableStatusArgs<Extra, std::reference_wrapper<const std::string>>));
    ASSERT((usableStatusArgs<Extra, std::reference_wrapper<const char*>>));
    ASSERT((usableStatusArgs<Extra, std::reference_wrapper<const char*>>));
}


TEST(ErrorExtraInfo, OptionalExtraInfoDoesNotThrowAndReturnsOriginalError) {
    const auto status = Status(ErrorCodes::ForTestingOptionalErrorExtraInfo, "");
    ASSERT_EQ(status, ErrorCodes::ForTestingOptionalErrorExtraInfo);
    // The ErrorExtraInfo pointer should be nullptr.
    ASSERT(!status.extraInfo());
}

TEST(ErrorExtraInfo, OptionalExtraInfoStatusParserThrows) {
    OptionalErrorExtraInfoExample::EnableParserForTest whenInScope;
    bool failed = false;

    auto pars = ErrorExtraInfo::parserFor(ErrorCodes::ForTestingOptionalErrorExtraInfo);
    try {
        pars(fromjson("{a: 1}"));
    } catch (const DBException&) {
        failed = true;
    }

    ASSERT(failed);
}

TEST(ErrorExtraInfo, OptionalExtraInfoStatusParserWorks) {
    OptionalErrorExtraInfoExample::EnableParserForTest whenInScope;
    const auto status =
        Status(ErrorCodes::ForTestingOptionalErrorExtraInfo, "", fromjson("{data: 123}"));
    ASSERT_EQ(status, ErrorCodes::ForTestingOptionalErrorExtraInfo);
    ASSERT(status.extraInfo());
    ASSERT(status.extraInfo<OptionalErrorExtraInfoExample>());
    ASSERT_EQ(status.extraInfo<OptionalErrorExtraInfoExample>()->data, 123);
}

TEST(ErrorExtraInfo, MissingOptionalExtraInfoStatus) {
    OptionalErrorExtraInfoExample::EnableParserForTest whenInScope;
    const auto status = Status(ErrorCodes::ForTestingOptionalErrorExtraInfo, "");
    ASSERT_EQ(status, ErrorCodes::ForTestingOptionalErrorExtraInfo);
    ASSERT_FALSE(status.extraInfo());
    ASSERT_FALSE(status.extraInfo<OptionalErrorExtraInfoExample>());
}

TEST(ErrorExtraInfo, TypedConstructorWorks) {
    const auto status = Status(ErrorExtraInfoExample(123), "");
    ASSERT_EQ(status, ErrorCodes::ForTestingErrorExtraInfo);
    ASSERT(status.extraInfo());
    ASSERT(status.extraInfo<ErrorExtraInfoExample>());
    ASSERT_EQ(status.extraInfo<ErrorExtraInfoExample>()->data, 123);
}

TEST(ErrorExtraInfo, StatusWhenParserThrows) {
    auto status = Status(ErrorCodes::ForTestingErrorExtraInfo, "", fromjson("{data: 123}"));
    ASSERT_EQ(status, ErrorCodes::duplicateCodeForTest(40681));
    ASSERT(!status.extraInfo());
    ASSERT(!status.extraInfo<ErrorExtraInfoExample>());
}

TEST(ErrorExtraInfo, StatusParserWorks) {
    ErrorExtraInfoExample::EnableParserForTest whenInScope;
    auto status = Status(ErrorCodes::ForTestingErrorExtraInfo, "", fromjson("{data: 123}"));
    ASSERT_EQ(status, ErrorCodes::ForTestingErrorExtraInfo);
    ASSERT(status.extraInfo());
    ASSERT(status.extraInfo<ErrorExtraInfoExample>());
    ASSERT_EQ(status.extraInfo<ErrorExtraInfoExample>()->data, 123);
}

TEST(ErrorExtraInfo, StatusParserWorksNested) {
    nested::twice::NestedErrorExtraInfoExample::EnableParserForTest whenInScope;
    auto status = Status(
        ErrorCodes::ForTestingErrorExtraInfoWithExtraInfoInNamespace, "", fromjson("{data: 123}"));
    ASSERT_EQ(status, ErrorCodes::ForTestingErrorExtraInfoWithExtraInfoInNamespace);
    ASSERT(status.extraInfo());
    ASSERT(status.extraInfo<nested::twice::NestedErrorExtraInfoExample>());
    ASSERT_EQ(status.extraInfo<nested::twice::NestedErrorExtraInfoExample>()->data, 123);
}

TEST(ErrorExtraInfo, StatusWhenParserThrowsNested) {
    auto status = Status(
        ErrorCodes::ForTestingErrorExtraInfoWithExtraInfoInNamespace, "", fromjson("{data: 123}"));
    ASSERT_EQ(status, ErrorCodes::duplicateCodeForTest(51100));
    ASSERT(!status.extraInfo());
    ASSERT(!status.extraInfo<nested::twice::NestedErrorExtraInfoExample>());
}

}  // namespace
}  // namespace mongo
