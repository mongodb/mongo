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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage_options.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/string_map.h"

namespace mongo {

    class Collection;
    class DatabaseCatalogEntry;
    class DataFile;
    class ExtentManager;
    class IndexCatalog;
    class MMAP1DatabaseCatalogEntry;
    class MmapV1ExtentManager;
    class NamespaceDetails;
    class NamespaceIndex;
    class OperationContext;

    /**
     * Database represents a database database
     * Each database database has its own set of files -- dbname.ns, dbname.0, dbname.1, ...
     * NOT memory mapped
    */
    class Database {
    public:
        // you probably need to be in dbHolderMutex when constructing this
        Database(OperationContext* txn,
                 const char *nm,
                 /*out*/ bool& newDb,
                 const std::string& path = storageGlobalParams.dbpath);

        /* you must use this to close - there is essential code in this method that is not in the ~Database destructor.
           thus the destructor is private.  this could be cleaned up one day...
        */
        static void closeDatabase( const std::string& db, const std::string& path );

        const std::string& name() const { return _name; }
        const std::string& path() const { return _path; }

        void clearTmpCollections(OperationContext* txn);

        bool isEmpty() const;

        /**
         * total file size of Database in bytes
         */
        long long fileSize() const;

        int numFiles() const;

        void getFileFormat( OperationContext* txn, int* major, int* minor );

        /**
         * @return true if success.  false if bad level or error creating profile ns
         */
        bool setProfilingLevel( OperationContext* txn, int newLevel , std::string& errmsg );

        void flushFiles( bool sync );

        /**
         * @return true if ns is part of the database
         *         ns=foo.bar, db=foo returns true
         */
        bool ownsNS( const std::string& ns ) const {
            if ( ! mongoutils::str::startsWith( ns , _name ) )
                return false;
            return ns[_name.size()] == '.';
        }

        int getProfilingLevel() const { return _profile; }
        const char* getProfilingNS() const { return _profileName.c_str(); }

        void getStats( OperationContext* opCtx, BSONObjBuilder* output, double scale = 1 );

        long long getIndexSizeForCollection( OperationContext* opCtx,
                                             Collection* collections,
                                             BSONObjBuilder* details = NULL,
                                             int scale = 1 );

        const DatabaseCatalogEntry* getDatabaseCatalogEntry() const;

        // TODO: do not think this method should exist, so should try and encapsulate better
        MmapV1ExtentManager* getExtentManager();
        const MmapV1ExtentManager* getExtentManager() const;

        Status dropCollection( OperationContext* txn, const StringData& fullns );

        Collection* createCollection( OperationContext* txn,
                                      const StringData& ns,
                                      const CollectionOptions& options = CollectionOptions(),
                                      bool allocateSpace = true,
                                      bool createDefaultIndexes = true );

        /**
         * @param ns - this is fully qualified, which is maybe not ideal ???
         */
        Collection* getCollection( OperationContext* txn, const StringData& ns );

        Collection* getCollection( OperationContext* txn, const NamespaceString& ns ) {
            return getCollection( txn, ns.ns() );
        }

        Collection* getOrCreateCollection( OperationContext* txn, const StringData& ns );

        Status renameCollection( OperationContext* txn,
                                 const StringData& fromNS,
                                 const StringData& toNS,
                                 bool stayTemp );

        /**
         * @return name of an existing database with same text name but different
         * casing, if one exists.  Otherwise the empty std::string is returned.  If
         * 'duplicates' is specified, it is filled with all duplicate names.
         // TODO move???
         */
        static string duplicateUncasedName( const std::string &name,
                                            const std::string &path,
                                            std::set< std::string > *duplicates = 0 );

        static Status validateDBName( const StringData& dbname );

        const std::string& getSystemIndexesName() const { return _indexesName; }
    private:

        void _clearCollectionCache( const StringData& fullns );

        void _clearCollectionCache_inlock( const StringData& fullns );

        ~Database(); // closes files and other cleanup see below.

        void _addNamespaceToCatalog( OperationContext* txn,
                                     const StringData& ns,
                                     const BSONObj* options );


        /**
         * removes from *.system.namespaces
         * frees extents
         * removes from NamespaceIndex
         * NOT RIGHT NOW, removes cache entry in Database TODO?
         */
        Status _dropNS( OperationContext* txn, const StringData& ns );

        Status _renameSingleNamespace( OperationContext* txn,
                                       const StringData& fromNS,
                                       const StringData& toNS,
                                       bool stayTemp );

        const std::string _name; // "alleyinsider"
        const std::string _path; // "/data/db"

        boost::scoped_ptr<MMAP1DatabaseCatalogEntry> _dbEntry;

        const std::string _profileName; // "alleyinsider.system.profile"
        const std::string _namespacesName; // "alleyinsider.system.namespaces"
        const std::string _indexesName; // "alleyinsider.system.indexes"

        int _profile; // 0=off.

        // TODO: make sure deletes go through
        // this in some ways is a dupe of _namespaceIndex
        // but it points to a much more useful data structure
        typedef StringMap< Collection* > CollectionMap;
        CollectionMap _collections;
        mongo::mutex _collectionLock;

        friend class Collection;
        friend class NamespaceDetails;
        friend class IndexCatalog;
    };

} // namespace mongo
