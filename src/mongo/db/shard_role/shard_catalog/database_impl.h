// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbcommands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/virtual_collection_options.h"
#include "mongo/util/modules.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_PRIVATE]] DatabaseImpl final : public Database {
public:
    explicit DatabaseImpl(const DatabaseName& dbName);

    void init(OperationContext*) final;

    const DatabaseName& name() const final {
        return _name;
    }

    void getStats(OperationContext* opCtx,
                  DBStats* output,
                  bool includeFreeStorage,
                  double scale = 1) const final;

    /**
     * dropCollection() will refuse to drop system collections. Use dropCollectionEvenIfSystem() if
     * that is required.
     *
     * If we are applying a 'drop' oplog entry on a secondary, 'dropOpTime' will contain the optime
     * of the oplog entry.
     *
     * When fromMigrate is set, the related oplog entry will be marked with a 'fromMigrate' field to
     * reduce its visibility (e.g. in change streams).
     *
     * The caller should hold a DB IX lock and ensure there are no index builds in progress on the
     * collection.
     */
    Status dropCollection(OperationContext* opCtx,
                          NamespaceString nss,
                          repl::OpTime dropOpTime,
                          bool markFromMigrate) const final;
    Status dropCollectionEvenIfSystem(OperationContext* opCtx,
                                      NamespaceString nss,
                                      repl::OpTime dropOpTime,
                                      bool markFromMigrate) const final;

    Status dropView(OperationContext* opCtx, NamespaceString viewName) const final;

    Status userCreateNS(OperationContext* opCtx,
                        const NamespaceString& nss,
                        CollectionOptions collectionOptions,
                        bool createDefaultIndexes,
                        const BSONObj& idIndex,
                        bool fromMigrate,
                        const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier,
                        boost::optional<bool> recordIdsReplicated) const final;

    Status userCreateVirtualNS(OperationContext* opCtx,
                               const NamespaceString& fullns,
                               CollectionOptions opts,
                               const VirtualCollectionOptions& vopts) const final;

    Collection* createCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptions& options = CollectionOptions(),
        bool createDefaultIndexes = true,
        const BSONObj& idIndex = BSONObj(),
        bool fromMigrate = false,
        boost::optional<bool> recordIdsReplicated = boost::none) const final;

    StatusWith<std::unique_ptr<CollatorInterface>> validateCollator(
        OperationContext* opCtx, CollectionOptions& opts) const final;

    Status renameCollection(OperationContext* opCtx,
                            NamespaceString fromNss,
                            NamespaceString toNss,
                            bool stayTemp) const final;

    static Status validateDBName(const DatabaseName& dbName);

    const NamespaceString& getSystemViewsName() const final {
        return _viewsName;
    }

    void createSystemDotViewsIfNecessary(OperationContext* opCtx) const final;

private:
    StatusWith<std::unique_ptr<CollatorInterface>> _validateCollator(OperationContext* opCtx,
                                                                     CollectionOptions& opts) const;
    Collection* _createCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptions& opts,
        const std::variant<VirtualCollectionOptions, CreateCollCatalogIdentifier>&
            voptsOrCatalogIdentifier,
        bool createDefaultIndexes,
        const BSONObj& idIndex,
        bool fromMigrate,
        boost::optional<bool> recordIdsReplicated) const;

    Status _createView(OperationContext* opCtx,
                       const NamespaceString& viewName,
                       const CollectionOptions& options) const;

    /**
     * Throws if there is a reason 'ns' cannot be created as a user collection. Namespace pattern
     * matching checks should be added to userAllowedCreateNS().
     */
    void _checkCanCreateCollection(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const CollectionOptions& options) const;

    /**
     * Completes a collection drop by removing the collection itself from the storage engine.
     *
     * This is called from dropCollectionEvenIfSystem() to drop the collection immediately on
     * unreplicated collection drops.
     */
    Status _finishDropCollection(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 Collection* collection) const;

    /**
     * Removes all indexes for a collection.
     */
    void _dropCollectionIndexes(OperationContext* opCtx,
                                const NamespaceString& nss,
                                Collection* collection) const;

    const DatabaseName _name;  // "dbname"

    const NamespaceString _viewsName;  // "dbname.system.views"
};

}  // namespace mongo
