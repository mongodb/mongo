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
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/rollback.h"
#include "mongo/db/s/collection_sharding_state.h"

namespace mongo {

struct InsertStatement;
class OperationContext;

namespace repl {
class OpTime;
}  // namespace repl

/**
 * Holds document update information used in logging.
 */
struct OplogUpdateEntryArgs {
    enum class StoreDocOption { None, PreImage, PostImage };

    // Name of the collection in which document is being updated.
    NamespaceString nss;

    OptionalCollectionUUID uuid;

    StmtId stmtId = kUninitializedStmtId;

    // The document before modifiers were applied.
    boost::optional<BSONObj> preImageDoc;

    // Fully updated document with damages (update modifiers) applied.
    BSONObj updatedDoc;

    // Document containing update modifiers -- e.g. $set and $unset
    BSONObj update;

    // Document containing the _id field of the doc being updated.
    BSONObj criteria;

    // True if this update comes from a chunk migration.
    bool fromMigrate = false;

    StoreDocOption storeDocOption = StoreDocOption::None;
};

struct TTLCollModInfo {
    Seconds expireAfterSeconds;
    Seconds oldExpireAfterSeconds;
    std::string indexName;
};

/**
 * The OpObserver interface contains methods that get called on certain database events. It provides
 * a way for various server subsystems to be notified of other events throughout the server.
 *
 * In order to call any OpObserver method, you must be in a 'WriteUnitOfWork'. This means that any
 * locks acquired for writes in that WUOW are still held. So, you can assume that any locks required
 * to perform the operation being observed are still held. These rules should apply for all observer
 * methods unless otherwise specified.
 */
class OpObserver {
public:
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
    virtual void aboutToDelete(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const BSONObj& doc) = 0;
    /**
     * Handles logging before document is deleted.
     *
     * "ns" name of the collection from which deleteState.idDoc will be deleted.
     * "fromMigrate" indicates whether the delete was induced by a chunk migration, and
     * so should be ignored by the user as an internal maintenance operation and not a
     * real delete.
     */
    virtual void onDelete(OperationContext* opCtx,
                          const NamespaceString& nss,
                          OptionalCollectionUUID uuid,
                          StmtId stmtId,
                          bool fromMigrate,
                          const boost::optional<BSONObj>& deletedDoc) = 0;
    /**
     * Logs a no-op with "msgObj" in the o field into oplog.
     *
     * This function should only be used internally. "nss", "uuid" and the o2 field should never be
     * exposed to users (for instance through the appendOplogNote command).
     */
    virtual void onInternalOpMessage(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const boost::optional<UUID> uuid,
                                     const BSONObj& msgObj,
                                     const boost::optional<BSONObj> o2MsgObj) = 0;

    /**
     * Logs a no-op with "msgObj" in the o field into oplog.
     */
    void onOpMessage(OperationContext* opCtx, const BSONObj& msgObj) {
        onInternalOpMessage(opCtx, {}, boost::none, msgObj, boost::none);
    }

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
     * Returns a null optime if an oplog entry was not written for this operation.
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

    /**
     * This function logs an oplog entry when a 'renameCollection' command on a collection is
     * executed. It should be used specifically in instances where the optime is necessary to
     * be obtained prior to performing the actual rename, and should only be used in conjunction
     * with postRenameCollection.
     * Returns the optime of the oplog entry successfully written to the oplog.
     * Returns a null optime if an oplog entry was not written for this operation.
     */
    virtual repl::OpTime preRenameCollection(OperationContext* opCtx,
                                             const NamespaceString& fromCollection,
                                             const NamespaceString& toCollection,
                                             OptionalCollectionUUID uuid,
                                             OptionalCollectionUUID dropTargetUUID,
                                             bool stayTemp) = 0;
    /**
     * This function performs all op observer handling for a 'renameCollection' command except for
     * logging the oplog entry. It should be used specifically in instances where the optime is
     * necessary to be obtained prior to performing the actual rename, and should only be used in
     * conjunction with preRenameCollection.
     */
    virtual void postRenameCollection(OperationContext* opCtx,
                                      const NamespaceString& fromCollection,
                                      const NamespaceString& toCollection,
                                      OptionalCollectionUUID uuid,
                                      OptionalCollectionUUID dropTargetUUID,
                                      bool stayTemp) = 0;
    /**
     * This function logs an oplog entry when a 'renameCollection' command on a collection is
     * executed. It calls preRenameCollection to log the entry and postRenameCollection to do all
     * other handling.
     */
    virtual void onRenameCollection(OperationContext* opCtx,
                                    const NamespaceString& fromCollection,
                                    const NamespaceString& toCollection,
                                    OptionalCollectionUUID uuid,
                                    OptionalCollectionUUID dropTargetUUID,
                                    bool stayTemp) = 0;

    virtual void onApplyOps(OperationContext* opCtx,
                            const std::string& dbName,
                            const BSONObj& applyOpCmd) = 0;
    virtual void onEmptyCapped(OperationContext* opCtx,
                               const NamespaceString& collectionName,
                               OptionalCollectionUUID uuid) = 0;
    /**
     * The onTransactionCommit method is called on the commit of an atomic transaction, before the
     * RecoveryUnit onCommit() is called.  It must not be called when no transaction is active.
     */
    virtual void onTransactionCommit(OperationContext* opCtx) = 0;

    /**
     * The onTransactionPrepare method is called when an atomic transaction is prepared. It must be
     * called when a transaction is active. It generates an OpTime and sets the prepare timestamp on
     * the recovery unit.
     * TODO: This is an incomplete implementation and should only be used for testing. It does not
     * write the prepare oplog entry, only generates an OpTime.
     */
    virtual void onTransactionPrepare(OperationContext* opCtx) = 0;

    /**
     * The onTransactionAbort method is called when an atomic transaction aborts, before the
     * RecoveryUnit onRollback() is called.  It must not be called when no transaction is active.
     */
    virtual void onTransactionAbort(OperationContext* opCtx) = 0;

    /**
     * A structure to hold information about a replication rollback suitable to be passed along to
     * any external subsystems that need to be notified of a rollback occurring.
     */
    struct RollbackObserverInfo {
        // A count of all oplog entries seen during rollback (even no-op entries).
        std::uint32_t numberOfEntriesObserved;

        // Set of all namespaces from ops being rolled back.
        std::set<NamespaceString> rollbackNamespaces = {};

        // Set of all session ids from ops being rolled back.
        std::set<UUID> rollbackSessionIds = {};

        // Maps UUIDs to a set of BSONObjs containing the _ids of the documents that will be deleted
        // from that collection due to rollback, and is used to populate rollback files.
        // For simplicity, this BSONObj set uses the simple binary comparison, as it is never wrong
        // to consider two _ids as distinct even if the collection default collation would put them
        // in the same equivalence class.
        stdx::unordered_map<UUID, SimpleBSONObjUnorderedSet, UUID::Hash> rollbackDeletedIdsMap;

        // True if the shard identity document was rolled back.
        bool shardIdentityRolledBack = false;

        // True if the config.version document was rolled back.
        bool configServerConfigVersionRolledBack = false;

        // Maps command names to a count of the number of those commands that are being rolled back.
        StringMap<std::uint32_t> rollbackCommandCounts;
    };

    /**
     * This function will get called after the replication system has completed a rollback. This
     * means that all on-disk, replicated data will have been reverted to the rollback common point
     * by the time this function is called. Subsystems may use this method to invalidate any in
     * memory caches or, optionally, rebuild any data structures from the data that is now on disk.
     * This function should not write any persistent state.
     *
     * When this function is called, there will be no locks held on the given OperationContext, and
     * it will not be called inside an existing WriteUnitOfWork. Any work done inside this handler
     * is expected to handle this on its own.
     *
     * This method is only applicable to the "rollback to a stable timestamp" algorithm, and is not
     * called when using any other rollback algorithm i.e "rollback via refetch".
     */
    virtual void onReplicationRollback(OperationContext* opCtx,
                                       const RollbackObserverInfo& rbInfo) = 0;

    struct Times;

protected:
    class ReservedTimes;
};

/**
 * This struct is a decoration for `OperationContext` which contains collected `repl::OpTime`
 * and `Date_t` timestamps of various critical stages of an operation performed by an OpObserver
 * chain.
 */
struct OpObserver::Times {
    static Times& get(OperationContext*);

    std::vector<repl::OpTime> reservedOpTimes;

private:
    friend OpObserver::ReservedTimes;

    // Because `OpObserver`s are re-entrant, it is necessary to track the recursion depth to know
    // when to actually clear the `reservedOpTimes` vector, using the `ReservedTimes` scope object.
    int _recursionDepth = 0;
};

/**
 * This class is an RAII object to manage the state of the `OpObserver::Times` decoration on an
 * operation context. Upon destruction the list of times in the decoration on the operation context
 * is cleared. It is intended for use as a scope object in `OpObserverRegistry` to manage
 * re-entrancy.
 */
class OpObserver::ReservedTimes {
    ReservedTimes(const ReservedTimes&) = delete;
    ReservedTimes& operator=(const ReservedTimes&) = delete;

public:
    explicit ReservedTimes(OperationContext* const opCtx);
    ~ReservedTimes();

    const Times& get() const {
        return _times;
    }

private:
    Times& _times;
};

}  // namespace mongo
