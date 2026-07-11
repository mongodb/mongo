// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Attempts to populate the actualCollection field of a CollectionUUIDMismatch error (if not already
 * populated) by contacting the primary shard. Returns the populated CollectionUUIDMismatch or the
 * error that caused attempting to populate the Status to fail. Will never return an OK Status.
 *
 * With the introduction of the Router and Shard Roles, nobody outside of the catalog modules should
 * be concerned with collection UUID checking.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]]
Status populateCollectionUUIDMismatch(OperationContext* opCtx,
                                      const Status& collectionUUIDMismatch);

}  // namespace mongo
