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

// TODO: Remove
#include "mongo/pch.h"

#include "mongo/db/client.h"
#include "mongo/db/db.h"
#include "mongo/db/keypattern.h"
#include "mongo/s/range_arithmetic.h"

namespace mongo {

    extern const BSONObj reverseNaturalObj; // {"$natural": -1 }

    class Cursor;
    class CoveredIndexMatcher;

    /**
     * db helpers are helper functions and classes that let us easily manipulate the local
     * database instance in-proc.
     *
     * all helpers assume locking is handled above them
     */
    struct Helpers {

        class RemoveSaver;

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
        static bool findOne(const StringData& ns, const BSONObj &query, BSONObj& result, bool requireIndex = false);
        static DiskLoc findOne(const StringData& ns, const BSONObj &query, bool requireIndex);

        /**
         * have to be locked already
         */
        static vector<BSONObj> findAll( const string& ns , const BSONObj& query );

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
        static bool isEmpty(const char *ns);

        // TODO: this should be somewhere else probably
        /* Takes object o, and returns a new object with the
         * same field elements but the names stripped out.
         * Example:
         *    o = {a : 5 , b : 6} --> {"" : 5, "" : 6}
         */
        static BSONObj toKeyFormat( const BSONObj& o );

        /* Takes object o, and infers an ascending keyPattern with the same fields as o
         * Example:
         *    o = {a : 5 , b : 6} --> {a : 1 , b : 1 }
         */
        static BSONObj inferKeyPattern( const BSONObj& o );

        /**
         * Takes a namespace range, specified by a min and max and qualified by an index pattern,
         * and removes all the documents in that range found by iterating
         * over the given index. Caller is responsible for insuring that min/max are
         * compatible with the given keyPattern (e.g min={a:100} is compatible with
         * keyPattern={a:1,b:1} since it can be extended to {a:100,b:minKey}, but
         * min={b:100} is not compatible).
         *
         * Caller must hold a write lock on 'ns'
         *
         * Returns -1 when no usable index exists
         *
         * Does oplog the individual document deletions.
         * // TODO: Refactor this mechanism, it is growing too large
         */
        static long long removeRange( const KeyRange& range,
                                      bool maxInclusive = false,
                                      bool secondaryThrottle = false,
                                      RemoveSaver* callback = NULL,
                                      bool fromMigrate = false,
                                      bool onlyRemoveOrphanedDocs = false );


        // TODO: This will supersede Chunk::MaxObjectsPerChunk
        static const long long kMaxDocsPerChunk;

        /**
         * Get sorted disklocs that belong to a range of a namespace defined over an index
         * key pattern (KeyRange).
         *
         * @param chunk range of a namespace over an index key pattern.
         * @param maxChunkSizeBytes max number of bytes that we will retrieve locs for, if the
         * range is estimated larger (from avg doc stats) we will stop recording locs.
         * @param locs set to record locs in
         * @param estChunkSizeBytes chunk size estimated from doc count and avg doc size
         * @param chunkTooBig whether the chunk was estimated larger than our maxChunkSizeBytes
         * @param errmsg filled with textual description of error if this call return false
         *
         * @return NamespaceNotFound if the namespace doesn't exist
         * @return IndexNotFound if the index pattern doesn't match any indexes
         * @return InvalidLength if the estimated size exceeds maxChunkSizeBytes
         */
        static Status getLocsInRange( const KeyRange& range,
                                      long long maxChunkSizeBytes,
                                      set<DiskLoc>* locs,
                                      long long* numDocs,
                                      long long* estChunkSizeBytes );

        /**
         * Remove all documents from a collection.
         * You do not need to set the database before calling.
         * Does not oplog the operation.
         */
        static void emptyCollection(const char *ns);

        /**
         * for saving deleted bson objects to a flat file
         */
        class RemoveSaver : public boost::noncopyable {
        public:
            RemoveSaver(const string& type, const string& ns, const string& why);
            ~RemoveSaver();

            void goingToDelete( const BSONObj& o );

        private:
            boost::filesystem::path _root;
            boost::filesystem::path _file;
            ofstream* _out;
        };

    };

} // namespace mongo
