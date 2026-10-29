// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/ddl/drop_indexes_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
class BSONObj;
class BSONObjBuilder;
class NamespaceString;
class OperationContext;

using IndexArgument = std::variant<std::string, std::vector<std::string>, mongo::BSONObj>;

/**
 * Drops one or more ready indexes, or aborts a single index builder from the "nss" collection that
 * matches the caller's "index" input.
 *
 * The "index" field may be:
 * 1) "*" <-- Aborts all index builders and drops all ready indexes except the _id index.
 * 2) "indexName" <-- Aborts an index builder or drops a ready index with the given name.
 * 3) { keyPattern } <-- Aborts an index builder or drops a ready index with a matching key pattern.
 * 4) ["indexName1", ..., "indexNameN"] <-- Aborts an index builder or drops ready indexes that
 *                                          match the given names.
 *
 * TODO SERVER-102344 remove the forceRawDataMode once 9.0 becomes last LTS
 */
[[MONGO_MOD_PRIVATE]] DropIndexesReply dropIndexes(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const boost::optional<UUID>& expectedUUID,
                                                   const IndexArgument& index,
                                                   bool forceRawDataMode = false);

/**
 * Performs a dry-run validation of dropping indexes without actually dropping them.
 * Validates all the same constraints and throws the same errors as dropIndexes would.
 */
[[MONGO_MOD_PARENT_PRIVATE]] DropIndexesReply dropIndexesDryRun(
    OperationContext* opCtx,
    const NamespaceString& origNss,
    const boost::optional<UUID>& expectedUUID,
    const IndexArgument& origIndexArgument,
    const boost::optional<BSONObj>& shardKeyPattern,
    bool forceRawDataMode = false);

/**
 * Same behaviour as "dropIndexes" but only drops ready indexes.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status dropIndexesForApplyOps(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              const BSONObj& cmdObj);

}  // namespace mongo
