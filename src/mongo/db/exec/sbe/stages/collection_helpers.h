// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <cstdint>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
/**
 * A callback which gets called whenever a SCAN stage asks an underlying index scan for a result.
 */
using IndexKeyConsistencyCheckCallback = bool (*)(OperationContext* opCtx,
                                                  StringMap<const IndexCatalogEntry*>&,
                                                  value::SlotAccessor* snapshotIdAccessor,
                                                  value::SlotAccessor* indexIdentAccessor,
                                                  value::SlotAccessor* indexKeyAccessor,
                                                  const CollectionPtr& collection,
                                                  const Record& nextRecord);

using IndexKeyCorruptionCheckCallback = void (*)(OperationContext* opCtx,
                                                 value::SlotAccessor* snapshotIdAccessor,
                                                 value::SlotAccessor* indexKeyAccessor,
                                                 value::SlotAccessor* indexKeyPatternAccessor,
                                                 const RecordId& rid,
                                                 const NamespaceString& nss);

}  // namespace mongo::sbe
