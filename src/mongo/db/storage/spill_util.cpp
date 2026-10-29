// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/storage/spill_util.h"

#include "mongo/db/storage/disk_space_util.h"
#include "mongo/util/str.h"

namespace mongo {

Status ensureSufficientDiskSpaceForSpilling(const boost::filesystem::path& dbpath,
                                            int64_t minRequired) {
    int64_t available = getAvailableDiskSpaceBytesInDbPath(dbpath);
    if (available < minRequired) {
        return Status(ErrorCodes::OutOfDiskSpace,
                      str::stream() << "Available space of " << available
                                    << " bytes is less than the required minimum of " << minRequired
                                    << " bytes");
    }
    return Status::OK();
}

}  // namespace mongo
