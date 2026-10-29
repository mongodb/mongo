// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/processinfo.h"

#include <fmt/core.h>

namespace mongo {
inline Status validateChunkMigrationFetcherMaxBufferedSizeBytesPerThread(
    const int& maxSize, const boost::optional<TenantId>&) {
    if (maxSize == 0) {
        return Status::OK();
    }

    if (maxSize < BSONObjMaxInternalSize) {
        return Status{ErrorCodes::InvalidOptions,
                      fmt::format("Chunk migration concurrency level must be 0 (no limit) or "
                                  "greater than or equal to {}.",
                                  BSONObjMaxInternalSize)};
    }
    return Status::OK();
}

}  // namespace mongo
