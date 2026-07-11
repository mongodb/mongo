// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <boost/optional/optional.hpp>

namespace mongo {

[[MONGO_MOD_PUBLIC]]
void checkCollectionUUIDMismatch(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 const Collection* coll,
                                 const boost::optional<UUID>& uuid);

[[MONGO_MOD_PUBLIC]]
void checkCollectionUUIDMismatch(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 const CollectionPtr& coll,
                                 const boost::optional<UUID>& uuid);

/**
 * Same as above, but with the catalog passed explicitly.
 */
[[MONGO_MOD_PRIVATE]]
void checkCollectionUUIDMismatch(OperationContext* opCtx,
                                 const CollectionCatalog& catalog,
                                 const NamespaceString& ns,
                                 const Collection* coll,
                                 const boost::optional<UUID>& uuid);


[[MONGO_MOD_PRIVATE]]
void checkCollectionUUIDMismatch(OperationContext* opCtx,
                                 const CollectionCatalog& catalog,
                                 const NamespaceString& ns,
                                 const CollectionPtr& coll,
                                 const boost::optional<UUID>& uuid);

}  // namespace mongo
