// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"

namespace mongo::collection_internal {

bool shouldDeferCappedDeletesToOplogApplication(OperationContext* opCtx,
                                                const CollectionPtr& collection);

/**
 * If the collection is capped and the current data size or number of records exceeds cappedMaxSize
 * or cappedMaxDocs respectively, this method will block and delete as many documents as necessary
 * in order to bring it back to under that confuguration.
 *
 * Generates oplog entries for the deleted records in FCV >= 5.0.
 */
void cappedDeleteUntilBelowConfiguredMaximum(OperationContext* opCtx,
                                             const CollectionPtr& collection,
                                             const RecordId& justInserted,
                                             OpDebug* opDebug);

}  // namespace mongo::collection_internal
