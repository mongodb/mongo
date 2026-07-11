// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/ddl/create_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/virtual_collection_options.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Creates a collection as described in "cmdObj" on the database "dbName". Creates the collection's
 * _id index according to 'idIndex', if it is non-empty. When 'idIndex' is empty, creates the
 * default _id index.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status createCollection(OperationContext* opCtx,
                                                        const DatabaseName& dbName,
                                                        const BSONObj& cmdObj,
                                                        const BSONObj& idIndex = BSONObj());

/**
 * Creates a collection as parsed in 'cmd'.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status createCollection(OperationContext* opCtx,
                                                        const CreateCommand& cmd);

/**
 * Creates the collection or the view as described by 'options'.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status createCollection(OperationContext* opCtx,
                                                        const NamespaceString& ns,
                                                        const CollectionOptions& options,
                                                        const boost::optional<BSONObj>& idIndex);

/**
 * Creates a virtual collection as described by 'vopts'.
 */
[[MONGO_MOD_PRIVATE]] Status createVirtualCollection(OperationContext* opCtx,
                                                     const NamespaceString& ns,
                                                     const VirtualCollectionOptions& vopts);

/**
 * As above, but only used by replication to apply operations. This allows recreating collections
 * with specific UUIDs (if ui is given). If ui is given and and a collection exists with the same
 * name, the existing collection will be renamed to a temporary name if allowRenameOutOfTheWay is
 * true. This function will invariant if there is an existing collection with the same name and
 * allowRenameOutOfTheWay is false. If ui is not given, an existing collection will result in an
 * error.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status createCollectionForApplyOps(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const boost::optional<UUID>& ui,
    const BSONObj& cmdObj,
    bool allowRenameOutOfTheWay,
    const boost::optional<BSONObj>& idIndex = boost::none,
    const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier = boost::none,
    boost::optional<bool> recordIdsReplicated = boost::none);

/**
 * Updates collection options if collections must be clustered by default.
 */
[[MONGO_MOD_PRIVATE]] CollectionOptions translateOptionsIfClusterByDefault(
    const NamespaceString& nss,
    CollectionOptions collectionOptions,
    const boost::optional<BSONObj>& idIndex = boost::none);

/**
 * Check if we already have a collection or view compatible with the given create command.
 * In case of timeseries create command it also checks if the corresponding timeseries buckets
 * collection already exists and is compatible.
 *
 * Returns:
 *  - false: if no conflicting collection or view exists
 *  - true: if the namespace already exists and has same options
 *  - throws NamespaceExists error if a collection or view already exists with different options
 */
[[MONGO_MOD_PRIVATE]]
bool checkNamespaceAndTimeseriesBucketsAlreadyExistsAndCompatible(OperationContext* opCtx,
                                                                  const CreateCommand& cmd);
}  // namespace mongo
