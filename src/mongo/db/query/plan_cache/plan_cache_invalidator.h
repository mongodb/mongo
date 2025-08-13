/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/service_context.h"
#include "mongo/util/uuid.h"

#include <cstddef>

#include <boost/move/utility_core.hpp>
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
