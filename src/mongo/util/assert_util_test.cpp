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


#include "mongo/util/assert_util.h"

#include "mongo/base/static_assert.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/str.h"

#include <ostream>
#include <type_traits>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest
namespace mongo {
namespace {
struct PrinterMockTassert {
    auto format(auto& fc) const {
        tasserted(9513401, "tasserting in mock printer");
        return fc.out();
    }
};

struct PrinterMockUassert {
    auto format(auto& fc) const {
        uasserted(9513402, "uasserting in mock printer");
        return fc.out();
    }
};

void someRiskyBusiness() {
    invariant(false, "ouch");
}
struct PrinterMockInvariant {
    auto format(auto& fc) const {
        someRiskyBusiness();
        return fc.out();
    }
};

struct MemberCallFormatter {
    constexpr auto parse(auto& ctx) {
        return ctx.begin();
    }
    auto format(const auto& obj, auto& ctx) const {
        return obj.format(ctx);
    }
};
}  // namespace
}  // namespace mongo
namespace fmt {
template <>
struct formatter<mongo::PrinterMockTassert> : mongo::MemberCallFormatter {};

template <>
struct formatter<mongo::PrinterMockUassert> : mongo::MemberCallFormatter {};

template <>
struct formatter<mongo::PrinterMockInvariant> : mongo::MemberCallFormatter {};
}  // namespace fmt

namespace mongo {
namespace {
#define ASSERT_CATCHES(code, Type)                                         \
    ([] {                                                                  \
        try {                                                              \
            uasserted(code, "");                                           \
        } catch (const Type&) {                                            \
            /* Success - ignore*/                                          \
        } catch (const std::exception& ex) {                               \
            FAIL("Expected to be able to catch " #code " as a " #Type)     \
                << " actual exception type: " << demangleName(typeid(ex)); \
        }                                                                  \
    }())

#define ASSERT_NOT_CATCHES(code, Type)            \
    ([] {                                         \
        try {                                     \
            uasserted(code, "");                  \
        } catch (const Type&) {                   \
            FAIL("Caught " #code " as a " #Type); \
        } catch (const DBException&) {            \
            /* Success - ignore*/                 \
        }                                         \
    }())

// BadValue - no categories
MONGO_STATIC_ASSERT(std::is_same<error_details::ErrorCategoriesFor<ErrorCodes::BadValue>,
                                 error_details::CategoryList<>>());
MONGO_STATIC_ASSERT(std::is_base_of<AssertionException, ExceptionFor<ErrorCodes::BadValue>>());
MONGO_STATIC_ASSERT(!std::is_base_of<ExceptionFor<ErrorCategory::NetworkError>,
                                     ExceptionFor<ErrorCodes::BadValue>>());
MONGO_STATIC_ASSERT(!std::is_base_of<ExceptionFor<ErrorCategory::NotPrimaryError>,
                                     ExceptionFor<ErrorCodes::BadValue>>());

TEST(AssertUtils, UassertNamedCodeWithoutCategories) {
    ASSERT_CATCHES(ErrorCodes::BadValue, DBException);
    ASSERT_CATCHES(ErrorCodes::BadValue, AssertionException);
    ASSERT_CATCHES(ErrorCodes::BadValue, ExceptionFor<ErrorCodes::BadValue>);
    ASSERT_NOT_CATCHES(ErrorCodes::BadValue, ExceptionFor<ErrorCodes::DuplicateKey>);
    ASSERT_NOT_CATCHES(ErrorCodes::BadValue, ExceptionFor<ErrorCategory::NetworkError>);
    ASSERT_NOT_CATCHES(ErrorCodes::BadValue, ExceptionFor<ErrorCategory::NotPrimaryError>);
}

// NotWritablePrimary - NotPrimaryError, RetriableError
MONGO_STATIC_ASSERT(std::is_same<error_details::ErrorCategoriesFor<ErrorCodes::NotWritablePrimary>,
                                 error_details::CategoryList<ErrorCategory::NotPrimaryError,
                                                             ErrorCategory::RetriableError>>());
MONGO_STATIC_ASSERT(
    std::is_base_of<AssertionException, ExceptionFor<ErrorCodes::NotWritablePrimary>>());
MONGO_STATIC_ASSERT(!std::is_base_of<ExceptionFor<ErrorCategory::NetworkError>,
                                     ExceptionFor<ErrorCodes::NotWritablePrimary>>());
MONGO_STATIC_ASSERT(std::is_base_of<ExceptionFor<ErrorCategory::NotPrimaryError>,
                                    ExceptionFor<ErrorCodes::NotWritablePrimary>>());

TEST(AssertUtils, UassertNamedCodeWithOneCategory) {
    ASSERT_CATCHES(ErrorCodes::NotWritablePrimary, DBException);
    ASSERT_CATCHES(ErrorCodes::NotWritablePrimary, AssertionException);
    ASSERT_CATCHES(ErrorCodes::NotWritablePrimary, ExceptionFor<ErrorCodes::NotWritablePrimary>);
    ASSERT_NOT_CATCHES(ErrorCodes::NotWritablePrimary, ExceptionFor<ErrorCodes::DuplicateKey>);
    ASSERT_NOT_CATCHES(ErrorCodes::NotWritablePrimary, ExceptionFor<ErrorCategory::NetworkError>);
    ASSERT_CATCHES(ErrorCodes::NotWritablePrimary, ExceptionFor<ErrorCategory::NotPrimaryError>);
}

// InterruptedDueToReplStateChange - NotPrimaryError, Interruption, RetriableError
// TODO(SERVER-56251): revise list when removing the Interruption category.
MONGO_STATIC_ASSERT(
    std::is_same<error_details::ErrorCategoriesFor<ErrorCodes::InterruptedDueToReplStateChange>,
                 error_details::CategoryList<ErrorCategory::Interruption,
                                             ErrorCategory::NotPrimaryError,
                                             ErrorCategory::RetriableError>>());
MONGO_STATIC_ASSERT(std::is_base_of<AssertionException,
                                    ExceptionFor<ErrorCodes::InterruptedDueToReplStateChange>>());
MONGO_STATIC_ASSERT(!std::is_base_of<ExceptionFor<ErrorCategory::NetworkError>,
                                     ExceptionFor<ErrorCodes::InterruptedDueToReplStateChange>>());
MONGO_STATIC_ASSERT(std::is_base_of<ExceptionFor<ErrorCategory::NotPrimaryError>,
                                    ExceptionFor<ErrorCodes::InterruptedDueToReplStateChange>>());

TEST(AssertUtils, UassertNamedCodeWithTwoCategories) {
    ASSERT_CATCHES(ErrorCodes::InterruptedDueToReplStateChange, DBException);
    ASSERT_CATCHES(ErrorCodes::InterruptedDueToReplStateChange, AssertionException);
    ASSERT_CATCHES(ErrorCodes::InterruptedDueToReplStateChange,
                   ExceptionFor<ErrorCodes::InterruptedDueToReplStateChange>);
    ASSERT_NOT_CATCHES(ErrorCodes::InterruptedDueToReplStateChange,
                       ExceptionFor<ErrorCodes::DuplicateKey>);
    ASSERT_NOT_CATCHES(ErrorCodes::InterruptedDueToReplStateChange,
                       ExceptionFor<ErrorCategory::NetworkError>);
    ASSERT_CATCHES(ErrorCodes::InterruptedDueToReplStateChange,
                   ExceptionFor<ErrorCategory::NotPrimaryError>);
}

MONGO_STATIC_ASSERT(!error_details::isNamedCode<19999>);
// ExceptionFor<ErrorCodes::Error19999)> invalidType;  // Must not compile.

TEST(AssertUtils, UassertNumericCode) {
    ASSERT_CATCHES(19999, DBException);
    ASSERT_CATCHES(19999, AssertionException);
    ASSERT_NOT_CATCHES(19999, ExceptionFor<ErrorCodes::DuplicateKey>);
    ASSERT_NOT_CATCHES(19999, ExceptionFor<ErrorCategory::NetworkError>);
    ASSERT_NOT_CATCHES(19999, ExceptionFor<ErrorCategory::NotPrimaryError>);
}

TEST(AssertUtils, UassertStatusOKPreservesExtraInfo) {
    const auto status = Status(ErrorExtraInfoExample(123), "");

    try {
        uassertStatusOK(status);
    } catch (const DBException& ex) {
        ASSERT(ex.extraInfo());
        ASSERT(ex.extraInfo<ErrorExtraInfoExample>());
        ASSERT_EQ(ex.extraInfo<ErrorExtraInfoExample>()->data, 123);
    }

    try {
        uassertStatusOK(status);
    } catch (const ExceptionFor<ErrorCodes::ForTestingErrorExtraInfo>& ex) {
        ASSERT(ex.extraInfo());
        ASSERT(ex.extraInfo<ErrorExtraInfoExample>());
        ASSERT_EQ(ex.extraInfo<ErrorExtraInfoExample>()->data, 123);
        ASSERT_EQ(ex->data, 123);
    }
}

TEST(AssertUtils, UassertStatusOKWithContextPreservesExtraInfo) {
    const auto status = Status(ErrorExtraInfoExample(123), "");

    try {
        uassertStatusOKWithContext(status, "foo");
    } catch (const DBException& ex) {
        ASSERT(ex.extraInfo());
        ASSERT(ex.extraInfo<ErrorExtraInfoExample>());
        ASSERT_EQ(ex.extraInfo<ErrorExtraInfoExample>()->data, 123);
    }

    try {
        uassertStatusOKWithContext(status, "foo");
    } catch (const ExceptionFor<ErrorCodes::ForTestingErrorExtraInfo>& ex) {
        ASSERT(ex.extraInfo());
        ASSERT(ex.extraInfo<ErrorExtraInfoExample>());
        ASSERT_EQ(ex.extraInfo<ErrorExtraInfoExample>()->data, 123);
        ASSERT_EQ(ex->data, 123);
    }
}

TEST(AssertUtils, UassertTypedExtraInfoWorks) {
    try {
        uasserted(ErrorExtraInfoExample(123), "");
    } catch (const DBException& ex) {
        ASSERT(ex.extraInfo());
        ASSERT(ex.extraInfo<ErrorExtraInfoExample>());
        ASSERT_EQ(ex.extraInfo<ErrorExtraInfoExample>()->data, 123);
    }

    try {
        uassert(ErrorExtraInfoExample(123), "", false);
    } catch (const ExceptionFor<ErrorCodes::ForTestingErrorExtraInfo>& ex) {
        ASSERT(ex.extraInfo());
        ASSERT(ex.extraInfo<ErrorExtraInfoExample>());
        ASSERT_EQ(ex.extraInfo<ErrorExtraInfoExample>()->data, 123);
        ASSERT_EQ(ex->data, 123);
    }
}

TEST(AssertUtils, UassertIncrementsUserAssertionCounter) {
    auto userAssertions = assertionCount.user.load();
    auto asserted = false;
    try {
        Status status = {ErrorCodes::BadValue, "Test"};
        uassertStatusOK(status);
    } catch (const DBException&) {
        asserted = true;
    }
    ASSERT(asserted);
    ASSERT_EQ(userAssertions + 1, assertionCount.user.load());
}

TEST(AssertUtils, InternalAssertWithStatus) {
    auto userAssertions = assertionCount.user.load();
    try {
        Status status = {ErrorCodes::BadValue, "Test"};
        iassert(status);
    } catch (const DBException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::BadValue);
        ASSERT_EQ(ex.reason(), "Test");
    }

    iassert(Status::OK());

    ASSERT_EQ(userAssertions, assertionCount.user.load());
}

TEST(AssertUtils, InternalAssertWithExpression) {
    auto userAssertions = assertionCount.user.load();
    try {
        iassert(48922, "Test", false);
    } catch (const DBException& ex) {
        ASSERT_EQ(ex.code(), 48922);
        ASSERT_EQ(ex.reason(), "Test");
    }

    iassert(48923, "Another test", true);

    ASSERT_EQ(userAssertions, assertionCount.user.load());
}

TEST(AssertUtils, MassertTypedExtraInfoWorks) {
    try {
        msgasserted(ErrorExtraInfoExample(123), "");
    } catch (const DBException& ex) {
        ASSERT(ex.extraInfo());
        ASSERT(ex.extraInfo<ErrorExtraInfoExample>());
        ASSERT_EQ(ex.extraInfo<ErrorExtraInfoExample>()->data, 123);
    }

    try {
        massert(ErrorExtraInfoExample(123), "", false);
    } catch (const ExceptionFor<ErrorCodes::ForTestingErrorExtraInfo>& ex) {
        ASSERT(ex.extraInfo());
        ASSERT(ex.extraInfo<ErrorExtraInfoExample>());
        ASSERT_EQ(ex.extraInfo<ErrorExtraInfoExample>()->data, 123);
        ASSERT_EQ(ex->data, 123);
    }
}

// tassert
void doTassert() {
    auto tripwireAssertions = assertionCount.tripwire.load();

    try {
        Status status = {ErrorCodes::BadValue, "Test with Status"};
        tassert(status);
    } catch (const DBException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::BadValue);
        ASSERT_EQ(ex.reason(), "Test with Status");
    }
    ASSERT_EQ(tripwireAssertions + 1, assertionCount.tripwire.load());

    tassert(Status::OK());
    ASSERT_EQ(tripwireAssertions + 1, assertionCount.tripwire.load());

    try {
        tassert(4457090, "Test with expression", false);
    } catch (const DBException& ex) {
        ASSERT_EQ(ex.code(), 4457090);
        ASSERT_EQ(ex.reason(), "Test with expression");
    }
    ASSERT_EQ(tripwireAssertions + 2, assertionCount.tripwire.load());

    tassert(4457091, "Another test with expression", true);
    ASSERT_EQ(tripwireAssertions + 2, assertionCount.tripwire.load());

    try {
        tasserted(4457092, "Test with tasserted");
    } catch (const DBException& ex) {
        ASSERT_EQ(ex.code(), 4457092);
        ASSERT_EQ(ex.reason(), "Test with tasserted");
    }
    ASSERT_EQ(tripwireAssertions + 3, assertionCount.tripwire.load());
}

DEATH_TEST_REGEX(TassertTerminationTest,
                 tassertCleanLogMsg,
                 "4457001.*Aborting process during exit due to prior failed tripwire assertions") {
    doTassert();
}

DEATH_TEST_REGEX(TassertTerminationTest,
                 tassertUncleanLogMsg,
                 "4457002.*Detected prior failed tripwire assertions") {
    doTassert();
    quickExit(ExitCode::abrupt);
}

DEATH_TEST(TassertTerminationTest, mongoUnreachableNonFatal, "Hit a MONGO_UNREACHABLE_TASSERT!") {
    try {
        MONGO_UNREACHABLE_TASSERT(4457093);
    } catch (const DBException&) {
        // Catch the DBException, to ensure that we eventually abort during clean exit.
    }
}

DEATH_TEST_REGEX(TassertTerminationTest,
                 mongoUnimplementedNonFatal,
                 "6634500.*Hit a MONGO_UNIMPLEMENTED_TASSERT!") {
    try {
        MONGO_UNIMPLEMENTED_TASSERT(6634500);
    } catch (const DBException&) {
        // Catch the DBException, to ensure that we eventually abort during clean exit.
    }
}

// fassert and its friends
DEATH_TEST(FassertionTerminationTest, BoolCondition, "40206") {
    fassert(40206, false);
}

DEATH_TEST(FassertionTerminationTest, Overload, "Terminating with fassert") {
    fassert(40207, {ErrorCodes::InternalError, "Terminating with fassert"});
}

DEATH_TEST(FassertionTerminationTest, StatusWithOverload, "Terminating with fassert") {
    fassert(50733,
            StatusWith<std::string>{ErrorCodes::InternalError,
                                    "Terminating with fassertStatusWithOverload"});
}

DEATH_TEST(FassertionTerminationTest, NoTrace, "Terminating with fassertNoTrace") {
    fassertNoTrace(50734, Status(ErrorCodes::InternalError, "Terminating with fassertNoTrace"));
}

DEATH_TEST(FassertionTerminationTest, NoTraceOverload, "Terminating with fassertNoTrace") {
    fassertNoTrace(50735,
                   StatusWith<std::string>(ErrorCodes::InternalError,
                                           "Terminating with fassertNoTraceOverload"));
}

DEATH_TEST(FassertionTerminationTest, FassertFailed, "40210") {
    fassertFailed(40210);
}

DEATH_TEST(FassertionTerminationTest, FassertFailedNoTrace, "40211") {
    fassertFailedNoTrace(40211);
}

DEATH_TEST(FassertionTerminationTest,
           FassertFailedWithStatus,
           "Terminating with fassertFailedWithStatus") {
    fassertFailedWithStatus(
        40212, {ErrorCodes::InternalError, "Terminating with fassertFailedWithStatus"});
}

DEATH_TEST(FassertionTerminationTest,
           FassertFailedWithStatusNoTrace,
           "Terminating with fassertFailedWithStatusNoTrace") {
    fassertFailedWithStatusNoTrace(
        40213, {ErrorCodes::InternalError, "Terminating with fassertFailedWithStatusNoTrace"});
}

// invariant and its friends
DEATH_TEST_REGEX(InvariantTerminationTest, invariant, "Invariant failure.*false.*" __FILE__) {
    invariant(false);
}

DEATH_TEST(InvariantTerminationTest, invariantOverload, "Terminating with invariant") {
    invariant(Status(ErrorCodes::InternalError, "Terminating with invariant"));
}

DEATH_TEST(InvariantTerminationTest, mongoUnimplementedFatal, "Hit a MONGO_UNIMPLEMENTED!") {
    MONGO_UNIMPLEMENTED;
}

DEATH_TEST(InvariantTerminationTest, invariantStatusWithOverload, "Terminating with invariant") {
    invariant(StatusWith<std::string>(ErrorCodes::InternalError,
                                      "Terminating with invariantStatusWithOverload"));
}

DEATH_TEST(InvariantTerminationTest,
           invariantWithStringLiteralMsg,
           "Terminating with string literal invariant message") {
    const char* msg = "Terminating with string literal invariant message";
    invariant(false, msg);
}

DEATH_TEST(InvariantTerminationTest,
           invariantWithStdStringMsg,
           "Terminating with std::string invariant message: 12345") {
    const std::string msg = str::stream()
        << "Terminating with std::string invariant message: " << 12345;
    invariant(false, msg);
}

DEATH_TEST(InvariantTerminationTest,
           invariantOverloadWithStringLiteralMsg,
           "Terminating with string literal invariant message") {
    invariant(Status(ErrorCodes::InternalError, "Terminating with invariant"),
              "Terminating with string literal invariant message");
}

DEATH_TEST(InvariantTerminationTest,
           invariantOverloadWithStdStringMsg,
           "Terminating with std::string invariant message: 12345") {
    const std::string msg = str::stream()
        << "Terminating with std::string invariant message: " << 12345;
    invariant(Status(ErrorCodes::InternalError, "Terminating with invariant"), msg);
}

DEATH_TEST(InvariantTerminationTest,
           invariantStatusWithOverloadWithStringLiteralMsg,
           "Terminating with string literal invariant message") {
    invariant(StatusWith<std::string>(ErrorCodes::InternalError, "Terminating with invariant"),
              "Terminating with string literal invariant message");
}

DEATH_TEST(InvariantTerminationTest,
           invariantStatusWithOverloadWithStdStringMsg,
           "Terminating with std::string invariant message: 12345") {
    const std::string msg = str::stream()
        << "Terminating with std::string invariant message: " << 12345;
    invariant(StatusWith<std::string>(ErrorCodes::InternalError, "Terminating with invariant"),
              msg);
}

DEATH_TEST(InvariantTerminationTest, invariantStatusOK, "Terminating with invariantStatusOK") {
    invariantStatusOK(Status(ErrorCodes::InternalError, "Terminating with invariantStatusOK"));
}

DEATH_TEST(InvariantTerminationTest,
           invariantStatusOKOverload,
           "Terminating with invariantStatusOK") {
    invariantStatusOK(
        StatusWith<std::string>(ErrorCodes::InternalError, "Terminating with invariantStatusOK"));
}

DEATH_TEST(InvariantTerminationTest,
           invariantStatusOKWithContext,
           "Terminating with invariantStatusOKWithContext") {
    invariantStatusOKWithContext(
        Status(ErrorCodes::InternalError, "Terminating with invariantStatusOKWithContext"),
        "Terminating with invariantStatusOKWithContext");
}

DEATH_TEST(InvariantTerminationTest,
           invariantStatusOKWithContextOverload,
           "Terminating with invariantStatusOKWithContextOverload") {
    invariantStatusOKWithContext(
        StatusWith<std::string>(ErrorCodes::InternalError,
                                "Terminating with invariantStatusOKWithContext"),
        "Terminating with invariantStatusOKWithContextOverload");
}

#if defined(MONGO_CONFIG_DEBUG_BUILD)
// dassert and its friends
DEATH_TEST_REGEX(DassertTerminationTest, invariant, "Invariant failure.*false.*" __FILE__) {
    dassert(false);
}

DEATH_TEST(DassertTerminationTest, dassertOK, "Terminating with dassertOK") {
    dassert(Status(ErrorCodes::InternalError, "Terminating with dassertOK"));
}

DEATH_TEST(DassertTerminationTest,
           invariantWithStringLiteralMsg,
           "Terminating with string literal dassert message") {
    const char* msg = "Terminating with string literal dassert message";
    dassert(false, msg);
}

DEATH_TEST(DassertTerminationTest,
           dassertWithStdStringMsg,
           "Terminating with std::string dassert message: 12345") {
    const std::string msg = str::stream()
        << "Terminating with std::string dassert message: " << 12345;
    dassert(false, msg);
}
#endif  // defined(MONGO_CONFIG_DEBUG_BUILD)

TEST(ScopedDebugInfo, Stack) {
    using namespace unittest::match;
    ScopedDebugInfoStack infoStack{};  // Avoiding the tls instance for now.
    std::vector<std::string> expected;
    ASSERT_THAT(infoStack.getAll(), Eq(expected));
    {
        ScopedDebugInfo greetingGuard("greeting", "hello", &infoStack);
        expected.push_back("greeting: hello");
        ASSERT_THAT(infoStack.getAll(), Eq(expected));

        ScopedDebugInfo numberGuard("age", 123, &infoStack);
        expected.push_back("age: 123");
        ASSERT_THAT(infoStack.getAll(), Eq(expected));

        {
            ScopedDebugInfo innerGuard("inner", 222, &infoStack);
            expected.push_back("inner: 222");
            ASSERT_THAT(infoStack.getAll(), Eq(expected));
            expected.pop_back();  // innerGuard
        }
        ASSERT_THAT(infoStack.getAll(), Eq(expected));
        expected.pop_back();  // numberGuard
        expected.pop_back();  // greetingGuard
    }
    ASSERT_THAT(infoStack.getAll(), Eq(expected));
}

DEATH_TEST(ScopedDebugInfo, PrintedOnInvariant, "mission: ATestInjectedString") {
    ScopedDebugInfo g("mission", "ATestInjectedString");
    someRiskyBusiness();
}

// The following test relies on SIGSEGV which is not supported on Windows.
#if !defined(_WIN32)
DEATH_TEST(ScopedDebugInfo, PrintedOnSignal, "mission: ATestInjectedString") {
    ScopedDebugInfo g("mission", "ATestInjectedString");
    raise(SIGSEGV);
}
#endif

TEST(ScopedDebugInfo, FormattingCanBeCalledMoreThanOnce) {
    using namespace unittest::match;
    ScopedDebugInfoStack infoStack{};

    ScopedDebugInfo guard("greeting", "hello", &infoStack);
    // A second call to `getAll` returns correct results.
    auto _ = infoStack.getAll();
    ASSERT_THAT(infoStack.getAll(), ElementsAre(Eq("greeting: hello")));
}

// Test that we are able to incrementally log ScopedDebugInfos from the stack (if one throws an
// exception, we are able to attempt to print the next one).
DEATH_TEST_REGEX(ScopedDebugInfo,
                 CorrectScopedDebugInfosOnStackAfterIncorrectOne,
                 "(?s)tasserting in mock printer.*BACKTRACE"
                 ".*ScopedDebugInfo failed.*test.*9513401") {
    ScopedDebugInfoStack infoStack{};
    ScopedDebugInfo greetingGuard("greeting", "hello", &infoStack);
    ScopedDebugInfo guardTasserts("test", PrinterMockTassert(), &infoStack);
    ScopedDebugInfo anotherGreetingGuard("greeting", "hey there", &infoStack);

    using namespace unittest::match;
    ASSERT_THAT(infoStack.getAll(), ElementsAre(Eq("greeting: hello"), Eq("greeting: hey there")));
}

// Test that if we log the `ScopedDebugInfoStack` due to a tassert, and we hit a tassert during
// logging, we only surface the original tassert.
DEATH_TEST_REGEX(ScopedDebugInfo,
                 TassertAndTassertDuringLogging,
                 "(?s)tasserting in test.*BACKTRACE"
                 ".*tasserting in mock printer.*BACKTRACE"
                 ".*ScopedDebugInfo failed.*test.*9513401") {
    ScopedDebugInfo guardTasserts("test", PrinterMockTassert());
    tasserted(9513403, "tasserting in test");
}

// Same as above, but with `uassert` instead of `tassert`.
DEATH_TEST_REGEX(ScopedDebugInfo,
                 TassertAndUassertDuringLogging,
                 "(?s)tasserting in test.*BACKTRACE"
                 ".*ScopedDebugInfo failed.*test.*9513402") {
    ScopedDebugInfo guardUasserts("test", PrinterMockUassert());
    tasserted(9513404, "tasserting in test");
}

// Test that we end the process as expected due to an invariant even if we hit a tassert during
// logging of the `ScopedDebugInfoStack`.
DEATH_TEST_REGEX(ScopedDebugInfo,
                 InvariantAndTassertDuringLogging,
                 "(?s)ouch.*abruptQuit"
                 ".*tasserting in mock printer.*mongo::tassertFailed"
                 ".*ScopedDebugInfo failed.*test.*9513401") {
    ScopedDebugInfo guardTasserts("test", PrinterMockTassert());
    someRiskyBusiness();
}

// Same as above, but with `uassert` instead of `tassert`.
DEATH_TEST_REGEX(ScopedDebugInfo,
                 InvariantAndUassertDuringLogging,
                 "(?s)ouch.*abruptQuit"
                 ".*ScopedDebugInfo failed.*test.*9513402") {
    ScopedDebugInfo guardUasserts("test", PrinterMockUassert());
    someRiskyBusiness();
}

DEATH_TEST_REGEX(ScopedDebugInfo,
                 InvariantAndInvariantDuringLogging,
                 "(?s)ouch.*abruptQuit.*ouch") {
    ScopedDebugInfo guardInvariants("test", PrinterMockInvariant());
    someRiskyBusiness();
}

// Tests the functionality of the `signalHandlerUsesDiagnosticLogging` knob.
// Note the regex here uses the negative lookahead operator `(?!)` in order to ensure that the
// string `hello` is not found within the logs. The syntax here means: we want to match the start of
// the string unless something after the start of the string contains `hello`.
DEATH_TEST_REGEX(ScopedDebugInfo, InvariantWhenSignalHandlerLoggingDisabled, "(?s)^(?!.*hello)") {
    RAIIServerParameterControllerForTest knobController("signalHandlerUsesDiagnosticLogging",
                                                        false);
    ScopedDebugInfo guard("test", "hello");
    someRiskyBusiness();
}

// The following tests relies on SIGSEGV which is not supported on Windows.
// Test that we end the process as expected due to a seg fault even if we hit a tassert during
// logging of the `ScopedDebugInfoStack`.
#if !defined(_WIN32)
DEATH_TEST_REGEX(ScopedDebugInfo,
                 SignalAndTassertDuringLogging,
                 "(?s)Invalid access at address.*abruptQuit"
                 ".*tasserting in mock printer.*BACKTRACE"
                 ".*ScopedDebugInfo failed.*test.*9513401") {
    ScopedDebugInfo guardTasserts("test", PrinterMockTassert());
    raise(SIGSEGV);
}

// Same as above, but with `uassert` instead of `tassert`.
DEATH_TEST_REGEX(ScopedDebugInfo,
                 SignalAndUassertDuringLogging,
                 "(?s)Invalid access at address.*abruptQuit"
                 ".*ScopedDebugInfo failed.*test.*9513402") {
    ScopedDebugInfo guardUasserts("test", PrinterMockUassert());
    raise(SIGSEGV);
}

// Tests the functionality of the `signalHandlerUsesDiagnosticLogging` knob with a signal
// raised.
// Note the regex here uses the negative lookahead operator `(?!)` in order to ensure that the
// string `hello` is not found within the logs. The syntax here means: we want to match the start of
// the string unless something after the start of the string contains `hello`.
DEATH_TEST_REGEX(ScopedDebugInfo, SignalWhenSignalHandlerLoggingDisabled, "(?s)^(?!.*hello)") {
    RAIIServerParameterControllerForTest knobController("signalHandlerUsesDiagnosticLogging",
                                                        false);
    ScopedDebugInfo guard("test", "hello");
    raise(SIGSEGV);
}
#endif

void mustNotCompile() {
#if 0
    fassert(9079709, "match");
    fassert(true, Status::OK());
    fassert(ErrorCodes::InternalError, "hi");
#endif
}


constexpr bool checkCanUseInvariantInConstexprCode = [] {
    invariant(true);
    invariant(true, "with message");
    invariant(Status::OK());
    invariant(StatusWith(1));
    invariant(Status::OK(), "with message");
    invariant(StatusWith(1), "with message");

    return true;
}();

constexpr bool checkCanUseUMITassertInConstexprCode = [] {
    uassert(ErrorCodes::BadValue, "with message", true);
    uassertStatusOK(Status::OK());
    uassertStatusOK(StatusWith(1));
    uassertStatusOKWithContext(Status::OK(), "context");
    uassertStatusOKWithContext(StatusWith(1), "context");

    massert(ErrorCodes::BadValue, "with message", true);
    massertStatusOK(Status::OK());
    massertStatusOK(StatusWith(1));

    iassert(ErrorCodes::BadValue, "with message", true);
    iassert(Status::OK());
    iassert(StatusWith(1));

    tassert(ErrorCodes::BadValue, "with message", true);
    tassert(Status::OK());
    tassert(StatusWith(1));

    return true;
}();

#define isConstexpr(expr)                       \
    [] {                                        \
        static const bool b = [] {              \
            if (std::is_constant_evaluated()) { \
                (void)expr;                     \
                return true;                    \
            } else {                            \
                return false;                   \
            }                                   \
        }();                                    \
        return b;                               \
    }()

TEST(AssertUtils, FailuresAreNotConstexpr) {
    // self-test of `isConstexpr`
    ASSERT(isConstexpr(1));
    ASSERT(!isConstexpr(std::terminate()));

    ASSERT(isConstexpr(Status::OK()));
    ASSERT(isConstexpr(StatusWith(1)));
    ASSERT(isConstexpr(invariant(true)));
    ASSERT(isConstexpr([] {
        uassert(ErrorCodes::BadValue, "", true);
    }()));

    ASSERT(!isConstexpr(Status(ErrorCodes::BadValue, "")));
    ASSERT(!isConstexpr(StatusWith<int>(ErrorCodes::BadValue, "")));
    ASSERT(!isConstexpr(invariant(false)));
    ASSERT(!isConstexpr([] {
        uassert(ErrorCodes::BadValue, "", false);
    }()));
}

}  // namespace
}  // namespace mongo
