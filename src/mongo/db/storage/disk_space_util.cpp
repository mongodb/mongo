// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/storage/disk_space_util.h"

#include <limits>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(simulateAvailableDiskSpace);
}

int64_t getAvailableDiskSpaceBytes(const boost::filesystem::path& path) {
    boost::system::error_code ec;
    boost::filesystem::space_info spaceInfo = boost::filesystem::space(path, ec);
    if (ec) {
        LOGV2_WARNING(7333403,
                      "Failed to query filesystem disk stats",
                      "error"_attr = ec.message(),
                      "errorCode"_attr = ec.value());
        // TODO(SERVER-121744): Change return value according to appropriate fallback.
        return std::numeric_limits<int64_t>::max();
    }
    return static_cast<int64_t>(spaceInfo.available);
}

int64_t getAvailableDiskSpaceBytesInDbPath(const boost::filesystem::path& dbpath) {
    if (auto fp = simulateAvailableDiskSpace.scoped(); fp.isActive()) {
        return static_cast<int64_t>(fp.getData()["bytes"].numberLong());
    }
    return getAvailableDiskSpaceBytes(dbpath);
}

}  // namespace mongo
