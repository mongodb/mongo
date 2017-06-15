/**
 *    Copyright (C) 2017 10gen Inc.
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

#include "mongo/db/catalog/database.h"

#include <memory>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/string_map.h"

namespace mongo {

class Collection;
class DatabaseCatalogEntry;
class IndexCatalog;
class NamespaceDetails;
class OperationContext;

/**
 * Represents a logical database containing Collections.
 *
 * The semantics for a const Database are that you can mutate individual collections but not add or
 * remove them.
 */
class DatabaseImpl : public Database::Impl {
public:
    typedef StringMap<Collection*> CollectionMap;

    /**
     * Iterating over a Database yields Collection* pointers.
     */
    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Collection*;
        using pointer = const value_type*;
        using reference = const value_type&;
        using difference_type = ptrdiff_t;

        iterator() = default;
        iterator(CollectionMap::const_iterator it) : _it(it) {}

        reference operator*() const {
            return _it->second;
        }

        pointer operator->() const {
            return &_it->second;
        }

        bool operator==(const iterator& other) {
            return _it == other._it;
        }

        bool operator!=(const iterator& other) {
            return _it != other._it;
        }

        iterator& operator++() {
            ++_it;
            return *this;
        }

        iterator operator++(int) {
            auto oldPosition = *this;
            ++_it;
            return oldPosition;
        }

    private:
        CollectionMap::const_iterator _it;
    };

    explicit DatabaseImpl(Database* this_,
                          OperationContext* opCtx,
                          StringData name,
                          DatabaseCatalogEntry* dbEntry);

    // must call close first
    ~DatabaseImpl();

    void init(OperationContext*) final;

    iterator begin() const {
        return iterator(_collections.begin());
    }

    iterator end() const {
        return iterator(_collections.end());
    }

    // closes files and other cleanup see below.
    void close(OperationContext* opCtx, const std::string& reason) final;

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

    /**
     * Get the view catalog, which holds the definition for all views created on this database. You
     * must be holding a database lock to use this accessor.
     */
    ViewCatalog* getViewCatalog() final {
        return &_views;
    }

    Collection* getOrCreateCollection(OperationContext* opCtx, const NamespaceString& nss) final;

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

    const NamespaceString& getSystemIndexesName() const final {
        return _indexesName;
    }

    const std::string& getSystemViewsName() const final {
        return _viewsName;
    }

    inline CollectionMap& collections() final {
        return _collections;
    }
    inline const CollectionMap& collections() const final {
        return _collections;
    }

private:
    /**
     * Gets or creates collection instance from existing metadata,
     * Returns NULL if invalid
     *
     * Note: This does not add the collection to _collections map, that must be done
     * by the caller, who takes onership of the Collection*
     */
    Collection* _getOrCreateCollectionInstance(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Throws if there is a reason 'ns' cannot be created as a user collection.
     */
    void _checkCanCreateCollection(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const CollectionOptions& options);

    /**
     * Deregisters and invalidates all cursors on collection 'fullns'.  Callers must specify
     * 'reason' for why the cache is being cleared. If 'collectionGoingAway' is false,
     * unpinned cursors will not be killed.
     */
    void _clearCollectionCache(OperationContext* opCtx,
                               StringData fullns,
                               const std::string& reason,
                               bool collectionGoingAway);

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

    class AddCollectionChange;
    class RemoveCollectionChange;

    const std::string _name;  // "dbname"

    DatabaseCatalogEntry* _dbEntry;  // not owned here

    const std::string _profileName;      // "dbname.system.profile"
    const NamespaceString _indexesName;  // "dbname.system.indexes"
    const std::string _viewsName;        // "dbname.system.views"

    int _profile;  // 0=off.

    // If '_dropPending' is true, this Database is in the midst of a two-phase drop. No new
    // collections may be created in this Database.
    // This variable may only be read/written while the database is locked in MODE_X.
    bool _dropPending = false;

    CollectionMap _collections;

    DurableViewCatalogImpl _durableViews;  // interface for system.views operations
    ViewCatalog _views;                    // in-memory representation of _durableViews
    Database* _this;                       // Pointer to wrapper, for external caller compatibility.

    friend class Collection;
    friend class NamespaceDetails;
    friend class IndexCatalog;
};

void dropAllDatabasesExceptLocalImpl(OperationContext* opCtx);

/**
 * Creates the namespace 'ns' in the database 'db' according to 'options'. If 'createDefaultIndexes'
 * is true, creates the _id index for the collection (and the system indexes, in the case of system
 * collections). Creates the collection's _id index according to 'idIndex', if it is non-empty. When
 * 'idIndex' is empty, creates the default _id index.
 */
Status userCreateNSImpl(OperationContext* opCtx,
                        Database* db,
                        StringData ns,
                        BSONObj options,
                        CollectionOptions::ParseKind parseKind = CollectionOptions::parseForCommand,
                        bool createDefaultIndexes = true,
                        const BSONObj& idIndex = BSONObj());

}  // namespace mongo
