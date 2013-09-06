// index.h

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

#include "mongo/pch.h"

#include <vector>

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/key.h"
#include "mongo/db/storage/namespace.h"

namespace mongo {

    /* Details about a particular index. There is one of these effectively for each object in
       system.namespaces (although this also includes the head pointer, which is not in that
       collection).

       ** MemoryMapped Record ** (i.e., this is on disk data)
     */
    class IndexDetails {
    public:
        /**
         * btree head disk location
         * TODO We should make this variable private, since btree operations
         * may change its value and we don't want clients to rely on an old
         * value.  If we create a btree class, we can provide a btree object
         * to clients instead of 'head'.
         */
        DiskLoc head;

        /* Location of index info object. Format:

             { name:"nameofindex", ns:"parentnsname", key: {keypattobject}
               [, unique: <bool>, background: <bool>, v:<version>]
             }

           This object is in the system.indexes collection.  Note that since we
           have a pointer to the object here, the object in system.indexes MUST NEVER MOVE.
        */
        DiskLoc info;

        /* extract key value from the query object
           e.g., if key() == { x : 1 },
                 { x : 70, y : 3 } -> { x : 70 }
        */
        BSONObj getKeyFromQuery(const BSONObj& query) const {
            BSONObj k = keyPattern();
            BSONObj res = query.extractFieldsUnDotted(k);
            return res;
        }

        /* get the key pattern for this object.
           e.g., { lastname:1, firstname:1 }
        */
        BSONObj keyPattern() const {
            return info.obj().getObjectField("key");
        }

        /**
         * @return offset into keyPattern for key
                   -1 if doesn't exist
         */
        int keyPatternOffset( const string& key ) const;
        bool inKeyPattern( const string& key ) const { return keyPatternOffset( key ) >= 0; }

        /* true if the specified key is in the index */
        bool hasKey(const BSONObj& key);

        // returns name of this index's storage area (database.collection.$index)
        string indexNamespace() const {
            return indexNamespaceFromObj(info.obj());
        }

        // returns the name of an index's storage area (database.collection.$index) from a BSONObj
        static string indexNamespaceFromObj(const BSONObj& io) {
            string s;
            s.reserve(Namespace::MaxNsLen);
            s = io.getStringField("ns");
            verify( !s.empty() );
            s += ".$";
            s += io.getStringField("name");
            return s;
        }


        string indexName() const { // e.g. "ts_1"
            BSONObj io = info.obj();
            return io.getStringField("name");
        }

        static bool isIdIndexPattern( const BSONObj &pattern ) {
            BSONObjIterator i(pattern);
            BSONElement e = i.next();
            //_id index must have form exactly {_id : 1} or {_id : -1}.
            //Allows an index of form {_id : "hashed"} to exist but
            //do not consider it to be the primary _id index
            if(! ( strcmp(e.fieldName(), "_id") == 0
                    && (e.numberInt() == 1 || e.numberInt() == -1)))
                return false;
            return i.next().eoo();
        }

        /* returns true if this is the _id index. */
        bool isIdIndex() const {
            return isIdIndexPattern( keyPattern() );
        }

        /* gets not our namespace name (indexNamespace for that),
           but the collection we index, its name.
           */
        string parentNS() const {
            BSONObj io = info.obj();
            return io.getStringField("ns");
        }

        static int versionForIndexObj( const BSONObj &obj ) {
            BSONElement e = obj["v"];
            if( e.type() == NumberInt ) 
                return e._numberInt();
            // should normally be an int.  this is for backward compatibility
            int v = e.numberInt();
            uassert(14802, "index v field should be Integer type", v == 0);
            return v;            
        }
        
        int version() const {
            return versionForIndexObj( info.obj() );
        }

        /** @return true if index has unique constraint */
        bool unique() const {
            BSONObj io = info.obj();
            return io["unique"].trueValue() ||
                   /* temp: can we juse make unique:true always be there for _id and get rid of this? */
                   isIdIndex();
        }

        /** return true if dropDups was set when building index (if any duplicates, dropdups drops the duplicating objects) */
        bool dropDups() const {
            return info.obj().getBoolField( "dropDups" );
        }

        /** delete this index.  does NOT clean up the system catalog
            (system.indexes or system.namespaces) -- only NamespaceIndex.
        */
        void kill_idx();

        string toString() const {
            return info.obj().toString();
        }

        /** @return true if supported.  supported means we can use the index, including adding new keys.
                    it may not mean we can build the index version in question: we may not maintain building 
                    of indexes in old formats in the future.
        */
        static bool isASupportedIndexVersionNumber(int v) { return (v&1)==v; } // v == 0 || v == 1
    };

    class NamespaceDetails;
    // changedId should be initialized to false
    // @return how many things were deleted
    int assureSysIndexesEmptied(const char *ns, IndexDetails *exceptForIdIndex);
    int removeFromSysIndexes(const char *ns, const char *idxName);

    /**
     * Prepare to build an index.  Does not actually build it (except for a special _id case).
     * - We validate that the params are good
     * - That the index does not already exist
     * - Creates the source collection if it DNE
     *
     * example of 'io':
     *   { ns : 'test.foo', name : 'z', key : { z : 1 } }
     *
     * @throws DBException
     *
     * @param mayInterrupt - When true, killop may interrupt the function call.
     * @param sourceNS - source NS we are indexing
     * @param sourceCollection - its details ptr
     * @return true if ok to continue.  when false we stop/fail silently (index already exists)
     */
    bool prepareToBuildIndex(const BSONObj& io,
                             bool mayInterrupt,
                             bool god,
                             string& sourceNS,
                             NamespaceDetails*& sourceCollection,
                             BSONObj& fixedIndexObject);

} // namespace mongo
