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

#include <exception>
#include <stdexcept>
#include <string>

#include <boost/exception/exception.hpp>

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/json.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

TEST(Basic, Accessors) {
    Status status(ErrorCodes::MaxError, "error");
    ASSERT_EQUALS(status.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(status.reason(), "error");
}

TEST(Basic, IsA) {
    ASSERT(!Status(ErrorCodes::BadValue, "").isA<ErrorCategory::Interruption>());
    ASSERT(Status(ErrorCodes::Interrupted, "").isA<ErrorCategory::Interruption>());
    ASSERT(!Status(ErrorCodes::Interrupted, "").isA<ErrorCategory::ShutdownError>());
}

TEST(Basic, OKIsAValidStatus) {
    Status status = Status::OK();
    ASSERT_EQUALS(status.code(), ErrorCodes::OK);
}

TEST(Basic, Compare) {
    Status errMax(ErrorCodes::MaxError, "error");
    ASSERT_EQ(errMax, errMax);
    ASSERT_NE(errMax, Status::OK());
}

TEST(Basic, WithReason) {
    const Status orig(ErrorCodes::MaxError, "error");

    const auto copy = orig.withReason("reason");
    ASSERT_EQ(copy.code(), ErrorCodes::MaxError);
    ASSERT_EQ(copy.reason(), "reason");

    ASSERT_EQ(orig.code(), ErrorCodes::MaxError);
    ASSERT_EQ(orig.reason(), "error");
}

TEST(Basic, WithContext) {
    const Status orig(ErrorCodes::MaxError, "error");

    const auto copy = orig.withContext("context");
    ASSERT_EQ(copy.code(), ErrorCodes::MaxError);
    ASSERT(str::startsWith(copy.reason(), "context ")) << copy.reason();
    ASSERT(str::endsWith(copy.reason(), " error")) << copy.reason();

    ASSERT_EQ(orig.code(), ErrorCodes::MaxError);
    ASSERT_EQ(orig.reason(), "error");
}

TEST(Cloning, Copy) {
    Status orig(ErrorCodes::MaxError, "error");
    ASSERT_EQUALS(orig.refCount(), 1U);

    Status dest(orig);
    ASSERT_EQUALS(dest.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(dest.reason(), "error");

    ASSERT_EQUALS(dest.refCount(), 2U);
    ASSERT_EQUALS(orig.refCount(), 2U);
}

TEST(Cloning, MoveCopyOK) {
    Status orig = Status::OK();
    ASSERT_TRUE(orig.isOK());
    ASSERT_EQUALS(orig.refCount(), 0U);

    Status dest(std::move(orig));

    ASSERT_TRUE(orig.isOK());
    ASSERT_EQUALS(orig.refCount(), 0U);

    ASSERT_TRUE(dest.isOK());
    ASSERT_EQUALS(dest.refCount(), 0U);
}

TEST(Cloning, MoveCopyError) {
    Status orig(ErrorCodes::MaxError, "error");
    ASSERT_FALSE(orig.isOK());
    ASSERT_EQUALS(orig.refCount(), 1U);

    Status dest(std::move(orig));

    ASSERT_TRUE(orig.isOK());
    ASSERT_EQUALS(orig.refCount(), 0U);

    ASSERT_FALSE(dest.isOK());
    ASSERT_EQUALS(dest.refCount(), 1U);
    ASSERT_EQUALS(dest.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(dest.reason(), "error");
}

TEST(Cloning, MoveAssignOKToOK) {
    Status orig = Status::OK();
    ASSERT_TRUE(orig.isOK());
    ASSERT_EQUALS(orig.refCount(), 0U);

    Status dest = Status::OK();
    ASSERT_TRUE(dest.isOK());
    ASSERT_EQUALS(dest.refCount(), 0U);

    dest = std::move(orig);

    ASSERT_TRUE(orig.isOK());
    ASSERT_EQUALS(orig.refCount(), 0U);

    ASSERT_TRUE(dest.isOK());
    ASSERT_EQUALS(dest.refCount(), 0U);
}

TEST(Cloning, MoveAssignErrorToError) {
    Status orig = Status(ErrorCodes::MaxError, "error");
    ASSERT_FALSE(orig.isOK());
    ASSERT_EQUALS(orig.refCount(), 1U);
    ASSERT_EQUALS(orig.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(orig.reason(), "error");

    Status dest = Status(ErrorCodes::InternalError, "error2");
    ASSERT_FALSE(dest.isOK());
    ASSERT_EQUALS(dest.refCount(), 1U);
    ASSERT_EQUALS(dest.code(), ErrorCodes::InternalError);
    ASSERT_EQUALS(dest.reason(), "error2");

    dest = std::move(orig);

    ASSERT_TRUE(orig.isOK());
    ASSERT_EQUALS(orig.refCount(), 0U);

    ASSERT_FALSE(dest.isOK());
    ASSERT_EQUALS(dest.refCount(), 1U);
    ASSERT_EQUALS(dest.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(dest.reason(), "error");
}

TEST(Cloning, MoveAssignErrorToOK) {
    Status orig = Status(ErrorCodes::MaxError, "error");
    ASSERT_FALSE(orig.isOK());
    ASSERT_EQUALS(orig.refCount(), 1U);
    ASSERT_EQUALS(orig.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(orig.reason(), "error");

    Status dest = Status::OK();
    ASSERT_TRUE(dest.isOK());
    ASSERT_EQUALS(dest.refCount(), 0U);

    dest = std::move(orig);

    ASSERT_TRUE(orig.isOK());
    ASSERT_EQUALS(orig.refCount(), 0U);

    ASSERT_FALSE(dest.isOK());
    ASSERT_EQUALS(dest.refCount(), 1U);
    ASSERT_EQUALS(dest.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(dest.reason(), "error");
}

TEST(Cloning, MoveAssignOKToError) {
    Status orig = Status::OK();
    ASSERT_TRUE(orig.isOK());
    ASSERT_EQUALS(orig.refCount(), 0U);

    Status dest = Status(ErrorCodes::MaxError, "error");
    ASSERT_FALSE(dest.isOK());
    ASSERT_EQUALS(dest.refCount(), 1U);
    ASSERT_EQUALS(dest.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(dest.reason(), "error");

    orig = std::move(dest);

    ASSERT_FALSE(orig.isOK());
    ASSERT_EQUALS(orig.refCount(), 1U);
    ASSERT_EQUALS(orig.code(), ErrorCodes::MaxError);
    ASSERT_EQUALS(orig.reason(), "error");

    ASSERT_TRUE(dest.isOK());
    ASSERT_EQUALS(dest.refCount(), 0U);
}

TEST(Cloning, OKIsNotRefCounted) {
    ASSERT_EQUALS(Status::OK().refCount(), 0U);

    Status myOk = Status::OK();
    ASSERT_EQUALS(myOk.refCount(), 0U);
    ASSERT_EQUALS(Status::OK().refCount(), 0U);
}

TEST(Parsing, CodeToEnum) {
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, ErrorCodes::Error(int(ErrorCodes::TypeMismatch)));
    ASSERT_EQUALS(ErrorCodes::UnknownError, ErrorCodes::Error(int(ErrorCodes::UnknownError)));
    ASSERT_EQUALS(ErrorCodes::MaxError, ErrorCodes::Error(int(ErrorCodes::MaxError)));
    ASSERT_EQUALS(ErrorCodes::OK, ErrorCodes::duplicateCodeForTest(0));
}

TEST(Transformers, ExceptionToStatus) {
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
}

DEATH_TEST_REGEX(ErrorExtraInfo, InvariantAllRegistered, "Invariant failure.*parsers::") {
    ErrorExtraInfo::invariantHaveAllParsers();
}

#ifdef MONGO_CONFIG_DEBUG_BUILD
DEATH_TEST_REGEX(ErrorExtraInfo, DassertShouldHaveExtraInfo, "Fatal assertion.*40680") {
    Status(ErrorCodes::ForTestingErrorExtraInfo, "");
}
#else
TEST(ErrorExtraInfo, ConvertCodeOnMissingExtraInfo) {
    auto status = Status(ErrorCodes::ForTestingErrorExtraInfo, "");
    ASSERT_EQ(status, ErrorCodes::duplicateCodeForTest(40671));
}
#endif

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
