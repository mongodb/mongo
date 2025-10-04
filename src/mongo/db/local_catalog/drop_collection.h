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
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/ddl/drop_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/uuid.h"

#include <boost/optional/optional.hpp>

namespace mongo {
class NamespaceString;
class OperationContext;

namespace repl {
class OpTime;
}  // namespace repl

enum class DropCollectionSystemCollectionMode {
    kDisallowSystemCollectionDrops,
    kAllowSystemCollectionDrops
};

/**
 * Drops the collection "collectionName" and populates "reply" with statistics about what
 * was removed. Aborts in-progress index builds on the collection if two phase index builds are
 * supported. Throws if the expectedUUID does not match the UUID of the collection being dropped.
 * When fromMigrate is set, the related oplog entry will be marked accordingly using the
 * 'fromMigrate' field to reduce its visibility (e.g. in change streams).
 */
Status dropCollection(OperationContext* opCtx,
                      const NamespaceString& collectionName,
                      const boost::optional<UUID>& expectedUUID,
                      DropReply* reply,
                      DropCollectionSystemCollectionMode systemCollectionMode,
                      bool fromMigrate = false);

Status dropCollection(OperationContext* opCtx,
                      const NamespaceString& collectionName,
                      DropReply* reply,
                      DropCollectionSystemCollectionMode systemCollectionMode,
                      bool fromMigrate = false);

/**
 * Drops the collection with the given namespace only if its uuid is not matching 'expectedUUID'.
 * When 'fromMigrate' is set, the related oplog entry will be marked accordingly using the
 * 'fromMigrate' field to reduce its visibility (e.g. in change streams).
 */
Status dropCollectionIfUUIDNotMatching(OperationContext* opCtx,
                                       const NamespaceString& ns,
                                       const UUID& expectedUUID,
                                       bool fromMigrate);

/**
 * Drops the collection "collectionName". When applying a 'drop' oplog entry on a secondary, the
 * 'dropOpTime' will contain the optime of the oplog entry.
 */
Status dropCollectionForApplyOps(OperationContext* opCtx,
                                 const NamespaceString& collectionName,
                                 const repl::OpTime& dropOpTime,
                                 DropCollectionSystemCollectionMode systemCollectionMode);

/**
 * If we are in a replset, every replicated collection must have an _id index. Issues a warning if
 * one is not found.
 *
 * The caller must have the database locked in X mode.
 */
void checkForIdIndexes(OperationContext* opCtx, const DatabaseName& dbName);

/**
 * Deletes all temporary collections under the specified database.
 *
 * The caller must have the database locked in at least IX mode.
 */
void clearTempCollections(OperationContext* opCtx, const DatabaseName& dbName);

/**
 * Checks that the namespace complies with naming restrictions and therefore can be dropped. It
 * returns a Status with details of that evaluation.
 */
Status isDroppableCollection(OperationContext* opCtx, const NamespaceString& nss);

}  // namespace mongo
