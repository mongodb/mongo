/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include <fstream>
#include <ostream>

#include "merizo/db/startup_warnings_merizod.h"
#include "merizo/unittest/temp_dir.h"
#include "merizo/unittest/unittest.h"

namespace {

using merizo::unittest::TempDir;

using namespace merizo;

TEST(StartupWarningsMerizodTest, ReadTransparentHugePagesParameterInvalidDirectory) {
    StatusWith<std::string> result =
        StartupWarningsMerizod::readTransparentHugePagesParameter("no_such_directory", "param");
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NonExistentPath, result.getStatus().code());
}

TEST(StartupWarningsMerizodTest, ReadTransparentHugePagesParameterInvalidFile) {
    TempDir tempDir("StartupWarningsMerizodTest_ReadTransparentHugePagesParameterInvalidFile");
    StatusWith<std::string> result =
        StartupWarningsMerizod::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NonExistentPath, result.getStatus().code());
}

TEST(StartupWarningsMerizodTest, ReadTransparentHugePagesParameterEmptyFile) {
    TempDir tempDir("StartupWarningsMerizodTest_ReadTransparentHugePagesParameterInvalidFile");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream(filename.c_str());
    }
    StatusWith<std::string> result =
        StartupWarningsMerizod::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FileStreamFailed, result.getStatus().code());
}

TEST(StartupWarningsMerizodTest, ReadTransparentHugePagesParameterBlankLine) {
    TempDir tempDir("StartupWarningsMerizodTest_ReadTransparentHugePagesParameterBlankLine");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << std::endl;
    }
    StatusWith<std::string> result =
        StartupWarningsMerizod::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST(StartupWarningsMerizodTest, ReadTransparentHugePagesParameterInvalidFormat) {
    TempDir tempDir("StartupWarningsMerizodTest_ReadTransparentHugePagesParameterBlankLine");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise never" << std::endl;
    }
    StatusWith<std::string> result =
        StartupWarningsMerizod::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST(StartupWarningsMerizodTest, ReadTransparentHugePagesParameterEmptyOpMode) {
    TempDir tempDir("StartupWarningsMerizodTest_ReadTransparentHugePagesParameterEmptyOpMode");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise [] never" << std::endl;
    }
    StatusWith<std::string> result =
        StartupWarningsMerizod::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
}

TEST(StartupWarningsMerizodTest, ReadTransparentHugePagesParameterUnrecognizedOpMode) {
    TempDir tempDir(
        "StartupWarningsMerizodTest_ReadTransparentHugePagesParameterUnrecognizedOpMode");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise never [unknown]" << std::endl;
    }
    StatusWith<std::string> result =
        StartupWarningsMerizod::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
}

TEST(StartupWarningsMerizodTest, ReadTransparentHugePagesParameterValidFormat) {
    TempDir tempDir("StartupWarningsMerizodTest_ReadTransparentHugePagesParameterBlankLine");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise [never]" << std::endl;
    }
    StatusWith<std::string> result =
        StartupWarningsMerizod::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS("never", result.getValue());
}

}  // namespace
