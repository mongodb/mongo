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

#include "mongo/db/catalog/database.h"

namespace mongo {

class DatabaseImpl final : public Database {
public:
    explicit DatabaseImpl(StringData name);

    void init(OperationContext*) const final;

    const std::string& name() const final {
        return _name;
    }

    void clearTmpCollections(OperationContext* opCtx) const final;

    void setDropPending(OperationContext* opCtx, bool dropPending) final;

    bool isDropPending(OperationContext* opCtx) const final;

    void getStats(OperationContext* opCtx,
                  BSONObjBuilder* output,
                  bool includeFreeStorage,
                  double scale = 1) const final;

    /**
     * dropCollection() will refuse to drop system collections. Use dropCollectionEvenIfSystem() if
     * that is required.
     *
     * If we are applying a 'drop' oplog entry on a secondary, 'dropOpTime' will contain the optime
     * of the oplog entry.
     *
     * The caller should hold a DB X lock and ensure there are no index builds in progress on the
     * collection.
     */
    Status dropCollection(OperationContext* opCtx,
                          NamespaceString nss,
                          repl::OpTime dropOpTime) const final;
    Status dropCollectionEvenIfSystem(OperationContext* opCtx,
                                      NamespaceString nss,
                                      repl::OpTime dropOpTime,
                                      bool markFromMigrate = false) const final;

    Status dropView(OperationContext* opCtx, NamespaceString viewName) const final;

    Status userCreateNS(OperationContext* opCtx,
                        const NamespaceString& nss,
                        CollectionOptions collectionOptions,
                        bool createDefaultIndexes,
                        const BSONObj& idIndex) const final;

    Collection* createCollection(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const CollectionOptions& options = CollectionOptions(),
                                 bool createDefaultIndexes = true,
                                 const BSONObj& idIndex = BSONObj()) const final;

    Status createView(OperationContext* opCtx,
                      const NamespaceString& viewName,
                      const CollectionOptions& options) const final;

    Status renameCollection(OperationContext* opCtx,
                            NamespaceString fromNss,
                            NamespaceString toNss,
                            bool stayTemp) const final;

    static Status validateDBName(StringData dbname);

    const NamespaceString& getSystemViewsName() const final {
        return _viewsName;
    }

    /**
     * Given an input pattern `collectionNameModel`, returns a namespace string where `%` characters
     * are replaced with random alpha-numerics.
     *
     * When called while holding an exclusive database lock, the collection name is guaranteed to
     * not exist. Otherwise the caller is responsible for acquiring locks to check uniqueness.
     *
     * Returns a NamespaceExists error status if multiple attempts fail to generate a possible
     * unique name.
     */
    StatusWith<NamespaceString> makeUniqueCollectionNamespace(
        OperationContext* opCtx, StringData collectionNameModel) const final;

    void checkForIdIndexesAndDropPendingCollections(OperationContext* opCtx) const final;

private:
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

    const std::string _name;  // "dbname"

    const NamespaceString _viewsName;  // "dbname.system.views"

    // If '_dropPending' is true, this Database is in the midst of a two-phase drop. No new
    // collections may be created in this Database.
    // This variable may only be read/written while the database is locked in MODE_X.
    AtomicWord<bool> _dropPending{false};
};

}  // namespace mongo
