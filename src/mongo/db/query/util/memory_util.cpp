/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/db/query/util/memory_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/pcre.h"
#include "mongo/util/processinfo.h"
#include <cstddef>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::memory_util {

StatusWith<MemoryUnits> parseUnitString(const std::string& strUnit) {
    if (strUnit.empty()) {
        return Status(ErrorCodes::Error{6007010}, "Unit value cannot be empty");
    }

    if (strUnit[0] == '%') {
        return MemoryUnits::kPercent;
    } else if (strUnit[0] == 'M' || strUnit[0] == 'm') {
        return MemoryUnits::kMB;
    } else if (strUnit[0] == 'G' || strUnit[0] == 'g') {
        return MemoryUnits::kGB;
    }

    return Status(ErrorCodes::Error{6007011}, "Incorrect unit value");
}

StatusWith<MemorySize> MemorySize::parse(const std::string& str) {
    // Looks for a floating point number with followed by a unit suffix (MB, GB, %).
    static auto& re = *new pcre::Regex(R"re((?i)^\s*(\d+\.?\d*)\s*(MB|GB|%)\s*$)re");
    auto m = re.matchView(str);
    if (!m) {
        return {ErrorCodes::Error{6007012}, "Unable to parse memory size string"};
    }
    double size = std::stod(std::string{m[1]});
    std::string strUnit{m[2]};

    auto statusWithUnit = parseUnitString(strUnit);
    if (!statusWithUnit.isOK()) {
        return statusWithUnit.getStatus();
    }

    return MemorySize{size, statusWithUnit.getValue()};
}

size_t convertToSizeInBytes(const MemorySize& memSize) {
    constexpr size_t kBytesInMB = 1024 * 1024;
    constexpr size_t kMBytesInGB = 1024;

    double sizeInMB = memSize.size;

    switch (memSize.units) {
        case MemoryUnits::kPercent:
            sizeInMB *= ProcessInfo::getMemSizeMB() / 100.0;
            break;
        case MemoryUnits::kMB:
            break;
        case MemoryUnits::kGB:
            sizeInMB *= kMBytesInGB;
            break;
    }

    return static_cast<size_t>(sizeInMB * kBytesInMB);
}

size_t getRequestedMemSizeInBytes(const MemorySize& memSize) {
    size_t planCacheSize = convertToSizeInBytes(memSize);
    uassert(5968001,
            "Cache size must be at least 1KB * number of cores",
            planCacheSize >= 1024 * ProcessInfo::getNumCores());
    return planCacheSize;
}

/**
 * Sets upper limit on a storage structure's size. Either that structure's maximumSize or to
 * percentage of the total system's memory (both known at call site), whichever is smaller.
 */
size_t capMemorySize(size_t requestedSizeBytes,
                     size_t maximumSizeGB,
                     double percentTotalSystemMemory) {
    constexpr size_t kBytesInGB = 1024 * 1024 * 1024;
    // Express maximum size in bytes.
    const size_t maximumSizeBytes = maximumSizeGB * kBytesInGB;
    const memory_util::MemorySize limitToProcessSize{percentTotalSystemMemory,
                                                     memory_util::MemoryUnits::kPercent};
    const size_t limitToProcessSizeInBytes = convertToSizeInBytes(limitToProcessSize);

    // The size will be capped by the minimum of the two values defined above.
    const size_t upperLimit = std::min(maximumSizeBytes, limitToProcessSizeInBytes);

    if (requestedSizeBytes > upperLimit) {
        requestedSizeBytes = upperLimit;
    }
    return requestedSizeBytes;
}
}  // namespace mongo::memory_util
