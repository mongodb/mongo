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

#ifdef __linux__

#include "mongo/db/startup_warnings_mongod.h"

#include <fstream>
#include <ostream>

#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using swm = StartupWarningsMongodLinux;
using unittest::TempDir;
using unittest::match::Eq;

TEST(StartupWarningsMongodTest, ReadTransparentHugePagesParameterInvalidDirectory) {
    StatusWith<std::string> result =
        swm::readTransparentHugePagesParameter("no_such_directory", "param");
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NonExistentPath, result.getStatus().code());
}

TEST(StartupWarningsMongodTest, ReadTransparentHugePagesParameterInvalidFile) {
    TempDir tempDir("StartupWarningsMongodTest_ReadTransparentHugePagesParameterInvalidFile");
    StatusWith<std::string> result =
        swm::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NonExistentPath, result.getStatus().code());
}

TEST(StartupWarningsMongodTest, ReadTransparentHugePagesParameterEmptyFile) {
    TempDir tempDir("StartupWarningsMongodTest_ReadTransparentHugePagesParameterInvalidFile");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream(filename.c_str());
    }
    StatusWith<std::string> result =
        swm::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FileStreamFailed, result.getStatus().code());
}

TEST(StartupWarningsMongodTest, ReadTransparentHugePagesParameterBlankLine) {
    TempDir tempDir("StartupWarningsMongodTest_ReadTransparentHugePagesParameterBlankLine");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << std::endl;
    }
    StatusWith<std::string> result =
        swm::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST(StartupWarningsMongodTest, ReadTransparentHugePagesParameterInvalidFormat) {
    TempDir tempDir("StartupWarningsMongodTest_ReadTransparentHugePagesParameterBlankLine");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise never" << std::endl;
    }
    StatusWith<std::string> result =
        swm::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST(StartupWarningsMongodTest, ReadTransparentHugePagesParameterEmptyOpMode) {
    TempDir tempDir("StartupWarningsMongodTest_ReadTransparentHugePagesParameterEmptyOpMode");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise [] never" << std::endl;
    }
    StatusWith<std::string> result =
        swm::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
}

TEST(StartupWarningsMongodTest, ReadTransparentHugePagesParameterUnrecognizedOpMode) {
    TempDir tempDir(
        "StartupWarningsMongodTest_ReadTransparentHugePagesParameterUnrecognizedOpMode");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise never [unknown]" << std::endl;
    }
    StatusWith<std::string> result =
        swm::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
}

void readTransparentHugePagesParameter(const std::string& parameter, const std::string& valueStr) {
    TempDir tempDir("StartupWarningsMongodTest_ReadTransparentHugePagesParameter");
    {
        std::string filename(tempDir.path() + "/" + parameter);
        std::ofstream ofs(filename.c_str());
        ofs << valueStr << std::endl;
    }

    std::string::size_type posBegin = valueStr.find('[');
    std::string::size_type posEnd = valueStr.find(']');
    std::string mode = valueStr.substr(posBegin + 1, posEnd - posBegin - 1);

    auto result = swm::readTransparentHugePagesParameter(parameter, tempDir.path());

    if (parameter == kTHPEnabledParameter && (mode == "defer" || mode == "defer+madvise")) {
        ASSERT_NOT_OK(result.getStatus());
        ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
        return;
    }
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(mode, result.getValue());
}

TEST(StartupWarningsMongodTest, ReadTransparentHugePagesEnabledAlways) {
    readTransparentHugePagesParameter(kTHPEnabledParameter,
                                      "[always] defer defer+madvise madvise never");
}

TEST(StartupWarningsMongodTest, ReadTransparentHugePagesEnabledDefer) {
    readTransparentHugePagesParameter(kTHPEnabledParameter,
                                      "always [defer] defer+madvise madvise never");
}

TEST(StartupWarningsMongodTest, ReadTransparentHugePagesEnabledDeferMadvise) {
    readTransparentHugePagesParameter(kTHPEnabledParameter,
                                      "always defer [defer+madvise] madvise never");
}

TEST(StartupWarningsMongodTest, ReadTransparentHugePagesEnabledMadvise) {
    readTransparentHugePagesParameter(kTHPEnabledParameter,
                                      "always defer defer+madvise [madvise] never");
}

TEST(StartupWarningsMongodTest, ReadTransparentHugePagesEnabledNever) {
    readTransparentHugePagesParameter(kTHPEnabledParameter,
                                      "always defer defer+madvise madvise [never]");
}

TEST(StartupWarningsMongodTest, EnablementStates) {
    using E = swm::THPEnablementWarningLogCase;

    ASSERT_THAT(swm::getTHPEnablementWarningCase("never", false), Eq(E::kNone)) << "good case";

    ASSERT_THAT(swm::getTHPEnablementWarningCase("always", true), Eq(E::kNone))
        << "good opt-out case";

    ASSERT_THAT(swm::getTHPEnablementWarningCase(Status(ErrorCodes::BadValue, ""),
                                                 std::make_error_code(std::errc::invalid_argument)),
                Eq(E::kSystemValueErrorWithOptOutError))
        << "double error case";

    ASSERT_THAT(swm::getTHPEnablementWarningCase("always", false), Eq(E::kWronglyEnabled))
        << "wrongly enabled case";

    ASSERT_THAT(swm::getTHPEnablementWarningCase("always",
                                                 std::make_error_code(std::errc::invalid_argument)),
                Eq(E::kOptOutError))
        << "error retrieving out opt value case";

    ASSERT_THAT(swm::getTHPEnablementWarningCase("never",
                                                 std::make_error_code(std::errc::invalid_argument)),
                Eq(E::kNone))
        << "good value opt-out error case";
}

}  // namespace

}  // namespace mongo

#endif  // __linux__
