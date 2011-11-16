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
*/

#pragma once

#include "cmdline.h"

namespace mongo {

    class ClientCursor;
    struct ByLocKey;
    typedef map<ByLocKey, ClientCursor*> CCByLoc;

    /**
     * Database represents a database database
     * Each database database has its own set of files -- dbname.ns, dbname.0, dbname.1, ...
     * NOT memory mapped
    */
    class Database {
    public:
        static bool _openAllFiles;

        Database(const char *nm, /*out*/ bool& newDb, const string& _path = dbpath);
    private:
        ~Database();
    public:
        /* you must use this to close - there is essential code in this method that is not in the ~Database destructor.
           thus the destructor is private.  this could be cleaned up one day...
        */
        static void closeDatabase( const char *db, const string& path );

        void openAllFiles();

        /**
         * tries to make sure that this hasn't been deleted
         */
        bool isOk() const { return magic == 781231; }

        bool isEmpty() { return ! namespaceIndex.allocated(); }

        /**
         * total file size of Database in bytes
         */
        long long fileSize() const;

        int numFiles() const { return (int)files.size(); }

        /**
         * returns file valid for file number n
         */
        boost::filesystem::path fileName( int n ) const;

        bool exists(int n) const { return boost::filesystem::exists( fileName( n ) ); }

        /**
         * return file n.  if it doesn't exist, create it
         */
        MongoDataFile* getFile( int n, int sizeNeeded = 0, bool preallocateOnly = false );

        MongoDataFile* addAFile( int sizeNeeded, bool preallocateNextFile );

        /**
         * makes sure we have an extra file at the end that is empty
         * safe to call this multiple times - the implementation will only preallocate one file
         */
        void preallocateAFile() { getFile( numFiles() , 0, true ); }

        MongoDataFile* suitableFile( const char *ns, int sizeNeeded, bool preallocate, bool enforceQuota );

        Extent* allocExtent( const char *ns, int size, bool capped, bool enforceQuota );

        MongoDataFile* newestFile();

        /**
         * @return true if success.  false if bad level or error creating profile ns
         */
        bool setProfilingLevel( int newLevel , string& errmsg );

        void flushFiles( bool sync ) const;

        /**
         * @return true if ns is part of the database
         *         ns=foo.bar, db=foo returns true
         */
        bool ownsNS( const string& ns ) const {
            if ( ! startsWith( ns , name ) )
                return false;
            return ns[name.size()] == '.';
        }

        // TODO: remove this method entirely
        static bool validDBName( const string& ns ) {
            return NamespaceString::validDBName( ns );
        }
        
        /**
         * @throws DatabaseDifferCaseCode if the name is a duplicate based on
         * case insensitive matching.
         */
        void checkDuplicateUncasedNames() const;

        /**
         * @return name of an existing database with same text name but different
         * casing, if one exists.  Otherwise the empty string is returned.  If
         * 'duplicates' is specified, it is filled with all duplicate names.
         */
        static string duplicateUncasedName( const string &name, const string &path, set< string > *duplicates = 0 );
        
    public: // this should be private later

        vector<MongoDataFile*> files;
        const string name; // "alleyinsider"
        const string path;
        NamespaceIndex namespaceIndex;
        int profile; // 0=off.
        const string profileName; // "alleyinsider.system.profile"
        CCByLoc ccByLoc;
        int magic; // used for making sure the object is still loaded in memory
    };

} // namespace mongo
