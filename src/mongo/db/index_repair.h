// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];
namespace mongo::index_repair {
/**
 * Deletes the record containing a duplicate key and inserts it into a local lost and found
 * collection titled "local.lost_and_found.<original collection UUID>". Returns the size of the
 * record removed.
 */
StatusWith<int> moveRecordToLostAndFound(OperationContext* opCtx,
                                         const NamespaceString& ns,
                                         const NamespaceString& lostAndFoundNss,
                                         const RecordId& dupRecord);

/**
 * If repair mode is enabled, tries the inserting missingIndexEntry into indexes. If the
 * missingIndexEntry is a duplicate on a unique index, removes the duplicate document and keeps it
 * in a local lost and found collection.
 */
int repairMissingIndexEntry(OperationContext* opCtx,
                            const IndexCatalogEntry* index,
                            const key_string::Value& ks,
                            const KeyFormat& keyFormat,
                            const NamespaceString& nss,
                            const CollectionPtr& coll,
                            ValidateResults* results);
}  // namespace mongo::index_repair
