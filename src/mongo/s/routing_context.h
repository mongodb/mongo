/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/util/assert_util.h"

namespace mongo {
/**
 * RoutingContext provides a structured interface for accessing routing table information via the
 * CatalogCache during a routing operation. It should be instantiated at the start of a query
 * routing operation to safely acquire routing information for a list of namespaces. The routing
 * table data is immutable once the context is created. Its lifetime spans the duration of the
 * operation and must persist until the routing tables have been validated by issuing a versioned
 * request to a shard.
 */
class RoutingContext {
public:
    // TODO SERVER-102931: Integrate the RouterAcquisitionSnapshot
    RoutingContext(OperationContext* opCtx,
                   const std::vector<NamespaceString>&
                       nssList,  // list of required namespaces for the routing operation
                   bool allowLocks = false);

    // TODO SERVER-102931: Integrate the RouterAcquisitionSnapshot
    /**
     * Constructs a RoutingContext from pre-acquired routing tables.
     */
    RoutingContext(OperationContext* opCtx,
                   const stdx::unordered_map<NamespaceString, CollectionRoutingInfo>& nssToCriMap);

    // Non-copyable, non-movable
    RoutingContext(const RoutingContext&) = delete;
    RoutingContext& operator=(const RoutingContext&) = delete;

    ~RoutingContext();

    /**
     * Create a RoutingContext without using a CatalogCache, for testing
     */
    static RoutingContext createForTest(
        stdx::unordered_map<NamespaceString, CollectionRoutingInfo> nssMap);

    /**
     * Returns the routing table mapped to a namespace. This is initialized at construction time
     * and remains the same throughout the routing operation.
     */
    const CollectionRoutingInfo& getCollectionRoutingInfo(const NamespaceString& nss) const;

    /**
     * Record that a versioned request for a namespace was accepted by a shard. The namespace is
     * considered validated.
     */
    void onResponseReceivedForNss(const NamespaceString& nss, const Status& status);

    /**
     * Utility to trigger CatalogCache refreshes for staleness errors directly from the
     * RoutingContext.
     */
    bool onStaleError(const NamespaceString& nss, const Status& status);

private:
    RoutingContext(stdx::unordered_map<NamespaceString, CollectionRoutingInfo> nssMap);
    /**
     * Obtain the routing table associated with a namespace from the CatalogCache. This returns the
     * latest routing table unless the read concern is snapshot with "atClusterTime" set. If an
     * error occurs, this returns a failed status.
     */
    StatusWith<CollectionRoutingInfo> _getCollectionRoutingInfo(OperationContext* opCtx,
                                                                const NamespaceString& nss,
                                                                bool allowLocks) const;

    CatalogCache* _catalogCache;
    // Map from _nss -> (CollectionRoutingInfo, Status).
    using NssCriMap =
        stdx::unordered_map<NamespaceString,
                            std::pair<const CollectionRoutingInfo, boost::optional<Status>>>;
    NssCriMap _nssToCriMap;
};
}  // namespace mongo
