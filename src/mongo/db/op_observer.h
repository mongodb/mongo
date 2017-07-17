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
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/s/collection_sharding_state.h"

namespace mongo {
struct CollectionOptions;
struct InsertStatement;
class NamespaceString;
class OperationContext;

namespace repl {
class OpTime;
}  // repl

/**
 * Holds document update information used in logging.
 */
struct OplogUpdateEntryArgs {
    // Name of the collection in which document is being updated.
    NamespaceString nss;

    OptionalCollectionUUID uuid;

    StmtId stmtId = kUninitializedStmtId;

    // Fully updated document with damages (update modifiers) applied.
    BSONObj updatedDoc;

    // Document containing update modifiers -- e.g. $set and $unset
    BSONObj update;

    // Document containing the _id field of the doc being updated.
    BSONObj criteria;

    // True if this update comes from a chunk migration.
    bool fromMigrate;
};

struct TTLCollModInfo {
    Seconds expireAfterSeconds;
    Seconds oldExpireAfterSeconds;
    std::string indexName;
};

class OpObserver {
    MONGO_DISALLOW_COPYING(OpObserver);

public:
    OpObserver() = default;
    virtual ~OpObserver() = default;

    virtual void onCreateIndex(OperationContext* opCtx,
                               const NamespaceString& nss,
                               OptionalCollectionUUID uuid,
                               BSONObj indexDoc,
                               bool fromMigrate) = 0;
    virtual void onInserts(OperationContext* opCtx,
                           const NamespaceString& nss,
                           OptionalCollectionUUID uuid,
                           std::vector<InsertStatement>::const_iterator begin,
                           std::vector<InsertStatement>::const_iterator end,
                           bool fromMigrate) = 0;
    virtual void onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) = 0;
    virtual CollectionShardingState::DeleteState aboutToDelete(OperationContext* opCtx,
                                                               const NamespaceString& nss,
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
                          const NamespaceString& nss,
                          OptionalCollectionUUID uuid,
                          StmtId stmtId,
                          CollectionShardingState::DeleteState deleteState,
                          bool fromMigrate) = 0;
    virtual void onOpMessage(OperationContext* opCtx, const BSONObj& msgObj) = 0;
    virtual void onCreateCollection(OperationContext* opCtx,
                                    Collection* coll,
                                    const NamespaceString& collectionName,
                                    const CollectionOptions& options,
                                    const BSONObj& idIndex) = 0;
    /**
     * This function logs an oplog entry when a 'collMod' command on a collection is executed.
     * Since 'collMod' commands can take a variety of different formats, the 'o' field of the
     * oplog entry is populated with the 'collMod' command object. For TTL index updates, we
     * transform key pattern index specifications into index name specifications, for uniformity.
     * All other collMod fields are added to the 'o' object without modifications.
     *
     * To facilitate the rollback process, 'oldCollOptions' contains the previous state of all
     * collection options i.e. the state prior to completion of the current collMod command.
     * 'ttlInfo' contains the index name and previous expiration time of a TTL index. The old
     * collection options will be stored in the 'o2.collectionOptions_old' field, and the old TTL
     * expiration value in the 'o2.expireAfterSeconds_old' field.
     *
     * Oplog Entry Example ('o' and 'o2' fields shown):
     *
     *      {
     *          ...
     *          o: {
     *              collMod: "test",
     *              validationLevel: "off",
     *              index: {name: "indexName_1", expireAfterSeconds: 600}
     *          }
     *          o2: {
     *              collectionOptions_old: {
     *                  validationLevel: "strict",
     *              },
     *              expireAfterSeconds_old: 300
     *          }
     *      }
     *
     */
    virtual void onCollMod(OperationContext* opCtx,
                           const NamespaceString& nss,
                           OptionalCollectionUUID uuid,
                           const BSONObj& collModCmd,
                           const CollectionOptions& oldCollOptions,
                           boost::optional<TTLCollModInfo> ttlInfo) = 0;
    virtual void onDropDatabase(OperationContext* opCtx, const std::string& dbName) = 0;

    /**
     * This function logs an oplog entry when a 'drop' command on a collection is executed.
     * Returns the optime of the oplog entry successfully written to the oplog.
     * Returns a null optime if an oplog entry should not be written for this operation.
     */
    virtual repl::OpTime onDropCollection(OperationContext* opCtx,
                                          const NamespaceString& collectionName,
                                          OptionalCollectionUUID uuid) = 0;

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
                             const NamespaceString& nss,
                             OptionalCollectionUUID uuid,
                             const std::string& indexName,
                             const BSONObj& indexInfo) = 0;
    virtual void onRenameCollection(OperationContext* opCtx,
                                    const NamespaceString& fromCollection,
                                    const NamespaceString& toCollection,
                                    OptionalCollectionUUID uuid,
                                    bool dropTarget,
                                    OptionalCollectionUUID dropTargetUUID,
                                    OptionalCollectionUUID dropSourceUUID,
                                    bool stayTemp) = 0;
    virtual void onApplyOps(OperationContext* opCtx,
                            const std::string& dbName,
                            const BSONObj& applyOpCmd) = 0;
    virtual void onEmptyCapped(OperationContext* opCtx,
                               const NamespaceString& collectionName,
                               OptionalCollectionUUID uuid) = 0;
};

}  // namespace mongo
