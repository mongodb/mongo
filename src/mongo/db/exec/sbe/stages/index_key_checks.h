// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

namespace mongo::sbe {

/**
 * Call this function to produce any necessary diagnostic log messages when an index entry's record
 * id does not have a corresponding collection entry even though the entry lookup and document fetch
 * were performed on the same snapshot.
 *
 * Dangling index entries can occur normally when a document fetch is performed on a newer snapshot
 * than the one used for the index scan, so callers must check the snapshot id used for the fetch
 * and only call this function when it differs from the snapshot id used for the index scan.
 *
 * Dangling index entries can also occur normally as a result of prepared transaction conflicts, but
 * this function handles that case by checking the operation context to determine if prepared
 * transaction conflicts are permitted and only logging an error when they are not.
 */
void checkIndexKeyCorruption(OperationContext* opCtx,
                             value::SlotAccessor* indexKeyAccessor,
                             value::SlotAccessor* indexKeyPatternAccessor,
                             const RecordId& rid,
                             const NamespaceString& nss);

/**
 * Checks if an index key and document (read from 'nextRecord') are consistent with each other,
 * where consistency means that a search for the index key in the index would return the document
 * based on its current contents. Use this function when an index lookup and corresponding document
 * fetch are done on different snapshots to ensure that any modifications to the document in the
 * newer snapshot remain in bounds of the index scan.
 *
 * 'entryMap' caches IndexCatalogEntry lookups by index ident. Callers must clear it whenever the
 * plan yields, since the catalog entries may not survive a yield.
 */
bool isIndexKeyConsistent(OperationContext* opCtx,
                          StringMap<const IndexCatalogEntry*>& entryMap,
                          value::SlotAccessor* indexIdentAccessor,
                          value::SlotAccessor* indexKeyAccessor,
                          const CollectionPtr& collection,
                          const Record& nextRecord);

}  // namespace mongo::sbe
