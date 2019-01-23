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

#include "mongo/platform/random.h"

namespace mongo {

class DatabaseImpl final : public Database {
public:
    explicit DatabaseImpl(StringData name, DatabaseCatalogEntry* dbEntry, uint64_t epoch);

    void init(OperationContext*) final;


    // closes files and other cleanup see below.
    void close(OperationContext* opCtx) final;

    const std::string& name() const final {
        return _name;
    }

    void clearTmpCollections(OperationContext* opCtx) final;

    /**
     * Sets a new profiling level for the database and returns the outcome.
     *
     * @param opCtx Operation context which to use for creating the profiling collection.
     * @param newLevel New profiling level to use.
     */
    Status setProfilingLevel(OperationContext* opCtx, int newLevel) final;

    int getProfilingLevel() const final {
        return _profile;
    }
    const char* getProfilingNS() const final {
        return _profileName.c_str();
    }

    void setDropPending(OperationContext* opCtx, bool dropPending) final;

    bool isDropPending(OperationContext* opCtx) const final;

    void getStats(OperationContext* opCtx, BSONObjBuilder* output, double scale = 1) final;

    const DatabaseCatalogEntry* getDatabaseCatalogEntry() const final;

    /**
     * dropCollection() will refuse to drop system collections. Use dropCollectionEvenIfSystem() if
     * that is required.
     *
     * If we are applying a 'drop' oplog entry on a secondary, 'dropOpTime' will contain the optime
     * of the oplog entry.
     */
    Status dropCollection(OperationContext* opCtx,
                          StringData fullns,
                          repl::OpTime dropOpTime) final;
    Status dropCollectionEvenIfSystem(OperationContext* opCtx,
                                      const NamespaceString& fullns,
                                      repl::OpTime dropOpTime) final;

    Status dropView(OperationContext* opCtx, StringData fullns) final;

    Status userCreateNS(OperationContext* opCtx,
                        const NamespaceString& fullns,
                        CollectionOptions collectionOptions,
                        bool createDefaultIndexes,
                        const BSONObj& idIndex) final;

    Collection* createCollection(OperationContext* opCtx,
                                 StringData ns,
                                 const CollectionOptions& options = CollectionOptions(),
                                 bool createDefaultIndexes = true,
                                 const BSONObj& idIndex = BSONObj()) final;

    Status createView(OperationContext* opCtx,
                      StringData viewName,
                      const CollectionOptions& options) final;

    /**
     * @param ns - this is fully qualified, which is maybe not ideal ???
     */
    Collection* getCollection(OperationContext* opCtx, StringData ns) const final;

    Collection* getCollection(OperationContext* opCtx, const NamespaceString& ns) const;

    Collection* getOrCreateCollection(OperationContext* opCtx, const NamespaceString& nss) final;

    /**
     * Renames the fully qualified namespace 'fromNS' to the fully qualified namespace 'toNS'.
     * Illegal to call unless both 'fromNS' and 'toNS' are within this database. Returns an error if
     * 'toNS' already exists or 'fromNS' does not exist.
     */
    Status renameCollection(OperationContext* opCtx,
                            StringData fromNS,
                            StringData toNS,
                            bool stayTemp) final;

    /**
     * Physically drops the specified opened database and removes it from the server's metadata. It
     * doesn't notify the replication subsystem or do any other consistency checks, so it should
     * not be used directly from user commands.
     *
     * Must be called with the specified database locked in X mode.
     */
    static void dropDatabase(OperationContext* opCtx, Database* db);

    static Status validateDBName(StringData dbname);

    const std::string& getSystemViewsName() const final {
        return _viewsName;
    }

    StatusWith<NamespaceString> makeUniqueCollectionNamespace(OperationContext* opCtx,
                                                              StringData collectionNameModel) final;

    void checkForIdIndexesAndDropPendingCollections(OperationContext* opCtx) final;

    iterator begin() const final {
        return iterator(_collections.begin());
    }

    iterator end() const final {
        return iterator(_collections.end());
    }

    uint64_t epoch() const {
        return _epoch;
    }

private:
    class FinishDropChange;

    /**
     * Throws if there is a reason 'ns' cannot be created as a user collection.
     */
    void _checkCanCreateCollection(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const CollectionOptions& options);

    /**
     * Completes a collection drop by removing all the indexes and removing the collection itself
     * from the storage engine.
     *
     * This is called from dropCollectionEvenIfSystem() to drop the collection immediately on
     * unreplicated collection drops.
     */
    Status _finishDropCollection(OperationContext* opCtx,
                                 const NamespaceString& fullns,
                                 Collection* collection);

    const std::string _name;  // "dbname"

    DatabaseCatalogEntry* _dbEntry;  // not owned here

    const uint64_t _epoch;

    const std::string _profileName;  // "dbname.system.profile"
    const std::string _viewsName;    // "dbname.system.views"

    int _profile;  // 0=off.

    // If '_dropPending' is true, this Database is in the midst of a two-phase drop. No new
    // collections may be created in this Database.
    // This variable may only be read/written while the database is locked in MODE_X.
    bool _dropPending = false;

    // Random number generator used to create unique collection namespaces suitable for temporary
    // collections. Lazily created on first call to makeUniqueCollectionNamespace().
    // This variable may only be read/written while the database is locked in MODE_X.
    std::unique_ptr<PseudoRandom> _uniqueCollectionNamespacePseudoRandom;

    CollectionMap _collections;
};

}  // namespace mongo
