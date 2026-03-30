/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/startup_check_rseq.h"

#include "mongo/base/parse_number.h"
#include "mongo/base/string_data.h"
#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/quick_exit.h"

#include <boost/optional.hpp>

#ifdef __linux__
#include <sys/utsname.h>
#endif

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
#include <tcmalloc/internal/percpu.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

bool isKernelVersionSafeForTCMallocPerCPUCache(StringData release) {
    int major = 0, minor = 0;
    char* end = nullptr;
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
    StringData release{u.release};
    if (!isKernelVersionSafeForTCMallocPerCPUCache(release)) {
        return false;
    }
#endif
    return true;
}

bool isTCMallocPerCPUCacheActive() {
#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
    return tcmalloc::tcmalloc_internal::subtle::percpu::IsFast();
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
