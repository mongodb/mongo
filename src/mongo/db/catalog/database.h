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

#include <memory>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/string_map.h"

namespace mongo {
/**
 * Represents a logical database containing Collections.
 *
 * The semantics for a const Database are that you can mutate individual collections but not add or
 * remove them.
 */
class Database {
public:
    typedef StringMap<Collection*> CollectionMap;

    class Impl {
    public:
        virtual ~Impl() = 0;

        virtual void init(OperationContext* opCtx) = 0;

        virtual void close(OperationContext* opCtx, const std::string& reason) = 0;

        virtual const std::string& name() const = 0;

        virtual void clearTmpCollections(OperationContext* opCtx) = 0;

        virtual Status setProfilingLevel(OperationContext* opCtx, int newLevel) = 0;

        virtual int getProfilingLevel() const = 0;

        virtual const char* getProfilingNS() const = 0;

        virtual void setDropPending(OperationContext* opCtx, bool dropPending) = 0;

        virtual bool isDropPending(OperationContext* opCtx) const = 0;

        virtual void getStats(OperationContext* opCtx, BSONObjBuilder* output, double scale) = 0;

        virtual const DatabaseCatalogEntry* getDatabaseCatalogEntry() const = 0;

        virtual Status dropCollection(OperationContext* opCtx,
                                      StringData fullns,
                                      repl::OpTime dropOpTime) = 0;
        virtual Status dropCollectionEvenIfSystem(OperationContext* opCtx,
                                                  const NamespaceString& fullns,
                                                  repl::OpTime dropOpTime) = 0;

        virtual Status dropView(OperationContext* opCtx, StringData fullns) = 0;

        virtual Collection* createCollection(OperationContext* opCtx,
                                             StringData ns,
                                             const CollectionOptions& options,
                                             bool createDefaultIndexes,
                                             const BSONObj& idIndex) = 0;

        virtual Status createView(OperationContext* opCtx,
                                  StringData viewName,
                                  const CollectionOptions& options) = 0;

        virtual Collection* getCollection(OperationContext* opCtx, StringData ns) const = 0;

        virtual ViewCatalog* getViewCatalog() = 0;

        virtual Collection* getOrCreateCollection(OperationContext* opCtx,
                                                  const NamespaceString& nss) = 0;

        virtual Status renameCollection(OperationContext* opCtx,
                                        StringData fromNS,
                                        StringData toNS,
                                        bool stayTemp) = 0;

        virtual const NamespaceString& getSystemIndexesName() const = 0;

        virtual const std::string& getSystemViewsName() const = 0;

        virtual CollectionMap& collections() = 0;
        virtual const CollectionMap& collections() const = 0;
    };

private:
    static std::unique_ptr<Impl> makeImpl(Database* _this,
                                          OperationContext* opCtx,
                                          StringData name,
                                          DatabaseCatalogEntry* dbEntry);

public:
    using factory_function_type = decltype(makeImpl);

    static void registerFactory(stdx::function<factory_function_type> factory);

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

    explicit inline Database(OperationContext* const opCtx,
                             const StringData name,
                             DatabaseCatalogEntry* const dbEntry)
        : _pimpl(makeImpl(this, opCtx, name, dbEntry)) {
        this->_impl().init(opCtx);
    }

    // must call close first
    inline ~Database() = default;

    inline Database(Database&&) = delete;
    inline Database& operator=(Database&&) = delete;

    inline iterator begin() const {
        return iterator(this->_impl().collections().begin());
    }

    inline iterator end() const {
        return iterator(this->_impl().collections().end());
    }

    // closes files and other cleanup see below.
    inline void close(OperationContext* const opCtx, const std::string& reason) {
        return this->_impl().close(opCtx, reason);
    }

    inline const std::string& name() const {
        return this->_impl().name();
    }

    inline void clearTmpCollections(OperationContext* const opCtx) {
        return this->_impl().clearTmpCollections(opCtx);
    }

    /**
     * Sets a new profiling level for the database and returns the outcome.
     *
     * @param opCtx Operation context which to use for creating the profiling collection.
     * @param newLevel New profiling level to use.
     */
    inline Status setProfilingLevel(OperationContext* const opCtx, const int newLevel) {
        return this->_impl().setProfilingLevel(opCtx, newLevel);
    }

    inline int getProfilingLevel() const {
        return this->_impl().getProfilingLevel();
    }

    inline const char* getProfilingNS() const {
        return this->_impl().getProfilingNS();
    }

    /**
     * Sets the 'drop-pending' state of this Database.
     * This is done at the beginning of a dropDatabase operation and is used to reject subsequent
     * collection creation requests on this database.
     * Throws a UserAssertion if this is called on a Database that is already in a 'drop-pending'
     * state.
     * The database must be locked in MODE_X when calling this function.
     */
    inline void setDropPending(OperationContext* opCtx, bool dropPending) {
        this->_impl().setDropPending(opCtx, dropPending);
    }

    /**
     * Returns the 'drop-pending' state of this Database.
     * The database must be locked in MODE_X when calling this function.
     */
    inline bool isDropPending(OperationContext* opCtx) const {
        return this->_impl().isDropPending(opCtx);
    }

    inline void getStats(OperationContext* const opCtx,
                         BSONObjBuilder* const output,
                         const double scale = 1) {
        return this->_impl().getStats(opCtx, output, scale);
    }

    inline const DatabaseCatalogEntry* getDatabaseCatalogEntry() const {
        return this->_impl().getDatabaseCatalogEntry();
    }

    /**
     * dropCollection() will refuse to drop system collections. Use dropCollectionEvenIfSystem() if
     * that is required.
     *
     * If we are applying a 'drop' oplog entry on a secondary, 'dropOpTime' will contain the optime
     * of the oplog entry.
     */
    inline Status dropCollection(OperationContext* const opCtx,
                                 const StringData fullns,
                                 repl::OpTime dropOpTime = {}) {
        return this->_impl().dropCollection(opCtx, fullns, dropOpTime);
    }
    inline Status dropCollectionEvenIfSystem(OperationContext* const opCtx,
                                             const NamespaceString& fullns,
                                             repl::OpTime dropOpTime = {}) {
        return this->_impl().dropCollectionEvenIfSystem(opCtx, fullns, dropOpTime);
    }

    inline Status dropView(OperationContext* const opCtx, const StringData fullns) {
        return this->_impl().dropView(opCtx, fullns);
    }

    inline Collection* createCollection(OperationContext* const opCtx,
                                        StringData ns,
                                        const CollectionOptions& options = CollectionOptions(),
                                        const bool createDefaultIndexes = true,
                                        const BSONObj& idIndex = BSONObj()) {
        return this->_impl().createCollection(opCtx, ns, options, createDefaultIndexes, idIndex);
    }

    inline Status createView(OperationContext* const opCtx,
                             const StringData viewName,
                             const CollectionOptions& options) {
        return this->_impl().createView(opCtx, viewName, options);
    }

    /**
     * @param ns - this is fully qualified, which is maybe not ideal ???
     */
    inline Collection* getCollection(OperationContext* opCtx, const StringData ns) const {
        return this->_impl().getCollection(opCtx, ns);
    }

    inline Collection* getCollection(OperationContext* opCtx, const NamespaceString& ns) const {
        return this->_impl().getCollection(opCtx, ns.ns());
    }

    /**
     * Get the view catalog, which holds the definition for all views created on this database. You
     * must be holding a database lock to use this accessor.
     */
    inline ViewCatalog* getViewCatalog() {
        return this->_impl().getViewCatalog();
    }

    inline Collection* getOrCreateCollection(OperationContext* const opCtx,
                                             const NamespaceString& nss) {
        return this->_impl().getOrCreateCollection(opCtx, nss);
    }

    inline Status renameCollection(OperationContext* const opCtx,
                                   const StringData fromNS,
                                   const StringData toNS,
                                   const bool stayTemp) {
        return this->_impl().renameCollection(opCtx, fromNS, toNS, stayTemp);
    }

    /**
     * Physically drops the specified opened database and removes it from the server's metadata. It
     * doesn't notify the replication subsystem or do any other consistency checks, so it should
     * not be used directly from user commands.
     *
     * Must be called with the specified database locked in X mode.
     */
    static void dropDatabase(OperationContext* opCtx, Database* db);

    /**
     * Registers an implementation of `Database::dropDatabase` for use by library clients.
     * This is necessary to allow `catalog/database` to be a vtable edge.
     * @param impl Implementation of `dropDatabase` to install.
     * @note This call is not thread safe.
     */
    static void registerDropDatabaseImpl(stdx::function<decltype(dropDatabase)> impl);

    // static Status validateDBName( StringData dbname );

    inline const NamespaceString& getSystemIndexesName() const {
        return this->_impl().getSystemIndexesName();
    }

    inline const std::string& getSystemViewsName() const {
        return this->_impl().getSystemViewsName();
    }

private:
    // This structure exists to give us a customization point to decide how to force users of this
    // class to depend upon the corresponding `database.cpp` Translation Unit (TU).  All public
    // forwarding functions call `_impl(), and `_impl` creates an instance of this structure.
    struct TUHook {
        static void hook() noexcept;

        explicit inline TUHook() noexcept {
            if (kDebugBuild)
                this->hook();
        }
    };

    inline const Impl& _impl() const {
        TUHook{};
        return *this->_pimpl;
    }

    inline Impl& _impl() {
        TUHook{};
        return *this->_pimpl;
    }

    std::unique_ptr<Impl> _pimpl;
};

void dropAllDatabasesExceptLocal(OperationContext* opCtx);

/**
 * Registers an implementation of `dropAllDatabaseExceptLocal` for use by library clients.
 * This is necessary to allow `catalog/database` to be a vtable edge.
 * @param impl Implementation of `dropAllDatabaseExceptLocal` to install.
 * @note This call is not thread safe.
 */
void registerDropAllDatabasesExceptLocalImpl(
    stdx::function<decltype(dropAllDatabasesExceptLocal)> impl);

/**
 * Creates the namespace 'ns' in the database 'db' according to 'options'. If 'createDefaultIndexes'
 * is true, creates the _id index for the collection (and the system indexes, in the case of system
 * collections). Creates the collection's _id index according to 'idIndex', if it is non-empty. When
 * 'idIndex' is empty, creates the default _id index.
 */
Status userCreateNS(OperationContext* opCtx,
                    Database* db,
                    StringData ns,
                    BSONObj options,
                    CollectionOptions::ParseKind parseKind = CollectionOptions::parseForCommand,
                    bool createDefaultIndexes = true,
                    const BSONObj& idIndex = BSONObj());

/**
 * Registers an implementation of `userCreateNS` for use by library clients.
 * This is necessary to allow `catalog/database` to be a vtable edge.
 * @param impl Implementation of `userCreateNS` to install.
 * @note This call is not thread safe.
 */
void registerUserCreateNSImpl(stdx::function<decltype(userCreateNS)> impl);
}  // namespace mongo
