// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/traffic_recorder_validators.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/str.h"

#include <boost/filesystem/operations.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

Status validateTrafficRecordDestination(const std::string& path, const boost::optional<TenantId>&) {
    if (!path.empty() && !boost::filesystem::is_directory(path)) {
        return Status(ErrorCodes::FileNotOpen,
                      str::stream()
                          << "traffic recording directory \"" << path << "\" is not a directory.");
    }

    return Status::OK();
}

}  // namespace mongo
