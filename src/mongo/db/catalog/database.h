// database.h

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage_options.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/string_map.h"

namespace mongo {

    class Collection;
    class DataFile;
    class DatabaseCatalogEntry;
    class ExtentManager;
    class IndexCatalog;
    class NamespaceDetails;
    class OperationContext;

    /**
     * Database represents a database database
     * Each database database has its own set of files -- dbname.ns, dbname.0, dbname.1, ...
     * NOT memory mapped
    */
    class Database {
    public:
        Database(OperationContext* txn, StringData name, DatabaseCatalogEntry* dbEntry);

        // must call close first
        ~Database();

        // closes files and other cleanup see below.
        void close( OperationContext* txn );

        const std::string& name() const { return _name; }

        void clearTmpCollections(OperationContext* txn);

        /**
         * Sets a new profiling level for the database and returns the outcome.
         *
         * @param txn Operation context which to use for creating the profiling collection.
         * @param newLevel New profiling level to use.
         */
        Status setProfilingLevel(OperationContext* txn, int newLevel);

        int getProfilingLevel() const { return _profile; }
        const char* getProfilingNS() const { return _profileName.c_str(); }

        void getStats( OperationContext* opCtx, BSONObjBuilder* output, double scale = 1 );

        const DatabaseCatalogEntry* getDatabaseCatalogEntry() const;

        Status dropCollection( OperationContext* txn, StringData fullns );

        Collection* createCollection( OperationContext* txn,
                                      StringData ns,
                                      const CollectionOptions& options = CollectionOptions(),
                                      bool allocateSpace = true,
                                      bool createDefaultIndexes = true );

        /**
         * @param ns - this is fully qualified, which is maybe not ideal ???
         */
        Collection* getCollection( StringData ns ) const ;

        Collection* getCollection( const NamespaceString& ns ) const {
            return getCollection( ns.ns() );
        }

        Collection* getOrCreateCollection( OperationContext* txn, StringData ns );

        Status renameCollection( OperationContext* txn,
                                 StringData fromNS,
                                 StringData toNS,
                                 bool stayTemp );

        /**
         * @return name of an existing database with same text name but different
         * casing, if one exists.  Otherwise the empty std::string is returned.  If
         * 'duplicates' is specified, it is filled with all duplicate names.
         // TODO move???
         */
        static std::string duplicateUncasedName( const std::string &name,
                                                 std::set< std::string > *duplicates = 0 );

        static Status validateDBName( StringData dbname );

        const std::string& getSystemIndexesName() const { return _indexesName; }
    private:

        /**
         * Gets or creates collection instance from existing metadata,
         * Returns NULL if invalid
         *
         * Note: This does not add the collection to _collections map, that must be done
         * by the caller, who takes onership of the Collection*
         */
        Collection* _getOrCreateCollectionInstance(OperationContext* txn, StringData fullns);

        void _clearCollectionCache(OperationContext* txn, StringData fullns );

        class AddCollectionChange;
        class RemoveCollectionChange;

        const std::string _name; // "alleyinsider"

        DatabaseCatalogEntry* _dbEntry; // not owned here

        const std::string _profileName; // "alleyinsider.system.profile"
        const std::string _indexesName; // "alleyinsider.system.indexes"

        int _profile; // 0=off.

        // TODO: make sure deletes go through
        // this in some ways is a dupe of _namespaceIndex
        // but it points to a much more useful data structure
        typedef StringMap< Collection* > CollectionMap;
        CollectionMap _collections;

        friend class Collection;
        friend class NamespaceDetails;
        friend class IndexCatalog;
    };

    void dropDatabase(OperationContext* txn, Database* db );

    void dropAllDatabasesExceptLocal(OperationContext* txn);

    Status userCreateNS( OperationContext* txn,
                         Database* db,
                         StringData ns,
                         BSONObj options,
                         bool logForReplication,
                         bool createDefaultIndexes = true );

} // namespace mongo
