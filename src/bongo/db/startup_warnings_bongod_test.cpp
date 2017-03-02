/**
 *    Copyright 2015 BongoDB Inc.
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

#include "bongo/platform/basic.h"

#include <fstream>
#include <ostream>

#include "bongo/db/startup_warnings_bongod.h"
#include "bongo/unittest/temp_dir.h"
#include "bongo/unittest/unittest.h"

namespace {

using bongo::unittest::TempDir;

using namespace bongo;

TEST(StartupWarningsBongodTest, ReadTransparentHugePagesParameterInvalidDirectory) {
    StatusWith<std::string> result =
        StartupWarningsBongod::readTransparentHugePagesParameter("no_such_directory", "param");
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NonExistentPath, result.getStatus().code());
}

TEST(StartupWarningsBongodTest, ReadTransparentHugePagesParameterInvalidFile) {
    TempDir tempDir("StartupWarningsBongodTest_ReadTransparentHugePagesParameterInvalidFile");
    StatusWith<std::string> result =
        StartupWarningsBongod::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NonExistentPath, result.getStatus().code());
}

TEST(StartupWarningsBongodTest, ReadTransparentHugePagesParameterEmptyFile) {
    TempDir tempDir("StartupWarningsBongodTest_ReadTransparentHugePagesParameterInvalidFile");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream(filename.c_str());
    }
    StatusWith<std::string> result =
        StartupWarningsBongod::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FileStreamFailed, result.getStatus().code());
}

TEST(StartupWarningsBongodTest, ReadTransparentHugePagesParameterBlankLine) {
    TempDir tempDir("StartupWarningsBongodTest_ReadTransparentHugePagesParameterBlankLine");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << std::endl;
    }
    StatusWith<std::string> result =
        StartupWarningsBongod::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST(StartupWarningsBongodTest, ReadTransparentHugePagesParameterInvalidFormat) {
    TempDir tempDir("StartupWarningsBongodTest_ReadTransparentHugePagesParameterBlankLine");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise never" << std::endl;
    }
    StatusWith<std::string> result =
        StartupWarningsBongod::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST(StartupWarningsBongodTest, ReadTransparentHugePagesParameterEmptyOpMode) {
    TempDir tempDir("StartupWarningsBongodTest_ReadTransparentHugePagesParameterEmptyOpMode");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise [] never" << std::endl;
    }
    StatusWith<std::string> result =
        StartupWarningsBongod::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
}

TEST(StartupWarningsBongodTest, ReadTransparentHugePagesParameterUnrecognizedOpMode) {
    TempDir tempDir(
        "StartupWarningsBongodTest_ReadTransparentHugePagesParameterUnrecognizedOpMode");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise never [unknown]" << std::endl;
    }
    StatusWith<std::string> result =
        StartupWarningsBongod::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
}

TEST(StartupWarningsBongodTest, ReadTransparentHugePagesParameterValidFormat) {
    TempDir tempDir("StartupWarningsBongodTest_ReadTransparentHugePagesParameterBlankLine");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise [never]" << std::endl;
    }
    StatusWith<std::string> result =
        StartupWarningsBongod::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS("never", result.getValue());
}

}  // namespace
