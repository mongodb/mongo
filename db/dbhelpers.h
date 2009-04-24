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
           set your db context first.

           @param requireIndex if true, complain if no index for the query.  a way to guard against
           writing a slow query.

           @return true if object found
        */
        static bool findOne(const char *ns, BSONObj query, BSONObj& result, bool requireIndex = false);

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

    /* Set database we want to use, then, restores when we finish (are out of scope)
       Note this is also helpful if an exception happens as the state if fixed up.
    */
    class DBContext {
        Database *old;
    public:
        DBContext(const char *ns) {
            old = database;
            setClientTempNs(ns);
        }
        DBContext(string ns) {
            old = database;
            setClientTempNs(ns.c_str());
        }
        ~DBContext() {
            database = old;
        }
    };

} // namespace mongo
