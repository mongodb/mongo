// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
