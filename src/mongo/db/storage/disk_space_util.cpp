/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/storage/disk_space_util.h"

#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"


#include <boost/filesystem.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(simulateAvailableDiskSpace);
}

int64_t getAvailableDiskSpaceBytes(const std::string& path) {
    boost::filesystem::path fsPath(path);
    boost::system::error_code ec;
    boost::filesystem::space_info spaceInfo = boost::filesystem::space(fsPath, ec);
    if (ec) {
        LOGV2(7333403,
              "Failed to query filesystem disk stats",
              "error"_attr = ec.message(),
              "errorCode"_attr = ec.value());
        // We don't want callers to take any action if we can't collect stats.
        return std::numeric_limits<int64_t>::max();
    }
    return static_cast<int64_t>(spaceInfo.available);
}

int64_t getAvailableDiskSpaceBytesInDbPath() {
    if (auto fp = simulateAvailableDiskSpace.scoped(); fp.isActive()) {
        return static_cast<int64_t>(fp.getData()["bytes"].numberLong());
    }
    return getAvailableDiskSpaceBytes(storageGlobalParams.dbpath);
}

}  // namespace mongo
