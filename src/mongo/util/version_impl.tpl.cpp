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


#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/version.h"

#include <string>
#include <string_view>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
namespace {
    using namespace std::literals::string_view_literals;

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

    std::string_view version() const final {
        return kVersion;
    }

    std::string_view gitVersion() const final {
        return kGitVersion;
    }

    std::vector<std::string_view> modules() const final {
        return modulesList;
    }

    std::string_view allocator() const final {
        return kAllocator;
    }

    std::string_view jsEngine() const final {
        return kJsEngine;
    }

    std::string_view targetMinOS() const final {
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
    std::string_view kVersion = "@mongo_version@"sv;
    int kMajorVersion = @mongo_version_major@;
    int kMinorVersion = @mongo_version_minor@;
    int kPatchVersion = @mongo_version_patch@;
    int kExtraVersion = @mongo_version_extra@;
    std::string_view kVersionExtraStr = "@mongo_version_extra_str@"sv;
    std::string_view kGitVersion = "@mongo_git_hash@"sv;
    std::string_view kAllocator = "@buildinfo_allocator@"sv;
    std::string_view kJsEngine = "@buildinfo_js_engine@"sv;
    std::vector<std::string_view> modulesList{@buildinfo_modules@};
    std::vector<VersionInfoInterface::BuildInfoField> buildEnvironment{@buildinfo_environment_data@};
    // clang-format on
};

bool enableVersionInfoDummy = [] {
    static StaticImmortal<InterpolatedVersionInfo> obj{};
    VersionInfoInterface::enable(&*obj);
    return false;
}();

}  // namespace
}  // namespace mongo
