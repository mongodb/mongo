// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#ifdef __linux__

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/db/startup_warnings_mongod.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

namespace swd = mongo::startup_warning_detail;
using mongo::unittest::match::Eq;

TEST(THPStartupWarnings, EnablementStates) {
    using E = swd::THPEnablementWarningLogCase;

    ASSERT_THAT(swd::getTHPEnablementWarningCase(true, "always", false), Eq(E::kNone))
        << "good google case";

    ASSERT_THAT(swd::getTHPEnablementWarningCase(false, "never", false), Eq(E::kNone))
        << "good gperftools case";

    ASSERT_THAT(swd::getTHPEnablementWarningCase(false, "always", true), Eq(E::kNone))
        << "good gperftools opt-out case";

    ASSERT_THAT(swd::getTHPEnablementWarningCase(true,
                                                 Status(ErrorCodes::BadValue, ""),
                                                 std::make_error_code(std::errc::invalid_argument)),
                Eq(E::kSystemValueErrorWithOptOutError))
        << "double error case";

    ASSERT_THAT(swd::getTHPEnablementWarningCase(true, Status(ErrorCodes::BadValue, ""), true),
                Eq(E::kSystemValueErrorWithWrongOptOut))
        << "sys error and wrongly opting out case";

    ASSERT_THAT(swd::getTHPEnablementWarningCase(false, "always", false), Eq(E::kWronglyEnabled))
        << "wrongly enabled case";

    ASSERT_THAT(swd::getTHPEnablementWarningCase(true, "always", true),
                Eq(E::kWronglyDisabledViaOptOut))
        << "wrongly disabled via opt-out case";

    ASSERT_THAT(swd::getTHPEnablementWarningCase(true, "never", false),
                Eq(E::kWronglyDisabledOnSystem))
        << "wrongly disabled on system case";

    ASSERT_THAT(swd::getTHPEnablementWarningCase(
                    true, "always", std::make_error_code(std::errc::invalid_argument)),
                Eq(E::kOptOutError))
        << "error retrieving out opt value case";
}

TEST(THPStartupWarnings, DefragStates) {
    ASSERT_EQ(swd::getDefragWarningCase(true, "defer+madvise"),
              swd::THPDefragWarningLogCase::kNone);
    ASSERT_EQ(swd::getDefragWarningCase(true, "never"),
              swd::THPDefragWarningLogCase::kWronglyNotUsingDeferMadvise);
    ASSERT_EQ(swd::getDefragWarningCase(true, "always"),
              swd::THPDefragWarningLogCase::kWronglyNotUsingDeferMadvise);
    ASSERT_EQ(swd::getDefragWarningCase(false, "always"), swd::THPDefragWarningLogCase::kNone);
}

TEST(THPStartupWarnings, MaxPtesNone) {
    ASSERT_TRUE(swd::verifyMaxPtesNoneIsCorrect(true, 0));
    ASSERT_FALSE(swd::verifyMaxPtesNoneIsCorrect(true, 1));
    ASSERT_TRUE(swd::verifyMaxPtesNoneIsCorrect(false, 0));
    ASSERT_TRUE(swd::verifyMaxPtesNoneIsCorrect(false, 1));
}

}  // namespace
}  // namespace mongo

#endif  // __linux__
