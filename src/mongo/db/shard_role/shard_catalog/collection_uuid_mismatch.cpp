// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch.h"

#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch_info.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/type_traits/decay.hpp>

namespace mongo {

void checkCollectionUUIDMismatch(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 const Collection* coll,
                                 const boost::optional<UUID>& uuid) {
    checkCollectionUUIDMismatch(opCtx, *CollectionCatalog::get(opCtx), ns, coll, uuid);
}

void checkCollectionUUIDMismatch(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 const CollectionPtr& coll,
                                 const boost::optional<UUID>& uuid) {
    checkCollectionUUIDMismatch(opCtx, *CollectionCatalog::get(opCtx), ns, coll.get(), uuid);
}

void checkCollectionUUIDMismatch(OperationContext* opCtx,
                                 const CollectionCatalog& catalog,
                                 const NamespaceString& ns,
                                 const CollectionPtr& coll,
                                 const boost::optional<UUID>& uuid) {
    checkCollectionUUIDMismatch(opCtx, catalog, ns, coll.get(), uuid);
}

void checkCollectionUUIDMismatch(OperationContext* opCtx,
                                 const CollectionCatalog& catalog,
                                 const NamespaceString& ns,
                                 const Collection* coll,
                                 const boost::optional<UUID>& uuid) {
    if (!uuid) {
        return;
    }
    // TODO SERVER-101784 Remove the code below once 9.0 becomes LTS and legacy time-series
    // collection are no more.
    auto nsForLogging = (ns.isTimeseriesBucketsCollection()) ? ns.getTimeseriesViewNamespace() : ns;
    auto actualNamespace = catalog.lookupNSSByUUID(opCtx, *uuid);
    uassert(
        (CollectionUUIDMismatchInfo{ns.dbName(),
                                    *uuid,
                                    std::string{nsForLogging.coll()},
                                    actualNamespace && actualNamespace->isEqualDb(ns)
                                        ? boost::make_optional(std::string{actualNamespace->coll()})
                                        : boost::none}),
        "Collection UUID does not match that specified",
        coll && coll->uuid() == *uuid);
}

}  // namespace mongo
