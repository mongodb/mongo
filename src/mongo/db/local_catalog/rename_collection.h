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
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/uuid.h"

#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class NamespaceString;
class OperationContext;

namespace repl {
class OpTime;
}  // namespace repl

/**
 * Renames the collection from "source" to "target" and drops the existing collection if
 * "dropTarget" is true. "stayTemp" indicates whether a collection should maintain its
 * temporariness. "newTargetCollectionUUID" is the UUID set to the final collection when renaming
 * across DBs (if not present, a random UUID will be assigned).
 */
struct RenameCollectionOptions {
    bool dropTarget = false;
    bool stayTemp = false;
    bool markFromMigrate = false;
    boost::optional<UUID> expectedSourceUUID;
    boost::optional<UUID> expectedTargetUUID;
    boost::optional<UUID> newTargetCollectionUuid;
    boost::optional<std::vector<BSONObj>> originalIndexes;
    boost::optional<BSONObj> originalCollectionOptions;
};

void doLocalRenameIfOptionsAndIndexesHaveNotChanged(OperationContext* opCtx,
                                                    const NamespaceString& sourceNs,
                                                    const NamespaceString& targetNs,
                                                    const RenameCollectionOptions& options);

/**
 * Checks that CollectionOptions 'expectedOptions' and 'currentOptions' are equal, except for the
 * 'uuid' field. Returns a CommandFailed status otherwise.
 * To be used by doLocalRenameIfOptionsAndIndexesHaveNotChanged and also its sharding-aware
 * equivalent in RenameCollectionCoordinator.
 */
Status checkTargetCollectionOptionsMatch(const NamespaceString& targetNss,
                                         const BSONObj& expectedOptions,
                                         const BSONObj& currentOptions);

/**
 * Checks that the lists of index specs 'expectedIndexes' and 'currentIndexes' are equal.
 * To be used by doLocalRenameIfOptionsAndIndexesHaveNotChanged and also its sharding-aware
 * equivalent in RenameCollectionCoordinator. Returns a CommandFailed status if indexes do not
 * match.
 */
Status checkTargetCollectionIndexesMatch(const NamespaceString& targetNss,
                                         const std::vector<BSONObj>& expectedIndexes,
                                         const std::vector<BSONObj>& currentIndexes);

Status renameCollection(OperationContext* opCtx,
                        const NamespaceString& source,
                        const NamespaceString& target,
                        const RenameCollectionOptions& options);

/**
 * As above, but may only be called from applyCommand_inlock. This allows creating a collection
 * with a specific UUID for cross-database renames.
 *
 * When 'cmd' contains dropTarget=true, 'renameOpTime' is used to rename the target collection to a
 * drop-pending collection.
 */
Status renameCollectionForApplyOps(OperationContext* opCtx,
                                   const boost::optional<UUID>& uuidToRename,
                                   const boost::optional<TenantId>& tid,
                                   const BSONObj& cmd,
                                   const repl::OpTime& renameOpTime);

/**
 * Same as renameCollection(), but used for rolling back renameCollection operations only.
 *
 * 'uuid' is used to look up the source namespace.
 * The 'target' namespace must refer to the same database as the source.
 */
Status renameCollectionForRollback(OperationContext* opCtx,
                                   const NamespaceString& target,
                                   const UUID& uuid);

/**
 * Performs validation checks to ensure source and target namespaces are eligible for rename.
 */
void validateNamespacesForRenameCollection(
    OperationContext* opCtx,
    const NamespaceString& source,
    const NamespaceString& target,
    const RenameCollectionOptions& options = RenameCollectionOptions());

/**
 * Runs renameCollection() with preliminary validation checks to ensure source
 * and target namespaces are eligible for rename.
 */
void validateAndRunRenameCollection(OperationContext* opCtx,
                                    const NamespaceString& source,
                                    const NamespaceString& target,
                                    const RenameCollectionOptions& options);

}  // namespace mongo
