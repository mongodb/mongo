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

#include "mongo/db/cc_by_loc.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/storage/extent_manager.h"

namespace mongo {

    class CollectionTemp;
    class Extent;
    class DataFile;

    /**
     * Database represents a database database
     * Each database database has its own set of files -- dbname.ns, dbname.0, dbname.1, ...
     * NOT memory mapped
    */
    class Database {
    public:
        // you probably need to be in dbHolderMutex when constructing this
        Database(const char *nm, /*out*/ bool& newDb, const string& path = dbpath);

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

        /**
         * return file n.  if it doesn't exist, create it
         */
        DataFile* getFile( int n, int sizeNeeded = 0, bool preallocateOnly = false ) {
            _namespaceIndex.init();
            return _extentManager.getFile( n, sizeNeeded, preallocateOnly );
        }

        DataFile* addAFile( int sizeNeeded, bool preallocateNextFile ) {
            return _extentManager.addAFile( sizeNeeded, preallocateNextFile );
        }

        /**
         * makes sure we have an extra file at the end that is empty
         * safe to call this multiple times - the implementation will only preallocate one file
         */
        void preallocateAFile() { _extentManager.preallocateAFile(); }

        Extent* allocExtent( const char *ns, int size, bool capped, bool enforceQuota );

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

        CCByLoc& ccByLoc() { return _ccByLoc; }

        const NamespaceIndex& namespaceIndex() const { return _namespaceIndex; }
        NamespaceIndex& namespaceIndex() { return _namespaceIndex; }

        // TODO: do not think this method should exist, so should try and encapsulate better
        ExtentManager& getExtentManager() { return _extentManager; }
        const ExtentManager& getExtentManager() const { return _extentManager; }

        void dropCollection( const StringData& fullns );

        /**
         * @param ns - this is fully qualified, which is maybe not ideal ???
         */
        CollectionTemp* getCollectionTemp( const StringData& ns );

        Status renameCollection( const StringData& fromNS, const StringData& toNS, bool stayTemp );

        /**
         * @return name of an existing database with same text name but different
         * casing, if one exists.  Otherwise the empty string is returned.  If
         * 'duplicates' is specified, it is filled with all duplicate names.
         */
        static string duplicateUncasedName( bool inholderlockalready, const string &name, const string &path, set< string > *duplicates = 0 );

        static Status validateDBName( const StringData& dbname );

    private:

        ~Database(); // closes files and other cleanup see below.

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

        CCByLoc _ccByLoc; // use by ClientCursor

        RecordStats _recordStats;
        int _profile; // 0=off.

        int _magic; // used for making sure the object is still loaded in memory

        // TODO: probably shouldn't be a std::map
        // TODO: make sure deletes go through
        // this in some ways is a dupe of _namespaceIndex
        // but it points to a much more useful data structure
        typedef std::map< std::string, CollectionTemp* > CollectionMap;
        CollectionMap _collections;
        mutex _collectionLock;

    };

} // namespace mongo
