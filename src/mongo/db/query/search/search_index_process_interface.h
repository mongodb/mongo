// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Interface to separate router role and shard role implementations.
 */
class SearchIndexProcessInterface {
public:
    virtual ~SearchIndexProcessInterface() = default;

    static SearchIndexProcessInterface* get(Service* service);
    static SearchIndexProcessInterface* get(OperationContext* opCtx);

    static void set(Service* service, std::unique_ptr<SearchIndexProcessInterface> impl);

    /**
     * Returns the collection UUID and optionally a ResolvedView (if query is on a view). If no
     * UUID, throws a NamespaceNotFound error.
     */
    virtual std::pair<UUID, boost::optional<ResolvedNamespace>>
    fetchCollectionUUIDAndResolveViewOrThrow(OperationContext* opCtx,
                                             const NamespaceString& nss) = 0;
    /**
     * Returns the collection UUID (or boost::none if no collection is found) and optionally a
     * ResolvedNamespace if query is on a view (or boost::none if query is on a normal collection).
     *
     * Search related operations on timeseries collections may either fail or act as a no-op;
     * so the 'failOnTsColl' flag indicates which behavior is appropriate if the collection
     * ends up being timeseries upon lookup.
     */
    virtual std::pair<boost::optional<UUID>, boost::optional<ResolvedNamespace>>
    fetchCollectionUUIDAndResolveView(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      bool failOnTsColl = true) = 0;
};

}  // namespace mongo
