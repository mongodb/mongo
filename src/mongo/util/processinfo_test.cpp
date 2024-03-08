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

#include <cstdint>
#include <fstream>
#include <map>

#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/processinfo.h"

using boost::optional;
using mongo::unittest::TempDir;

namespace mongo {

namespace {
using StringMap = std::map<std::string, uint64_t>;

StringMap toStringMap(BSONObj& obj) {
    StringMap map;

    for (const auto& e : obj) {
        map[e.fieldName()] = e.numberLong();
    }

    return map;
}

#define ASSERT_KEY(_key) ASSERT_TRUE(stringMap.find(_key) != stringMap.end());

TEST(ProcessInfo, SysInfoIsInitialized) {
    ProcessInfo processInfo;
    if (processInfo.supported()) {
        ASSERT_FALSE(processInfo.getOsType().empty());
    }
}

TEST(ProcessInfo, TestSysInfo) {
    auto sysInfo = ProcessInfo();
    BSONObjBuilder builder;
    sysInfo.appendSystemDetails(builder);
    BSONObj obj = builder.obj();

    auto stringMap = toStringMap(obj);
    ASSERT_KEY("cpuString");

#if defined(__linux__)

#if defined(__aarch64__) || defined(__arm__)
    ASSERT_KEY("cpuImplementer");
    ASSERT_KEY("cpuArchitecture");
    ASSERT_KEY("cpuVariant");
    ASSERT_KEY("cpuPart");
    ASSERT_KEY("cpuRevision");
    ASSERT_KEY("glibc_rseq_present");
#endif

    ASSERT_KEY("mountInfo");

    BSONElement mountInfoArray = obj.getField("mountInfo");
    ASSERT_TRUE(mountInfoArray.type() == Array);
    int count = 0;
    BSONObjIterator it(mountInfoArray.Obj());
    while (it.more()) {
        BSONObj subobj = it.next().Obj();
        // Check for the last* field of /proc/diskstats
        //  *see linux kernel admin-guide/iostats.rst
        if (subobj.hasField("ioMsWeighted")) {
            count++;
        }
    }
    ASSERT_GREATER_THAN(count, 0);
#endif
}

TEST(ProcessInfo, GetNumAvailableCores) {
#if defined(__APPLE__) || defined(__linux__) || (defined(__sun) && defined(__SVR4)) || \
    defined(_WIN32)
    unsigned long numAvailCores = ProcessInfo::getNumAvailableCores();
    ASSERT_GREATER_THAN(numAvailCores, 0u);
    ASSERT_LESS_THAN_OR_EQUALS(numAvailCores, ProcessInfo::getNumLogicalCores());
#endif
}

TEST(ProcessInfo, GetNumCoresReturnsNonZeroNumberOfProcessors) {
    ASSERT_GREATER_THAN(ProcessInfo::getNumLogicalCores(), 0u);
}

#if defined(__linux__)
TEST(ProcessInfo, ReadTransparentHugePagesParameterInvalidDirectory) {
    StatusWith<std::string> result =
        ProcessInfo::readTransparentHugePagesParameter("no_such_directory", "param");
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NonExistentPath, result.getStatus().code());
}

TEST(ProcessInfo, ReadTransparentHugePagesParameterInvalidFile) {
    TempDir tempDir("ProcessInfo_ReadTransparentHugePagesParameterInvalidFile");
    StatusWith<std::string> result =
        ProcessInfo::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NonExistentPath, result.getStatus().code());
}

TEST(ProcessInfo, ReadTransparentHugePagesParameterEmptyFile) {
    TempDir tempDir("ProcessInfo_ReadTransparentHugePagesParameterInvalidFile");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream(filename.c_str());
    }
    StatusWith<std::string> result =
        ProcessInfo::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NonExistentPath, result.getStatus().code());
}

TEST(ProcessInfo, ReadTransparentHugePagesParameterBlankLine) {
    TempDir tempDir("ProcessInfo_ReadTransparentHugePagesParameterBlankLine");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << std::endl;
    }
    StatusWith<std::string> result =
        ProcessInfo::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NonExistentPath, result.getStatus().code());
}

TEST(ProcessInfo, ReadTransparentHugePagesParameterInvalidFormat) {
    TempDir tempDir("ProcessInfo_ReadTransparentHugePagesParameterBlankLine");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise never" << std::endl;
    }
    StatusWith<std::string> result =
        ProcessInfo::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST(ProcessInfo, ReadTransparentHugePagesParameterEmptyOpMode) {
    TempDir tempDir("ProcessInfo_ReadTransparentHugePagesParameterEmptyOpMode");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise [] never" << std::endl;
    }
    StatusWith<std::string> result =
        ProcessInfo::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
}

TEST(ProcessInfo, ReadTransparentHugePagesParameterUnrecognizedOpMode) {
    TempDir tempDir("ProcessInfo_ReadTransparentHugePagesParameterUnrecognizedOpMode");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise never [unknown]" << std::endl;
    }
    StatusWith<std::string> result =
        ProcessInfo::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
}

TEST(ProcessInfo, ReadTransparentHugePagesParameterValidFormat) {
    TempDir tempDir("ProcessInfo_ReadTransparentHugePagesParameterBlankLine");
    {
        std::string filename(tempDir.path() + "/param");
        std::ofstream ofs(filename.c_str());
        ofs << "always madvise [never]" << std::endl;
    }
    StatusWith<std::string> result =
        ProcessInfo::readTransparentHugePagesParameter("param", tempDir.path());
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS("never", result.getValue());
}
#endif  // __linux__

#if defined(__linux__) && defined(MONGO_CONFIG_GLIBC_RSEQ)
TEST(ProcessInfo, GLIBCRseqTunable) {
    using namespace fmt::literals;

    std::string glibcOriginalEnv("");
    if (auto res = getenv(ProcessInfo::kGlibcTunableEnvVar); res != nullptr) {
        glibcOriginalEnv = std::string(res);
    }

    ON_BLOCK_EXIT([&]() { setenv(ProcessInfo::kGlibcTunableEnvVar, glibcOriginalEnv.c_str(), 1); });

    auto checkRseqSetting = [&](const char* settingName, const char* setting, bool expectOK) {
        auto setting1 = "{}={}"_format(settingName, setting);
        setenv(ProcessInfo::kGlibcTunableEnvVar, setting1.c_str(), 1);
        auto res = ProcessInfo::checkGlibcRseqTunable();
        if (expectOK) {
            ASSERT(res);
        } else {
            ASSERT_FALSE(res);
        }
    };

    checkRseqSetting(ProcessInfo::kRseqKey, "0", true);
    checkRseqSetting(ProcessInfo::kRseqKey, "1", false);
    checkRseqSetting("", "", false);
    checkRseqSetting(ProcessInfo::kRseqKey, "a", false);
}
#endif
}  // namespace
}  // namespace mongo
