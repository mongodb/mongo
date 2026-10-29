// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/index/index_access_method.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/update_index_data.h"
#include "mongo/util/modules.h"

namespace mongo::index_catalog_helpers {

/**
 * Populate the outData structure using the index keys found in the index definition.
 */
void computeUpdateIndexData(const IndexCatalogEntry* entry,
                            const IndexAccessMethod* accessMethod,
                            UpdateIndexData* outData);

}  // namespace mongo::index_catalog_helpers
