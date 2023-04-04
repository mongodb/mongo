/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include <fmt/core.h>

#include "mongo/base/status.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/processinfo.h"

namespace mongo {

inline Status validateChunkMigrationConcurrency(const int& chunkMigrationConcurrency,
                                                const boost::optional<TenantId>&) {
    const int maxConcurrency = 500;
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (!mongo::feature_flags::gConcurrencyInChunkMigration.isEnabledAndIgnoreFCVUnsafe()) {
        return Status{ErrorCodes::InvalidOptions,
                      "Cannot set migration concurrency number without enabling migration "
                      "concurrency feature flag"};
    }

    if (chunkMigrationConcurrency <= 0 ||
        (chunkMigrationConcurrency > maxConcurrency && !getTestCommandsEnabled())) {
        return Status{
            ErrorCodes::InvalidOptions,
            fmt::format("Chunk migration concurrency level must be positive and less than {}.",
                        maxConcurrency)};
    }
    return Status::OK();
}

}  // namespace mongo
