// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/modules.h"

namespace mongo {

class MigrationSecondaryThrottleOptions;
template <typename T>
class StatusWith;
class OperationContext;

/**
 * Returns the default write concern for migration cleanup on the donor shard and for cloning
 * documents on the destination shard.
 */
class ChunkMoveWriteConcernOptions {
public:
    /**
     * Based on the type of the server (standalone or replica set) and the requested secondary
     * throttle options returns what write concern should be used locally both for writing migrated
     * documents and for performing range deletions.
     *
     * Returns a non-OK status if the requested write concern cannot be satisfied for some reason.
     *
     * These are the rules for determining the local write concern to be used:
     *  - secondaryThrottle is not specified (kDefault) or it is on (kOn), but there is no custom
     *    write concern:
     *      - if replication is enabled and there are 2 or more nodes - w:2, j:false, timeout:60000
     *      - if replication is not enabled or less than 2 nodes - w:1, j:false, timeout:0
     *  - secondaryThrottle is off (kOff): w:1, j:false, timeout:0
     *  - secondaryThrottle is on (kOn) and there is custom write concern, use the custom write
     *    concern.
     */
    static StatusWith<WriteConcernOptions> getEffectiveWriteConcern(
        OperationContext* opCtx, const MigrationSecondaryThrottleOptions& options);
};

}  // namespace mongo
