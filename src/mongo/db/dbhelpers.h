/* @file dbhelpers.h

   db helpers are helper functions and classes that let us easily manipulate the local
   database instance in-proc.
*/

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

#include "../pch.h"
#include "client.h"
#include "db.h"

namespace mongo {

    const BSONObj reverseNaturalObj = BSON( "$natural" << -1 );

    class Cursor;
    class CoveredIndexMatcher;

    /**
       all helpers assume locking is handled above them
     */
    struct Helpers {

        /* ensure the specified index exists.

           @param keyPattern key pattern, e.g., { ts : 1 }
           @param name index name, e.g., "name_1"

           This method can be a little (not much) cpu-slow, so you may wish to use
             OCCASIONALLY ensureIndex(...);

           Note: use ensureHaveIdIndex() for the _id index: it is faster.
           Note: does nothing if collection does not yet exist.
        */
        static void ensureIndex(const char *ns, BSONObj keyPattern, bool unique, const char *name);

        /* fetch a single object from collection ns that matches query.
           set your db SavedContext first.

           @param query - the query to perform.  note this is the low level portion of query so "orderby : ..."
                          won't work.

           @param requireIndex if true, assert if no index for the query.  a way to guard against
           writing a slow query.

           @return true if object found
        */
        static bool findOne(const char *ns, const BSONObj &query, BSONObj& result, bool requireIndex = false);
        static DiskLoc findOne(const char *ns, const BSONObj &query, bool requireIndex);

        /**
         * @param foundIndex if passed in will be set to 1 if ns and index found
         * @return true if object found
         */
        static bool findById(Client&, const char *ns, BSONObj query, BSONObj& result ,
                             bool * nsFound = 0 , bool * indexFound = 0 );

        /* uasserts if no _id index.
           @return null loc if not found */
        static DiskLoc findById(NamespaceDetails *d, BSONObj query);

        /** Get/put the first (or last) object from a collection.  Generally only useful if the collection
            only ever has a single object -- which is a "singleton collection".

            You do not need to set the database (Context) before calling.

            @return true if object exists.
        */
        static bool getSingleton(const char *ns, BSONObj& result);
        static void putSingleton(const char *ns, BSONObj obj);
        static void putSingletonGod(const char *ns, BSONObj obj, bool logTheOp);
        static bool getFirst(const char *ns, BSONObj& result) { return getSingleton(ns, result); }
        static bool getLast(const char *ns, BSONObj& result); // get last object int he collection; e.g. {$natural : -1}

        /**
         * you have to lock
         * you do not have to have Context set
         * o has to have an _id field or will assert
         */
        static void upsert( const string& ns , const BSONObj& o, bool fromMigrate = false );

        /** You do not need to set the database before calling.
            @return true if collection is empty.
        */
        static bool isEmpty(const char *ns, bool doAuth=true);

        // TODO: this should be somewhere else probably
        static BSONObj toKeyFormat( const BSONObj& o , BSONObj& key );

        class RemoveCallback {
        public:
            virtual ~RemoveCallback() {}
            virtual void goingToDelete( const BSONObj& o ) = 0;
        };

        /**
         * Remove all documents in the range.
         * Does oplog the individual document deletions.
         */
        static long long removeRange( const string& ns , 
                                      const BSONObj& min , 
                                      const BSONObj& max , 
                                      bool yield = false , 
                                      bool maxInclusive = false , 
                                      RemoveCallback * callback = 0, 
                                      bool fromMigrate = false );

        /**
         * Remove all documents from a collection.
         * You do not need to set the database before calling.
         * Does not oplog the operation.
         */
        static void emptyCollection(const char *ns);

    };

    /**
     * user for saving deleted bson objects to a flat file
     */
    class RemoveSaver : public Helpers::RemoveCallback , boost::noncopyable {
    public:
        RemoveSaver( const string& type , const string& ns , const string& why);
        ~RemoveSaver();

        void goingToDelete( const BSONObj& o );

    private:
        boost::filesystem::path _root;
        boost::filesystem::path _file;
        ofstream* _out;

    };


} // namespace mongo
