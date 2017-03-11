/**
 *    Copyright 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/s/collection_sharding_state.h"

namespace mongo {
struct CollectionOptions;
class NamespaceString;
class OperationContext;

/**
 * Holds document update information used in logging.
 */
struct OplogUpdateEntryArgs {
    // Name of the collection in which document is being updated.
    NamespaceString nss;

    // Fully updated document with damages (update modifiers) applied.
    BSONObj updatedDoc;

    // Document containing update modifiers -- e.g. $set and $unset
    BSONObj update;

    // Document containing the _id field of the doc being updated.
    BSONObj criteria;

    // True if this update comes from a chunk migration.
    bool fromMigrate;
};

class OpObserver {
    MONGO_DISALLOW_COPYING(OpObserver);

public:
    OpObserver() = default;
    virtual ~OpObserver() = default;

    virtual void onCreateIndex(OperationContext* opCtx,
                               const NamespaceString& ns,
                               BSONObj indexDoc,
                               bool fromMigrate) = 0;
    virtual void onInserts(OperationContext* opCtx,
                           const NamespaceString& ns,
                           std::vector<BSONObj>::const_iterator begin,
                           std::vector<BSONObj>::const_iterator end,
                           bool fromMigrate) = 0;
    virtual void onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) = 0;
    virtual CollectionShardingState::DeleteState aboutToDelete(OperationContext* opCtx,
                                                               const NamespaceString& ns,
                                                               const BSONObj& doc) = 0;
    /**
     * Handles logging before document is deleted.
     *
     * "ns" name of the collection from which deleteState.idDoc will be deleted.
     * "deleteState" holds information about the deleted document.
     * "fromMigrate" indicates whether the delete was induced by a chunk migration, and
     * so should be ignored by the user as an internal maintenance operation and not a
     * real delete.
     */
    virtual void onDelete(OperationContext* opCtx,
                          const NamespaceString& ns,
                          CollectionShardingState::DeleteState deleteState,
                          bool fromMigrate) = 0;
    virtual void onOpMessage(OperationContext* opCtx, const BSONObj& msgObj) = 0;
    virtual void onCreateCollection(OperationContext* opCtx,
                                    const NamespaceString& collectionName,
                                    const CollectionOptions& options,
                                    const BSONObj& idIndex) = 0;
    virtual void onCollMod(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const BSONObj& collModCmd) = 0;
    virtual void onDropDatabase(OperationContext* opCtx, const std::string& dbName) = 0;
    virtual void onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName) = 0;
    /**
     * This function logs an oplog entry when an index is dropped. The namespace of the index,
     * the index name, and the index info from the index descriptor are used to create a
     * 'dropIndexes' op where the 'o' field is the name of the index and the 'o2' field is the
     * index info. The index info can then be used to reconstruct the index on rollback.
     *
     * If a user specifies {dropIndexes: 'foo', index: '*'}, each index dropped will have its own
     * oplog entry. This means it's possible to roll back half of the index drops.
     */
    virtual void onDropIndex(OperationContext* opCtx,
                             const NamespaceString& ns,
                             const std::string& indexName,
                             const BSONObj& indexInfo) = 0;
    virtual void onRenameCollection(OperationContext* opCtx,
                                    const NamespaceString& fromCollection,
                                    const NamespaceString& toCollection,
                                    bool dropTarget,
                                    bool stayTemp) = 0;
    virtual void onApplyOps(OperationContext* opCtx,
                            const std::string& dbName,
                            const BSONObj& applyOpCmd) = 0;
    virtual void onEmptyCapped(OperationContext* opCtx, const NamespaceString& collectionName) = 0;
    virtual void onConvertToCapped(OperationContext* opCtx,
                                   const NamespaceString& collectionName,
                                   double size) = 0;
};

}  // namespace mongo
