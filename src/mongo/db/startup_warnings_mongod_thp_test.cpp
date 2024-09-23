/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#ifdef __linux__

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
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
