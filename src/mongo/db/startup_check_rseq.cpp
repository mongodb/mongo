// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/startup_check_rseq.h"

#include "mongo/base/parse_number.h"
#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/quick_exit.h"

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
    int major = 0, minor = 0;
    const char* end = nullptr;
    if (!NumberParser::strToAny(10)(release, &major, &end).isOK() || *end != '.' ||
        !NumberParser::strToAny(10)(end + 1, &minor).isOK()) {
        // If the version cannot be parsed, assume the kernel is compatible
        LOGV2_WARNING(12257601,
                      "Unable to parse kernel version, cannot check for kernel "
                      "version compatibility",
                      "kernel-version"_attr = release);
        return true;
    }
    return major < 6 || (major == 6 && minor < 19);
}

namespace {

bool isKernelSafeForTCMallocPerCPUCache() {
#ifdef __linux__
    struct utsname u;
    if (uname(&u) != 0) {
        LOGV2_WARNING(12257602,
                      "Unable to determine kernel version via uname, cannot check for kernel "
                      "version compatibility");
        return true;
    }
    std::string_view release{u.release};
    if (!isKernelVersionSafeForTCMallocPerCPUCache(release)) {
        return false;
    }
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
            "MongoDB cannot start: Linux kernel versions 6.19 and newer has a known "
            "incompatibility with this version of MongoDB. See "
            "https://jira.mongodb.org/browse/SERVER-121912 for more information.");
        quickExit(ExitCode::fail);
    }
}

}  // namespace mongo
