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


#include <string>
#include <vector>

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/string_data.h"
#include "mongo/logv2/log.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
namespace {

class InterpolatedVersionInfo : public VersionInfoInterface {
public:
    int majorVersion() const final {
        return kMajorVersion;
    }

    int minorVersion() const final {
        return kMinorVersion;
    }

    int patchVersion() const final {
        return kPatchVersion;
    }

    int extraVersion() const final {
        return kExtraVersion;
    }

    StringData version() const final {
        return kVersion;
    }

    StringData gitVersion() const final {
        return kGitVersion;
    }

    std::vector<StringData> modules() const final {
        return modulesList;
    }

    StringData allocator() const final {
        return kAllocator;
    }

    StringData jsEngine() const final {
        return kJsEngine;
    }

    StringData targetMinOS() const final {
#if defined(_WIN32)
#if (NTDDI_VERSION >= NTDDI_WIN7)
        return "Windows 7/Windows Server 2008 R2";
#else
#error This targeted Windows version is not supported
#endif  // NTDDI_VERSION
#else
        LOGV2_FATAL(40277, "VersionInfoInterface::targetMinOS is only available for Windows");
#endif
    }

    std::vector<BuildInfoField> buildInfo() const final {
        return buildEnvironment;
    }

private:
    // clang-format off
    StringData kVersion = "@mongo_version@"_sd;
    int kMajorVersion = @mongo_version_major@;
    int kMinorVersion = @mongo_version_minor@;
    int kPatchVersion = @mongo_version_patch@;
    int kExtraVersion = @mongo_version_extra@;
    StringData kVersionExtraStr = "@mongo_version_extra_str@"_sd;
    StringData kGitVersion = "@mongo_git_hash@"_sd;
    StringData kAllocator = "@buildinfo_allocator@"_sd;
    StringData kJsEngine = "@buildinfo_js_engine@"_sd;
    std::vector<StringData> modulesList{@buildinfo_modules@};
    std::vector<VersionInfoInterface::BuildInfoField> buildEnvironment{@buildinfo_environment_data@};
    // clang-format on
};

const InterpolatedVersionInfo interpolatedVersionInfo;

MONGO_INITIALIZER_GENERAL(EnableVersionInfo, (), ("BeginStartupOptionRegistration"))
(InitializerContext*) {
    VersionInfoInterface::enable(&interpolatedVersionInfo);
}

}  // namespace
}  // namespace mongo
