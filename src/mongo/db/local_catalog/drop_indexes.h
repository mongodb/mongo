/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/local_catalog/ddl/drop_indexes_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
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
DropIndexesReply dropIndexes(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID>& expectedUUID,
                             const IndexArgument& index,
                             bool forceRawDataMode = false);

/**
 * Performs a dry-run validation of dropping indexes without actually dropping them.
 * Validates all the same constraints and throws the same errors as dropIndexes would.
 */
DropIndexesReply dropIndexesDryRun(OperationContext* opCtx,
                                   const NamespaceString& origNss,
                                   const boost::optional<UUID>& expectedUUID,
                                   const IndexArgument& origIndexArgument,
                                   const boost::optional<BSONObj>& shardKeyPattern,
                                   bool forceRawDataMode = false);

/**
 * Same behaviour as "dropIndexes" but only drops ready indexes.
 */
Status dropIndexesForApplyOps(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& cmdObj);

}  // namespace mongo
