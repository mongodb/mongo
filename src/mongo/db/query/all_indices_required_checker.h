// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <map>
#include <string>

namespace mongo {

/**
 * A utility which, on construction, takes note of all indices for a given collection. The caller
 * can subsequently check whether any of those indices have been dropped.
 */
class AllIndicesRequiredChecker {
public:
    /**
     * Constructs an 'AllIndicesRequiredChecker' which can be used later to ensure that none of the
     * indices from 'collections' have been dropped. The caller must hold the appropriate db_raii
     * object in order to read the collection's index catalog.
     */
    explicit AllIndicesRequiredChecker(const MultipleCollectionAccessor& collections);

    /**
     * Throws a 'QueryPlanKilled' error if any of the indices which existed at the time of
     * construction have since been dropped.
     */
    void check(OperationContext* opCtx, const MultipleCollectionAccessor& collections) const;

private:
    void saveIndicesForCollection(const CollectionPtr& collection);

    void checkIndicesForCollection(OperationContext* opCtx, const CollectionPtr& collection) const;

    // This map of map holds index idents to all of the index catalog entries known at the time of
    // construction, grouped first by collection UUID then by index name. Later, we can attempt to
    // lookup the index entry by its ident in order to determine whether an index in the list has
    // been dropped.
    std::map<UUID, StringMap<std::string>> _identEntries;
};

}  // namespace mongo
