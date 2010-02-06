// dbhelpers.h

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

/* db helpers are helper functions and classes that let us easily manipulate the local
   database instance.
*/

#pragma once

namespace mongo {

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

           @param requireIndex if true, complain if no index for the query.  a way to guard against
           writing a slow query.

           @return true if object found
        */
        static bool findOne(const char *ns, BSONObj query, BSONObj& result, bool requireIndex = false);
        
        /**
           -1 no index
           0 not found
           1 found
         */
        static int findById(const char *ns, BSONObj query, BSONObj& result );

        /* Get/put the first object from a collection.  Generally only useful if the collection
           only ever has a single object -- which is a "singleton collection".

		   You do not need to set the database before calling.
		   
		   Returns: true if object exists.
        */
        static bool getSingleton(const char *ns, BSONObj& result);
        static void putSingleton(const char *ns, BSONObj obj);


        /* Remove all objects from a collection.
        You do not need to set the database before calling.
        */
        static void emptyCollection(const char *ns);

    };

    class Database;

    /* Set database we want to use, then, restores when we finish (are out of scope)
       Note this is also helpful if an exception happens as the state if fixed up.
    */
    class DBContext {
        Database *olddb;
        string oldns;
    public:
        DBContext(const char *ns) {
            olddb = cc().database();
            oldns = cc().ns();
            setClient(ns);
        }
        DBContext(string ns) {
            olddb = cc().database();
            oldns = cc().ns();
            setClient(ns.c_str());
        }

        /* this version saves the context but doesn't yet set the new one: */
        DBContext() {
            olddb = cc().database();
            oldns = cc().ns();        }


        ~DBContext() {
            cc().setns(oldns.c_str(), olddb);
        }
    };

    // manage a set using collection backed storage
    class DbSet {
    public:
        DbSet( const string &name = "", const BSONObj &key = BSONObj() ) :
        name_( name ),
        key_( key.getOwned() ) {
        }
        ~DbSet();
        void reset( const string &name = "", const BSONObj &key = BSONObj() );
        bool get( const BSONObj &obj ) const;
        void set( const BSONObj &obj, bool val );
    private:
        string name_;
        BSONObj key_;
    };
    
} // namespace mongo
