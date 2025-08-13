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
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbcommands_gen.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/virtual_collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/optime.h"

#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class DatabaseImpl final : public Database {
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
     * The caller should hold a DB X lock and ensure there are no index builds in progress on the
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

    Status userCreateNS(
        OperationContext* opCtx,
        const NamespaceString& nss,
        CollectionOptions collectionOptions,
        bool createDefaultIndexes,
        const BSONObj& idIndex,
        bool fromMigrate,
        const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier) const final;

    Status userCreateVirtualNS(OperationContext* opCtx,
                               const NamespaceString& fullns,
                               CollectionOptions opts,
                               const VirtualCollectionOptions& vopts) const final;

    Collection* createCollection(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const CollectionOptions& options = CollectionOptions(),
                                 bool createDefaultIndexes = true,
                                 const BSONObj& idIndex = BSONObj(),
                                 bool fromMigrate = false) const final;

    Collection* createVirtualCollection(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const CollectionOptions& opts,
                                        const VirtualCollectionOptions& vopts) const final;

    StatusWith<std::unique_ptr<CollatorInterface>> validateCollator(
        OperationContext* opCtx, CollectionOptions& opts) const final;

    Status createView(OperationContext* opCtx,
                      const NamespaceString& viewName,
                      const CollectionOptions& options) const final;

    Status renameCollection(OperationContext* opCtx,
                            NamespaceString fromNss,
                            NamespaceString toNss,
                            bool stayTemp) const final;

    static Status validateDBName(const DatabaseName& dbName);

    const NamespaceString& getSystemViewsName() const final {
        return _viewsName;
    }

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
        bool fromMigrate) const;

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
