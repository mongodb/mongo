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
                       CollectionUUID uuid,
                       BSONObj indexDoc,
                       bool fromMigrate) override {}

    void onStartIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           CollectionUUID collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<BSONObj>& indexes,
                           bool fromMigrate) override {}

    void onCommitIndexBuild(OperationContext* opCtx,
                            const NamespaceString& nss,
                            CollectionUUID collUUID,
                            const UUID& indexBuildUUID,
                            const std::vector<BSONObj>& indexes,
                            bool fromMigrate) override {}

    void onAbortIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           CollectionUUID collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<BSONObj>& indexes,
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
                            const BSONObj& idIndex,
                            const OplogSlot& createOpTime) override {}
    void onCollMod(OperationContext* opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   const BSONObj& collModCmd,
                   const CollectionOptions& oldCollOptions,
                   boost::optional<TTLCollModInfo> ttlInfo) override;
    void onDropDatabase(OperationContext* opCtx, const std::string& dbName) override {}
    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  OptionalCollectionUUID uuid,
                                  std::uint64_t numRecords,
                                  CollectionDropType dropType) override;
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
                            std::uint64_t numRecords,
                            bool stayTemp) override {
        // Do nothing: collection renames don't affect the UUID mapping.
    }

    repl::OpTime preRenameCollection(OperationContext* opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     OptionalCollectionUUID uuid,
                                     OptionalCollectionUUID dropTargetUUID,
                                     std::uint64_t numRecords,
                                     bool stayTemp) override {
        // Do nothing: collection renames don't affect the UUID mapping.
        return {};
    }

    void postRenameCollection(OperationContext* opCtx,
                              const NamespaceString& fromCollection,
                              const NamespaceString& toCollection,
                              OptionalCollectionUUID uuid,
                              OptionalCollectionUUID dropTargetUUID,
                              bool stayTemp) override {
        // Do nothing: collection renames don't affect the UUID mapping.
    }

    void onApplyOps(OperationContext* opCtx,
                    const std::string& dbName,
                    const BSONObj& applyOpCmd) override {}
    void onEmptyCapped(OperationContext* opCtx,
                       const NamespaceString& collectionName,
                       OptionalCollectionUUID uuid) override {}
    void onUnpreparedTransactionCommit(
        OperationContext* opCtx, const std::vector<repl::ReplOperation>& statements) override {}
    void onPreparedTransactionCommit(
        OperationContext* opCtx,
        OplogSlot commitOplogEntryOpTime,
        Timestamp commitTimestamp,
        const std::vector<repl::ReplOperation>& statements) noexcept override {}
    void onTransactionPrepare(OperationContext* opCtx,
                              const std::vector<OplogSlot>& reservedSlots,
                              std::vector<repl::ReplOperation>& statements) override {}
    void onTransactionAbort(OperationContext* opCtx,
                            boost::optional<OplogSlot> abortOplogEntryOpTime) override {}
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
    class iterator {
    public:
        using value_type = Collection*;
        using pointer = const value_type*;
        using reference = const value_type&;

        iterator(StringData dbName, uint64_t genNum, const UUIDCatalog& uuidCatalog);
        iterator(
            std::map<std::pair<std::string, CollectionUUID>, Collection*>::const_iterator mapIter);
        pointer operator->();
        reference operator*();
        iterator operator++();
        iterator operator++(int);

        /*
         * Equality operators == and != do not attempt to reposition the iterators being compared.
         * The behavior for comparing invalid iterators is undefined.
         */
        bool operator==(const iterator& other);
        bool operator!=(const iterator& other);

    private:
        bool _repositionIfNeeded();
        bool _exhausted();

        std::string _dbName;
        boost::optional<CollectionUUID> _uuid;
        uint64_t _genNum;
        std::map<std::pair<std::string, CollectionUUID>, Collection*>::const_iterator _mapIter;
        const UUIDCatalog* _uuidCatalog;
        static constexpr Collection* _nullCollection = nullptr;
    };

    static UUIDCatalog& get(ServiceContext* svcCtx);
    static UUIDCatalog& get(OperationContext* opCtx);
    UUIDCatalog() = default;

    /**
     * This function inserts the entry for uuid, coll into the UUID Collection. It is called by
     * the op observer when a collection is created.
     */
    void onCreateCollection(OperationContext* opCtx,
                            std::unique_ptr<Collection> coll,
                            CollectionUUID uuid);

    /**
     * This function removes the entry for uuid from the UUID catalog. It is called by the op
     * observer when a collection is dropped.
     */
    void onDropCollection(OperationContext* opCtx, CollectionUUID uuid);

    /**
     * This function is responsible for safely setting the namespace string inside 'coll' to the
     * value of 'toCollection'. The caller need not hold locks on the collection.
     *
     * Must be called within a WriteUnitOfWork. The Collection namespace will be set back to
     * 'fromCollection' if the WriteUnitOfWork aborts.
     */
    void setCollectionNamespace(OperationContext* opCtx,
                                Collection* coll,
                                const NamespaceString& fromCollection,
                                const NamespaceString& toCollection);

    /**
     * Implies onDropCollection for all collections in db, but is not transactional.
     */
    void onCloseDatabase(Database* db);

    Collection* replaceUUIDCatalogEntry(CollectionUUID uuid, std::unique_ptr<Collection> coll);
    void registerUUIDCatalogEntry(CollectionUUID uuid, std::unique_ptr<Collection> coll);
    std::unique_ptr<Collection> removeUUIDCatalogEntry(CollectionUUID uuid);

    /**
     * This function gets the Collection pointer that corresponds to the CollectionUUID. The
     * required locks should be obtained prior to calling this function, or else the found
     * Collection pointer might no longer be valid when the call returns.
     *
     * Returns nullptr if the 'uuid' is not known.
     */
    Collection* lookupCollectionByUUID(CollectionUUID uuid) const;

    /**
     * This function gets the Collection pointer that corresponds to the NamespaceString. The
     * required locks should be obtained prior to calling this function, or else the found
     * Collection pointer may no longer be valid when the call returns.
     *
     * Returns nullptr if the namespace is unknown.
     */
    Collection* lookupCollectionByNamespace(const NamespaceString& nss) const;

    /**
     * This function gets the NamespaceString from the Collection* pointer that
     * corresponds to CollectionUUID uuid. If there is no such pointer, an empty
     * NamespaceString is returned. See onCloseCatalog/onOpenCatalog for more info.
     */
    NamespaceString lookupNSSByUUID(CollectionUUID uuid) const;

    /**
     * Puts the catalog in closed state. In this state, the lookupNSSByUUID method will fall back
     * to the pre-close state to resolve queries for currently unknown UUIDs. This allows processes,
     * like authorization and replication, which need to do lookups outside of database locks, to
     * proceed.
     *
     * Must be called with the global lock acquired in exclusive mode.
     */
    void onCloseCatalog(OperationContext* opCtx);

    /**
     * Puts the catatlog back in open state, removing the pre-close state. See onCloseCatalog.
     *
     * Must be called with the global lock acquired in exclusive mode.
     */
    void onOpenCatalog(OperationContext* opCtx);

    /**
     * Return the UUID lexicographically preceding `uuid` in the database named by `db`.
     *
     * Return `boost::none` if `uuid` is not found, or is the first UUID in that database.
     */
    boost::optional<CollectionUUID> prev(StringData db, CollectionUUID uuid);

    /**
     * Return the UUID lexicographically following `uuid` in the database named by `db`.
     *
     * Return `boost::none` if `uuid` is not found, or is the last UUID in that database.
     */
    boost::optional<CollectionUUID> next(StringData db, CollectionUUID uuid);

    iterator begin(StringData db) const;
    iterator end() const;

private:
    class FinishDropChange;

    const std::vector<CollectionUUID>& _getOrdering_inlock(const StringData& db,
                                                           const stdx::lock_guard<stdx::mutex>&);
    void _registerUUIDCatalogEntry_inlock(CollectionUUID uuid, std::unique_ptr<Collection> coll);
    std::unique_ptr<Collection> _removeUUIDCatalogEntry_inlock(CollectionUUID uuid);

    mutable mongo::stdx::mutex _catalogLock;
    /**
     * When present, indicates that the catalog is in closed state, and contains a map from UUID
     * to pre-close NSS. See also onCloseCatalog.
     */
    boost::optional<
        mongo::stdx::unordered_map<CollectionUUID, NamespaceString, CollectionUUID::Hash>>
        _shadowCatalog;

    /**
     * Unordered map from Collection UUID to the corresponding Collection object.
     */
    mongo::stdx::unordered_map<CollectionUUID, std::unique_ptr<Collection>, CollectionUUID::Hash>
        _catalog;

    /**
     * Ordered map from <database name, collection UUID> to a Collection object.
     */
    std::map<std::pair<std::string, CollectionUUID>, Collection*> _orderedCollections;

    mongo::stdx::unordered_map<NamespaceString, Collection*> _collections;
    /**
     * Generation number to track changes to the catalog that could invalidate iterators.
     */
    uint64_t _generationNumber;
};
}  // namespace mongo
