// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/startup_check_rseq.h"

#include "mongo/base/parse_number.h"
#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/str.h"

#include <array>
#include <fstream>
#include <string_view>

#include <boost/optional.hpp>

#ifdef __linux__
#include <sys/utsname.h>
#endif

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
#include <tcmalloc/malloc_extension.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

bool isKernelVersionSafeForTCMallocPerCPUCache(std::string_view release) {
    std::array<int, 3> version;
    const char* end = nullptr;
    if (!NumberParser::strToAny(10)(release, &version[0], &end).isOK() || *end != '.' ||
        !NumberParser::strToAny(10)(end + 1, &version[1], &end).isOK() || *end != '.' ||
        !NumberParser::strToAny(10)(end + 1, &version[2]).isOK()) {
        // If the version cannot be parsed, assume the kernel is compatible
        LOGV2_WARNING(12257601,
                      "Unable to parse kernel version, cannot check for kernel "
                      "version compatibility",
                      "kernel-version"_attr = release);
        return true;
    }

    return version < std::array{6, 19, 0} || std::array{7, 0, 13} < version;
}

boost::optional<std::string_view> ubuntuKernelVersionFromVersionSignature(
    std::string_view versionSignature) {
    // The format of version_signature looks like this:
    // Ubuntu 6.5.0-1022.22~22.04.1-aws 6.5.13
    // It is specified to be "Ubuntu {KERNEL} {UPSTREAM-VERSION}"
    auto prefix = std::string_view{"Ubuntu "};

    if (!versionSignature.starts_with(prefix)) {
        return {};
    }

    auto prefixRemoved = versionSignature.substr(prefix.size());
    auto spaceBeforeVersion = prefixRemoved.find(' ');
    if (spaceBeforeVersion == std::string_view::npos) {
        return {};
    }

    return prefixRemoved.substr(spaceBeforeVersion + 1);
}

namespace {

#ifdef __linux__
boost::optional<std::string> ubuntuKernelVersion() {
    auto versionSignatureFile = std::ifstream{"/proc/version_signature"};
    std::string versionSignature;

    // The version information is only on the first line.
    if (!std::getline(versionSignatureFile, versionSignature)) {
        return {};
    }

    if (auto ubuntuKernelVersion = ubuntuKernelVersionFromVersionSignature(versionSignature)) {
        return std::string{*ubuntuKernelVersion};
    }

    return {};
}

boost::optional<std::string> kernelVersion() {
    struct utsname unameResult;
    if (uname(&unameResult) != 0) {
        return {};
    }

    // If the kernel version does not start with 7.0, the patch version is unimportant.
    // The Ubuntu specific way to get the patch version is therefore unneeded.
    if (!std::string_view{unameResult.release}.starts_with("7.0")) {
        return std::string{unameResult.release};
    }

    if (str::contains(unameResult.version, "Ubuntu")) {
        return ubuntuKernelVersion().value_or(unameResult.release);
    }

    return std::string{unameResult.release};
}
#endif

bool isKernelSafeForTCMallocPerCPUCache() {
#ifdef __linux__
    if (auto version = kernelVersion()) {
        return isKernelVersionSafeForTCMallocPerCPUCache(*version);
    }

    LOGV2_WARNING(12257602,
                  "Unable to determine kernel version via uname, cannot check for kernel "
                  "version compatibility");
#endif
    return true;
}

bool isTCMallocPerCPUCacheActive() {
#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
    return tcmalloc::MallocExtension::PerCpuCachesActive();
#else
    return false;
#endif
}

}  // namespace

void validateRseqKernelCompat() {
    if (isTCMallocPerCPUCacheActive() && !isKernelSafeForTCMallocPerCPUCache()) {
        LOGV2_FATAL_OPTIONS(
            12257600,
            logv2::LogOptions(logv2::LogComponent::kControl, logv2::FatalMode::kContinue),
            "MongoDB cannot start: Linux kernel versions 6.19.0 through 7.0.13 have a known "
            "incompatibility with this version of MongoDB. See "
            "https://jira.mongodb.org/browse/SERVER-121912 for more information.");
        quickExit(ExitCode::fail);
    }
}

}  // namespace mongo
