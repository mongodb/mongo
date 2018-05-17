/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include <unordered_map>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/uuid.h"

namespace mongo {
/**
 * Class used for updating the UUID catalog on metadata operations.
 */
class UUIDCatalogObserver : public OpObserver {
public:
    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       OptionalCollectionUUID uuid,
                       BSONObj indexDoc,
                       bool fromMigrate) override {}
    void onInserts(OperationContext* opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   bool fromMigrate) override {}
    void onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) override {}
    void aboutToDelete(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const BSONObj& doc) override {}
    void onDelete(OperationContext* opCtx,
                  const NamespaceString& nss,
                  OptionalCollectionUUID uuid,
                  StmtId stmtId,
                  bool fromMigrate,
                  const boost::optional<BSONObj>& deletedDoc) override {}
    void onInternalOpMessage(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID> uuid,
                             const BSONObj& msgObj,
                             const boost::optional<BSONObj> o2MsgObj) override {}
    void onCreateCollection(OperationContext* opCtx,
                            Collection* coll,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex) override;
    void onCollMod(OperationContext* opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   const BSONObj& collModCmd,
                   const CollectionOptions& oldCollOptions,
                   boost::optional<TTLCollModInfo> ttlInfo) override;
    void onDropDatabase(OperationContext* opCtx, const std::string& dbName) override {}
    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  OptionalCollectionUUID uuid) override;
    void onDropIndex(OperationContext* opCtx,
                     const NamespaceString& nss,
                     OptionalCollectionUUID uuid,
                     const std::string& indexName,
                     const BSONObj& idxDescriptor) override {}
    void onRenameCollection(OperationContext* opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            OptionalCollectionUUID uuid,
                            OptionalCollectionUUID dropTargetUUID,
                            bool stayTemp) override;
    repl::OpTime preRenameCollection(OperationContext* opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     OptionalCollectionUUID uuid,
                                     OptionalCollectionUUID dropTargetUUID,
                                     bool stayTemp) override;
    void postRenameCollection(OperationContext* opCtx,
                              const NamespaceString& fromCollection,
                              const NamespaceString& toCollection,
                              OptionalCollectionUUID uuid,
                              OptionalCollectionUUID dropTargetUUID,
                              bool stayTemp) override;
    void onApplyOps(OperationContext* opCtx,
                    const std::string& dbName,
                    const BSONObj& applyOpCmd) override {}
    void onEmptyCapped(OperationContext* opCtx,
                       const NamespaceString& collectionName,
                       OptionalCollectionUUID uuid) override {}
    void onTransactionCommit(OperationContext* opCtx) override {}
    void onTransactionPrepare(OperationContext* opCtx) override {}
    void onTransactionAbort(OperationContext* opCtx) override {}
    void onReplicationRollback(OperationContext* opCtx,
                               const RollbackObserverInfo& rbInfo) override {}
};

/**
 * This class comprises a UUID to collection catalog, allowing for efficient
 * collection lookup by UUID.
 */
using CollectionUUID = UUID;
class Database;

class UUIDCatalog {
    MONGO_DISALLOW_COPYING(UUIDCatalog);

public:
    static UUIDCatalog& get(ServiceContext* svcCtx);
    static UUIDCatalog& get(OperationContext* opCtx);
    UUIDCatalog() = default;

    /**
     * This function inserts the entry for uuid, coll into the UUID Collection. It is called by
     * the op observer when a collection is created.
     */
    void onCreateCollection(OperationContext* opCtx, Collection* coll, CollectionUUID uuid);

    /**
     * This function removes the entry for uuid from the UUID catalog. It is called by the op
     * observer when a collection is dropped.
     */
    void onDropCollection(OperationContext* opCtx, CollectionUUID uuid);

    /**
     * This function atomically removes any existing entry for uuid from the UUID catalog and adds
     * a new entry for uuid associated with the Collection coll. It is called by the op observer
     * when a collection is renamed.
     */
    void onRenameCollection(OperationContext* opCtx, Collection* coll, CollectionUUID uuid);

    /**
     * Implies onDropCollection for all collections in db, but is not transactional.
     */
    void onCloseDatabase(Database* db);

    Collection* replaceUUIDCatalogEntry(CollectionUUID uuid, Collection* coll);
    void registerUUIDCatalogEntry(CollectionUUID uuid, Collection* coll);
    Collection* removeUUIDCatalogEntry(CollectionUUID uuid);

    /**
     * This function gets the Collection* pointer that corresponds to
     * CollectionUUID uuid. The required locks should be obtained prior
     * to calling this function, or else the found Collection pointer
     * might no longer be valid when the call returns.
     */
    Collection* lookupCollectionByUUID(CollectionUUID uuid) const;

    /**
     * This function gets the NamespaceString from the Collection* pointer that
     * corresponds to CollectionUUID uuid. If there is no such pointer, an empty
     * NamespaceString is returned. See onCloseCatalog/onOpenCatalog for more info.
     */
    NamespaceString lookupNSSByUUID(CollectionUUID uuid) const;

    /**
     * Puts the catalog in closed state. In this state, the lookupNSSByUUID method will fall back
     * to the pre-close state to resolve queries for currently unknown UUIDs. This allows
     * authorization, which needs to do lookups outside of database locks, to proceed.
     */
    void onCloseCatalog();

    /**
     * Puts the catatlog back in open state, removing the pre-close state. See onCloseCatalog.
     */
    void onOpenCatalog();

    /**
     * Return the UUID lexicographically preceding `uuid` in the database named by `db`.
     *
     * Return `boost::none` if `uuid` is not found, or is the first UUID in that database.
     */
    boost::optional<CollectionUUID> prev(const StringData& db, CollectionUUID uuid);

    /**
     * Return the UUID lexicographically following `uuid` in the database named by `db`.
     *
     * Return `boost::none` if `uuid` is not found, or is the last UUID in that database.
     */
    boost::optional<CollectionUUID> next(const StringData& db, CollectionUUID uuid);

private:
    const std::vector<CollectionUUID>& _getOrdering_inlock(const StringData& db,
                                                           const stdx::lock_guard<stdx::mutex>&);

    mutable mongo::stdx::mutex _catalogLock;
    /**
     * When present, indicates that the catalog is in closed state, and contains a map from UUID
     * to pre-close NSS. See also onCloseCatalog.
     */
    boost::optional<
        mongo::stdx::unordered_map<CollectionUUID, NamespaceString, CollectionUUID::Hash>>
        _shadowCatalog;

    /**
     * Map from database names to ordered `vector`s of their UUIDs.
     *
     * Works as a cache of such orderings: every ordering in this map is guaranteed to be valid, but
     * not all databases are guaranteed to have an ordering in it.
     */
    StringMap<std::vector<CollectionUUID>> _orderedCollections;
    mongo::stdx::unordered_map<CollectionUUID, Collection*, CollectionUUID::Hash> _catalog;
};

}  // namespace mongo
