/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/config.h"
#include "mongo/platform/basic.h"

#include <type_traits>

#include "mongo/base/static_assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

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
MONGO_STATIC_ASSERT(!std::is_base_of<ExceptionForCat<ErrorCategory::NetworkError>,
                                     ExceptionFor<ErrorCodes::BadValue>>());
MONGO_STATIC_ASSERT(!std::is_base_of<ExceptionForCat<ErrorCategory::NotMasterError>,
                                     ExceptionFor<ErrorCodes::BadValue>>());
MONGO_STATIC_ASSERT(!std::is_base_of<ExceptionForCat<ErrorCategory::Interruption>,
                                     ExceptionFor<ErrorCodes::BadValue>>());

TEST(AssertUtils, UassertNamedCodeWithoutCategories) {
    ASSERT_CATCHES(ErrorCodes::BadValue, DBException);
    ASSERT_CATCHES(ErrorCodes::BadValue, AssertionException);
    ASSERT_CATCHES(ErrorCodes::BadValue, ExceptionFor<ErrorCodes::BadValue>);
    ASSERT_NOT_CATCHES(ErrorCodes::BadValue, ExceptionFor<ErrorCodes::DuplicateKey>);
    ASSERT_NOT_CATCHES(ErrorCodes::BadValue, ExceptionForCat<ErrorCategory::NetworkError>);
    ASSERT_NOT_CATCHES(ErrorCodes::BadValue, ExceptionForCat<ErrorCategory::NotMasterError>);
    ASSERT_NOT_CATCHES(ErrorCodes::BadValue, ExceptionForCat<ErrorCategory::Interruption>);
}

// NotMaster - just NotMasterError
MONGO_STATIC_ASSERT(std::is_same<error_details::ErrorCategoriesFor<ErrorCodes::NotMaster>,
                                 error_details::CategoryList<ErrorCategory::NotMasterError>>());
MONGO_STATIC_ASSERT(std::is_base_of<AssertionException, ExceptionFor<ErrorCodes::NotMaster>>());
MONGO_STATIC_ASSERT(!std::is_base_of<ExceptionForCat<ErrorCategory::NetworkError>,
                                     ExceptionFor<ErrorCodes::NotMaster>>());
MONGO_STATIC_ASSERT(std::is_base_of<ExceptionForCat<ErrorCategory::NotMasterError>,
                                    ExceptionFor<ErrorCodes::NotMaster>>());
MONGO_STATIC_ASSERT(!std::is_base_of<ExceptionForCat<ErrorCategory::Interruption>,
                                     ExceptionFor<ErrorCodes::NotMaster>>());

TEST(AssertUtils, UassertNamedCodeWithOneCategory) {
    ASSERT_CATCHES(ErrorCodes::NotMaster, DBException);
    ASSERT_CATCHES(ErrorCodes::NotMaster, AssertionException);
    ASSERT_CATCHES(ErrorCodes::NotMaster, ExceptionFor<ErrorCodes::NotMaster>);
    ASSERT_NOT_CATCHES(ErrorCodes::NotMaster, ExceptionFor<ErrorCodes::DuplicateKey>);
    ASSERT_NOT_CATCHES(ErrorCodes::NotMaster, ExceptionForCat<ErrorCategory::NetworkError>);
    ASSERT_CATCHES(ErrorCodes::NotMaster, ExceptionForCat<ErrorCategory::NotMasterError>);
    ASSERT_NOT_CATCHES(ErrorCodes::NotMaster, ExceptionForCat<ErrorCategory::Interruption>);
}

// InterruptedDueToStepDown - NotMasterError and Interruption
MONGO_STATIC_ASSERT(
    std::is_same<
        error_details::ErrorCategoriesFor<ErrorCodes::InterruptedDueToStepDown>,
        error_details::CategoryList<ErrorCategory::Interruption, ErrorCategory::NotMasterError>>());
MONGO_STATIC_ASSERT(
    std::is_base_of<AssertionException, ExceptionFor<ErrorCodes::InterruptedDueToStepDown>>());
MONGO_STATIC_ASSERT(!std::is_base_of<ExceptionForCat<ErrorCategory::NetworkError>,
                                     ExceptionFor<ErrorCodes::InterruptedDueToStepDown>>());
MONGO_STATIC_ASSERT(std::is_base_of<ExceptionForCat<ErrorCategory::NotMasterError>,
                                    ExceptionFor<ErrorCodes::InterruptedDueToStepDown>>());
MONGO_STATIC_ASSERT(std::is_base_of<ExceptionForCat<ErrorCategory::Interruption>,
                                    ExceptionFor<ErrorCodes::InterruptedDueToStepDown>>());

TEST(AssertUtils, UassertNamedCodeWithTwoCategories) {
    ASSERT_CATCHES(ErrorCodes::InterruptedDueToStepDown, DBException);
    ASSERT_CATCHES(ErrorCodes::InterruptedDueToStepDown, AssertionException);
    ASSERT_CATCHES(ErrorCodes::InterruptedDueToStepDown,
                   ExceptionFor<ErrorCodes::InterruptedDueToStepDown>);
    ASSERT_NOT_CATCHES(ErrorCodes::InterruptedDueToStepDown,
                       ExceptionFor<ErrorCodes::DuplicateKey>);
    ASSERT_NOT_CATCHES(ErrorCodes::InterruptedDueToStepDown,
                       ExceptionForCat<ErrorCategory::NetworkError>);
    ASSERT_CATCHES(ErrorCodes::InterruptedDueToStepDown,
                   ExceptionForCat<ErrorCategory::NotMasterError>);
    ASSERT_CATCHES(ErrorCodes::InterruptedDueToStepDown,
                   ExceptionForCat<ErrorCategory::Interruption>);
}

MONGO_STATIC_ASSERT(!error_details::isNamedCode<19999>);
// ExceptionFor<ErrorCodes::Error(19999)> invalidType;  // Must not compile.

TEST(AssertUtils, UassertNumericCode) {
    ASSERT_CATCHES(19999, DBException);
    ASSERT_CATCHES(19999, AssertionException);
    ASSERT_NOT_CATCHES(19999, ExceptionFor<ErrorCodes::DuplicateKey>);
    ASSERT_NOT_CATCHES(19999, ExceptionForCat<ErrorCategory::NetworkError>);
    ASSERT_NOT_CATCHES(19999, ExceptionForCat<ErrorCategory::NotMasterError>);
    ASSERT_NOT_CATCHES(19999, ExceptionForCat<ErrorCategory::Interruption>);
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

// uassert and its friends
DEATH_TEST(UassertionTerminationTest, uassert, "Terminating with uassert") {
    uassert(40204, "Terminating with uassert", false);
}

DEATH_TEST(UassertionTerminationTest, uasserted, "Terminating with uasserted") {
    uasserted(40205, "Terminating with uasserted");
}

DEATH_TEST(UassertionTerminationTest, uassertStatusOK, "Terminating with uassertStatusOK") {
    uassertStatusOK(Status(ErrorCodes::InternalError, "Terminating with uassertStatusOK"));
}

DEATH_TEST(UassertionTerminationTest, uassertStatusOKOverload, "Terminating with uassertStatusOK") {
    uassertStatusOK(
        StatusWith<std::string>(ErrorCodes::InternalError, "Terminating with uassertStatusOK"));
}

// fassert and its friends
DEATH_TEST(FassertionTerminationTest, fassert, "40206") {
    fassert(40206, false);
}

DEATH_TEST(FassertionTerminationTest, fassertOverload, "Terminating with fassert") {
    fassert(40207, {ErrorCodes::InternalError, "Terminating with fassert"});
}

DEATH_TEST(FassertionTerminationTest, fassertStatusWithOverload, "Terminating with fassert") {
    fassert(50733,
            StatusWith<std::string>{ErrorCodes::InternalError,
                                    "Terminating with fassertStatusWithOverload"});
}

DEATH_TEST(FassertionTerminationTest, fassertNoTrace, "Terminating with fassertNoTrace") {
    fassertNoTrace(50734, Status(ErrorCodes::InternalError, "Terminating with fassertNoTrace"));
}

DEATH_TEST(FassertionTerminationTest, fassertNoTraceOverload, "Terminating with fassertNoTrace") {
    fassertNoTrace(50735,
                   StatusWith<std::string>(ErrorCodes::InternalError,
                                           "Terminating with fassertNoTraceOverload"));
}

DEATH_TEST(FassertionTerminationTest, fassertFailed, "40210") {
    fassertFailed(40210);
}

DEATH_TEST(FassertionTerminationTest, fassertFailedNoTrace, "40211") {
    fassertFailedNoTrace(40211);
}

DEATH_TEST(FassertionTerminationTest,
           fassertFailedWithStatus,
           "Terminating with fassertFailedWithStatus") {
    fassertFailedWithStatus(
        40212, {ErrorCodes::InternalError, "Terminating with fassertFailedWithStatus"});
}

DEATH_TEST(FassertionTerminationTest,
           fassertFailedWithStatusNoTrace,
           "Terminating with fassertFailedWithStatusNoTrace") {
    fassertFailedWithStatusNoTrace(
        40213, {ErrorCodes::InternalError, "Terminating with fassertFailedWithStatusNoTrace"});
}

// massert and its friends
DEATH_TEST(MassertionTerminationTest, massert, "Terminating with massert") {
    massert(40214, "Terminating with massert", false);
}


DEATH_TEST(MassertionTerminationTest, massertStatusOK, "Terminating with massertStatusOK") {
    massertStatusOK(Status(ErrorCodes::InternalError, "Terminating with massertStatusOK"));
}

DEATH_TEST(MassertionTerminationTest, msgasserted, "Terminating with msgasserted") {
    msgasserted(40215, "Terminating with msgasserted");
}

// invariant and its friends
DEATH_TEST(InvariantTerminationTest, invariant, "Invariant failure false " __FILE__) {
    invariant(false);
}

DEATH_TEST(InvariantTerminationTest, invariantOverload, "Terminating with invariant") {
    invariant(Status(ErrorCodes::InternalError, "Terminating with invariant"));
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
    const std::string msg = str::stream() << "Terminating with std::string invariant message: "
                                          << 12345;
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
    const std::string msg = str::stream() << "Terminating with std::string invariant message: "
                                          << 12345;
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
    const std::string msg = str::stream() << "Terminating with std::string invariant message: "
                                          << 12345;
    invariant(StatusWith<std::string>(ErrorCodes::InternalError, "Terminating with invariant"),
              msg);
}

#if defined(MONGO_CONFIG_DEBUG_BUILD)
// dassert and its friends
DEATH_TEST(DassertTerminationTest, invariant, "Invariant failure false " __FILE__) {
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
    const std::string msg = str::stream() << "Terminating with std::string dassert message: "
                                          << 12345;
    dassert(false, msg);
}
#endif  // defined(MONGO_CONFIG_DEBUG_BUILD)

}  // namespace
}  // namespace mongo
