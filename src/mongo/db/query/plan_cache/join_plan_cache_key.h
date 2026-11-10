// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace mongo {

/**
 * The key for a cached join plan is an opaque string representing the normalized join graph shape,
 * plus the query settings the query ran under. Query settings (e.g. index hints, query framework)
 * can change which join plan is chosen for the same shape, so including them in the key ensures
 * that a plan cached with one set of settings is not reused for a query runnning with different
 * settings.
 *
 * Hash and equality intentionally differ in how they treat query settings, aligning with the
 * classic PlanCacheKey. hash() ignores fields that don't affect plan selection (comment and
 * maxTimeMS), while operator== compares the whole QuerySettings object. Keys that differ only
 * in these fields will hash alike but are unequal, and get separate cache entries. This will
 * result in potential duplicate entries in the cache for the same plan.
 *
 * TODO (SERVER-138854): Align hash and equality to remove the duplicate entry caveat above.
 */
class JoinPlanCacheKey {
public:
    JoinPlanCacheKey(std::string shape, query_settings::QuerySettings querySettings);

    const std::string& shape() const {
        return _shape;
    }

    const query_settings::QuerySettings& querySettings() const {
        return _querySettings;
    }

    uint32_t hash() const {
        return _hash;
    }

    // Estimated size of the key in bytes for cache budget accounting.
    size_t keySizeInBytes() const {
        return _keySizeInBytes;
    }

    bool operator==(const JoinPlanCacheKey& other) const {
        return _hash == other._hash && _shape == other._shape &&
            _querySettings == other._querySettings;
    }

    friend std::ostream& operator<<(std::ostream& os, const JoinPlanCacheKey& key) {
        return os << key._shape;
    }

private:
    std::string _shape;
    query_settings::QuerySettings _querySettings;
    uint32_t _hash;
    size_t _keySizeInBytes;
};

/**
 * Builds a JoinPlanCacheKey from a fully-constructed join graph.
 *
 * Each node's contribution is a PlanCacheKeyInfo (match expression shape +
 * indexability discriminators for that node's collection), which ensures that
 * the key changes if the collection's index set changes eligibility for the query.
 *
 * 'resolvedPaths' maps PathId values in JoinEdge predicates to their concrete
 * FieldPath strings, this is produced by the PathResolver during join graph construction.
 *
 * 'collections' is used to look up the CollectionPtr for each node's collection so
 * that indexability discriminators can be computed.
 *
 * 'querySettings' is stored in the key; see JoinPlanCacheKey for how it affects hashing and
 * equality.
 */
JoinPlanCacheKey makeJoinPlanCacheKey(const join_ordering::JoinGraph& graph,
                                      const std::vector<join_ordering::ResolvedPath>& resolvedPaths,
                                      const MultipleCollectionAccessor& collections,
                                      const query_settings::QuerySettings& querySettings);

}  // namespace mongo
