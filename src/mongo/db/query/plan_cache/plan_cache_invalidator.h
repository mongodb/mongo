// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstddef>

#include <boost/optional/optional.hpp>

namespace mongo {
/**
 * Controls life-time of PlanCache entries associated with a particular collection of a particular
 * version. A new copy of the collection is created each time the collection
 * changes (copy-on-write policy). The collection version is incremented only after changes that
 * require invalidating the plan cache (for example, creating an index or deleting the collection).
 * If the catalog change does not require invalidation of plan cache entries (for example, changing
 * the document validator), then the collection version remains unchanged.
 */
class PlanCacheInvalidator {
public:
    PlanCacheInvalidator() = default;
    PlanCacheInvalidator(const Collection* collection, ServiceContext* serviceContext);

    ~PlanCacheInvalidator();

    /**
     * Forces SBE PlanCache invalidation for the collection UUID and version stored in this
     * invalidator.
     */
    void clearPlanCache() const;

    size_t versionNumber() const {
        return _version;
    }

private:
    // "Version" of the collection, increased any time we need to invalidate PlanCache.
    const size_t _version{};

    // The collection's UUID.
    const boost::optional<UUID> _uuid{};

    ServiceContext* const _serviceContext{};
};
}  // namespace mongo
