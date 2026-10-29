// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"

#include <string>
#include <vector>

namespace mongo {

inline Status chunkBoundsNotEmpty(const std::vector<BSONObj>& values) {
    if (values.size() < 3) {
        return {ErrorCodes::InvalidOptions,
                "need to provide at least three chunk boundaries for the chunks to be merged"};
    }
    return Status::OK();
}
}  // namespace mongo
