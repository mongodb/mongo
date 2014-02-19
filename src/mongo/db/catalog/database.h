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

#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/storage_options.h"
#include "mongo/util/string_map.h"

namespace mongo {

    class Collection;
    class Extent;
    class DataFile;
    class IndexCatalog;
    class IndexDetails;

    struct CollectionOptions {
        CollectionOptions() {
            reset();
        }

        void reset() {
            capped = false;
            cappedSize = 0;
            cappedMaxDocs = 0;
            initialNumExtents = 0;
            initialExtentSizes.clear();
            autoIndexId = DEFAULT;
            flags = 0;
            flagsSet = false;
            temp = false;
        }

        Status parse( const BSONObj& obj );
        BSONObj toBSON() const;

        // ----

        bool capped;
        long long cappedSize;
        long long cappedMaxDocs;

        // following 2 are mutually exclusive, can only have one set
        long long initialNumExtents;
        vector<long long> initialExtentSizes;

        // behavior of _id index creation when collection created
        void setNoIdIndex() { autoIndexId = NO; }
        enum {
            DEFAULT, // currently yes for most collections, NO for some system ones
            YES, // create _id index
            NO // do not create _id index
        } autoIndexId;

        // user flags
        int flags;
        bool flagsSet;

        bool temp;
    };

    /**
     * Database represents a database database
     * Each database database has its own set of files -- dbname.ns, dbname.0, dbname.1, ...
     * NOT memory mapped
    */
    class Database {
    public:
        // you probably need to be in dbHolderMutex when constructing this
        Database(const char *nm, /*out*/ bool& newDb,
                 const string& path = storageGlobalParams.dbpath);

        /* you must use this to close - there is essential code in this method that is not in the ~Database destructor.
           thus the destructor is private.  this could be cleaned up one day...
        */
        static void closeDatabase( const string& db, const string& path );

        const string& name() const { return _name; }
        const string& path() const { return _path; }

        void clearTmpCollections();

        /**
         * tries to make sure that this hasn't been deleted
         */
        bool isOk() const { return _magic == 781231; }

        bool isEmpty() { return ! _namespaceIndex.allocated(); }

        /**
         * total file size of Database in bytes
         */
        long long fileSize() const { return _extentManager.fileSize(); }

        int numFiles() const { return _extentManager.numFiles(); }

        void getFileFormat( int* major, int* minor );

        /**
         * makes sure we have an extra file at the end that is empty
         * safe to call this multiple times - the implementation will only preallocate one file
         */
        void preallocateAFile() { _extentManager.preallocateAFile(); }

        /**
         * @return true if success.  false if bad level or error creating profile ns
         */
        bool setProfilingLevel( int newLevel , string& errmsg );

        void flushFiles( bool sync ) { return _extentManager.flushFiles( sync ); }

        /**
         * @return true if ns is part of the database
         *         ns=foo.bar, db=foo returns true
         */
        bool ownsNS( const string& ns ) const {
            if ( ! startsWith( ns , _name ) )
                return false;
            return ns[_name.size()] == '.';
        }

        const RecordStats& recordStats() const { return _recordStats; }
        RecordStats& recordStats() { return _recordStats; }

        int getProfilingLevel() const { return _profile; }
        const char* getProfilingNS() const { return _profileName.c_str(); }

        const NamespaceIndex& namespaceIndex() const { return _namespaceIndex; }
        NamespaceIndex& namespaceIndex() { return _namespaceIndex; }

        // TODO: do not think this method should exist, so should try and encapsulate better
        ExtentManager& getExtentManager() { return _extentManager; }
        const ExtentManager& getExtentManager() const { return _extentManager; }

        Status dropCollection( const StringData& fullns );

        Collection* createCollection( const StringData& ns,
                                      const CollectionOptions& options = CollectionOptions(),
                                      bool allocateSpace = true,
                                      bool createDefaultIndexes = true );

        /**
         * @param ns - this is fully qualified, which is maybe not ideal ???
         */
        Collection* getCollection( const StringData& ns );

        Collection* getCollection( const NamespaceString& ns ) { return getCollection( ns.ns() ); }

        Collection* getOrCreateCollection( const StringData& ns );

        Status renameCollection( const StringData& fromNS, const StringData& toNS, bool stayTemp );

        /**
         * @return name of an existing database with same text name but different
         * casing, if one exists.  Otherwise the empty string is returned.  If
         * 'duplicates' is specified, it is filled with all duplicate names.
         */
        static string duplicateUncasedName( bool inholderlockalready, const string &name, const string &path, set< string > *duplicates = 0 );

        static Status validateDBName( const StringData& dbname );

        const string& getSystemIndexesName() const { return _indexesName; }
    private:

        void _clearCollectionCache( const StringData& fullns );

        void _clearCollectionCache_inlock( const StringData& fullns );

        ~Database(); // closes files and other cleanup see below.

        void _addNamespaceToCatalog( const StringData& ns, const BSONObj* options );


        /**
         * removes from *.system.namespaces
         * frees extents
         * removes from NamespaceIndex
         * NOT RIGHT NOW, removes cache entry in Database TODO?
         */
        Status _dropNS( const StringData& ns );

        /**
         * @throws DatabaseDifferCaseCode if the name is a duplicate based on
         * case insensitive matching.
         */
        void checkDuplicateUncasedNames(bool inholderlockalready) const;

        void openAllFiles();

        Status _renameSingleNamespace( const StringData& fromNS, const StringData& toNS,
                                       bool stayTemp );

        const string _name; // "alleyinsider"
        const string _path; // "/data/db"

        NamespaceIndex _namespaceIndex;
        ExtentManager _extentManager;

        const string _profileName; // "alleyinsider.system.profile"
        const string _namespacesName; // "alleyinsider.system.namespaces"
        const string _indexesName; // "alleyinsider.system.indexes"

        RecordStats _recordStats;
        int _profile; // 0=off.

        int _magic; // used for making sure the object is still loaded in memory

        // TODO: make sure deletes go through
        // this in some ways is a dupe of _namespaceIndex
        // but it points to a much more useful data structure
        typedef StringMap< Collection* > CollectionMap;
        CollectionMap _collections;
        mutex _collectionLock;

        friend class Collection;
        friend class NamespaceDetails;
        friend class IndexDetails;
        friend class IndexCatalog;
    };

} // namespace mongo
