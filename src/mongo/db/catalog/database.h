
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

#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/string_map.h"

namespace mongo {

class DatabaseCatalogEntry;
class OperationContext;

/**
 * Represents a logical database containing Collections.
 *
 * The semantics for a const Database are that you can mutate individual collections but not add or
 * remove them.
 */
class Database : public Decorable<Database> {
public:
    typedef StringMap<Collection*> CollectionMap;

    /**
     * Creates the namespace 'ns' in the database 'db' according to 'options'. If
     * 'createDefaultIndexes' is true, creates the _id index for the collection (and the system
     * indexes, in the case of system collections). Creates the collection's _id index according
     * to 'idIndex', if it is non-empty.  When 'idIndex' is empty, creates the default _id index.
     */
    virtual Status userCreateNS(OperationContext* opCtx,
                                const NamespaceString& fullns,
                                CollectionOptions collectionOptions,
                                bool createDefaultIndexes = true,
                                const BSONObj& idIndex = BSONObj()) = 0;

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

        explicit inline iterator() = default;
        inline iterator(CollectionMap::const_iterator it) : _it(std::move(it)) {}

        inline reference operator*() const {
            return _it->second;
        }

        inline pointer operator->() const {
            return &_it->second;
        }

        inline friend bool operator==(const iterator& lhs, const iterator& rhs) {
            return lhs._it == rhs._it;
        }

        inline friend bool operator!=(const iterator& lhs, const iterator& rhs) {
            return !(lhs == rhs);
        }

        inline iterator& operator++() {
            ++_it;
            return *this;
        }

        inline iterator operator++(int) {
            auto oldPosition = *this;
            ++_it;
            return oldPosition;
        }

    private:
        CollectionMap::const_iterator _it;
    };

    Database() = default;

    // must call close first
    virtual ~Database() = default;

    inline Database(Database&&) = delete;
    inline Database& operator=(Database&&) = delete;

    virtual iterator begin() const = 0;
    virtual iterator end() const = 0;

    /**
     * Sets up internal memory structures.
     */
    virtual void init(OperationContext* opCtx) = 0;

    // closes files and other cleanup see below.
    virtual void close(OperationContext* const opCtx) = 0;

    virtual const std::string& name() const = 0;

    virtual void clearTmpCollections(OperationContext* const opCtx) = 0;

    /**
     * Sets a new profiling level for the database and returns the outcome.
     *
     * @param opCtx Operation context which to use for creating the profiling collection.
     * @param newLevel New profiling level to use.
     */
    virtual Status setProfilingLevel(OperationContext* const opCtx, const int newLevel) = 0;

    virtual int getProfilingLevel() const = 0;

    virtual const char* getProfilingNS() const = 0;

    /**
     * Sets the 'drop-pending' state of this Database.
     * This is done at the beginning of a dropDatabase operation and is used to reject subsequent
     * collection creation requests on this database.
     * Throws a UserAssertion if this is called on a Database that is already in a 'drop-pending'
     * state.
     * The database must be locked in MODE_X when calling this function.
     */
    virtual void setDropPending(OperationContext* opCtx, bool dropPending) = 0;

    /**
     * Returns the 'drop-pending' state of this Database.
     * The database must be locked in MODE_X when calling this function.
     */
    virtual bool isDropPending(OperationContext* opCtx) const = 0;

    virtual void getStats(OperationContext* const opCtx,
                          BSONObjBuilder* const output,
                          const double scale = 1) = 0;

    virtual const DatabaseCatalogEntry* getDatabaseCatalogEntry() const = 0;

    /**
     * dropCollection() will refuse to drop system collections. Use dropCollectionEvenIfSystem() if
     * that is required.
     *
     * If we are applying a 'drop' oplog entry on a secondary, 'dropOpTime' will contain the optime
     * of the oplog entry.
     */
    virtual Status dropCollection(OperationContext* const opCtx,
                                  const StringData fullns,
                                  repl::OpTime dropOpTime = {}) = 0;
    virtual Status dropCollectionEvenIfSystem(OperationContext* const opCtx,
                                              const NamespaceString& fullns,
                                              repl::OpTime dropOpTime = {}) = 0;

    virtual Status dropView(OperationContext* const opCtx, const StringData fullns) = 0;

    virtual Collection* createCollection(OperationContext* const opCtx,
                                         StringData ns,
                                         const CollectionOptions& options = CollectionOptions(),
                                         const bool createDefaultIndexes = true,
                                         const BSONObj& idIndex = BSONObj()) = 0;

    virtual Status createView(OperationContext* const opCtx,
                              const StringData viewName,
                              const CollectionOptions& options) = 0;

    /**
     * @param ns - this is fully qualified, which is maybe not ideal ???
     */
    virtual Collection* getCollection(OperationContext* opCtx, const StringData ns) const = 0;

    virtual Collection* getCollection(OperationContext* opCtx, const NamespaceString& ns) const = 0;

    /**
     * Get the view catalog, which holds the definition for all views created on this database. You
     * must be holding a database lock to use this accessor.
     */
    virtual ViewCatalog* getViewCatalog() = 0;

    virtual Collection* getOrCreateCollection(OperationContext* const opCtx,
                                              const NamespaceString& nss) = 0;

    virtual Status renameCollection(OperationContext* const opCtx,
                                    const StringData fromNS,
                                    const StringData toNS,
                                    const bool stayTemp) = 0;

    virtual const std::string& getSystemViewsName() const = 0;

    /**
     * Generates a collection namespace suitable for creating a temporary collection.
     * The namespace is based on a model that replaces each percent sign in 'collectionNameModel' by
     * a random character in the range [0-9A-Za-z].
     * Returns FailedToParse if 'collectionNameModel' does not contain any percent signs.
     * Returns NamespaceExists if we are unable to generate a collection name that does not conflict
     * with an existing collection in this database.
     *
     * The database must be locked in MODE_X when calling this function.
     */
    virtual StatusWith<NamespaceString> makeUniqueCollectionNamespace(
        OperationContext* opCtx, StringData collectionNameModel) = 0;

    /**
     * If we are in a replset, every replicated collection must have an _id index.  As we scan each
     * database, we also gather a list of drop-pending collection namespaces for the
     * DropPendingCollectionReaper to clean up eventually.
     */
    virtual void checkForIdIndexesAndDropPendingCollections(OperationContext* opCtx) = 0;

    /**
     * A database is assigned a new epoch whenever it is closed and re-opened. This involves
     * deleting and reallocating a new Database object, so the epoch for a particular Database
     * instance is immutable.
     *
     * Callers of this method must hold the global lock in at least MODE_IS.
     *
     * This allows callers which drop and reacquire locks to detect an intervening database close.
     * For example, closing a database must kill all active queries against the database. This is
     * implemented by checking that the epoch has not changed during query yield recovery.
     */
    virtual uint64_t epoch() const = 0;
};

}  // namespace mongo
