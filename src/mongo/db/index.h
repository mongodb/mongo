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
*/

#pragma once

#include "pch.h"

#include <vector>

#include "mongo/db/diskloc.h"
#include "mongo/db/index_insertion_continuation.h"
#include "mongo/db/indexkey.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/key.h"
#include "mongo/db/namespace.h"

namespace mongo {

    class IndexInterface {
    protected:
        virtual ~IndexInterface() { }
    public:
        class IndexInserter : private boost::noncopyable {
        public:
            IndexInserter();
            ~IndexInserter();

            void addInsertionContinuation(IndexInsertionContinuation *c);
            void finishAllInsertions();

        private:
            std::vector<IndexInsertionContinuation *> _continuations;
        };

        virtual IndexInsertionContinuation *beginInsertIntoIndex(
            int idxNo,
            IndexDetails &_idx, DiskLoc _recordLoc, const BSONObj &_key,
            const Ordering& _order, bool dupsAllowed) = 0;

        virtual int keyCompare(const BSONObj& l,const BSONObj& r, const Ordering &ordering) = 0;
        virtual long long fullValidate(const DiskLoc& thisLoc, const BSONObj &order) = 0;
        virtual DiskLoc findSingle(const IndexDetails &indexdetails , const DiskLoc& thisLoc, const BSONObj& key) const = 0;
        virtual bool unindex(const DiskLoc thisLoc, IndexDetails& id, const BSONObj& key, const DiskLoc recordLoc) const = 0;
        virtual int bt_insert(const DiskLoc thisLoc, const DiskLoc recordLoc,
            const BSONObj& key, const Ordering &order, bool dupsAllowed,
            IndexDetails& idx, bool toplevel = true) const = 0;
        virtual DiskLoc addBucket(const IndexDetails&) = 0;
        virtual void uassertIfDups(IndexDetails& idx, vector<BSONObj*>& addedKeys, DiskLoc head, 
            DiskLoc self, const Ordering& ordering) = 0;

        // these are for geo
        virtual bool isUsed(DiskLoc thisLoc, int pos) = 0;
        virtual void keyAt(DiskLoc thisLoc, int pos, BSONObj&, DiskLoc& recordLoc) = 0;
        virtual BSONObj keyAt(DiskLoc thisLoc, int pos) = 0;
        virtual DiskLoc locate(const IndexDetails &idx , const DiskLoc& thisLoc, const BSONObj& key, const Ordering &order,
                               int& pos, bool& found, const DiskLoc &recordLoc, int direction=1) = 0;
        virtual DiskLoc advance(const DiskLoc& thisLoc, int& keyOfs, int direction, const char *caller) = 0;
    };

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

        /* pull out the relevant key objects from obj, so we
           can index them.  Note that the set is multiple elements
           only when it's a "multikey" array.
           keys will be left empty if key not found in the object.
        */
        void getKeysFromObject( const BSONObj& obj, BSONObjSet& keys) const;

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

        // returns name of this index's storage area
        // database.table.$index
        string indexNamespace() const {
            BSONObj io = info.obj();
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
            if( strcmp(e.fieldName(), "_id") != 0 ) return false;
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

        const IndexSpec& getSpec() const;

        string toString() const {
            return info.obj().toString();
        }

        /** @return true if supported.  supported means we can use the index, including adding new keys.
                    it may not mean we can build the index version in question: we may not maintain building 
                    of indexes in old formats in the future.
        */
        static bool isASupportedIndexVersionNumber(int v) { return (v&1)==v; } // v == 0 || v == 1

        /** @return the interface for this interface, which varies with the index version.
            used for backward compatibility of index versions/formats.
        */
        IndexInterface& idxInterface() const { 
            int v = version();
            dassert( isASupportedIndexVersionNumber(v) );
            return *iis[v&1];
        }

        static IndexInterface *iis[];
    };

    struct IndexChanges { /*on an update*/
        BSONObjSet oldkeys;
        BSONObjSet newkeys;
        vector<BSONObj*> removed; // these keys were removed as part of the change
        vector<BSONObj*> added;   // these keys were added as part of the change

        /** @curObjLoc - the object we want to add's location.  if it is already in the
                         index, that is allowed here (for bg indexing case).
        */
        void dupCheck(IndexDetails& idx, DiskLoc curObjLoc) {
            if( added.empty() || !idx.unique() )
                return;
            const Ordering ordering = Ordering::make(idx.keyPattern());
            idx.idxInterface().uassertIfDups(idx, added, idx.head, curObjLoc, ordering); // "E11001 duplicate key on update"
        }
    };

    class NamespaceDetails;
    // changedId should be initialized to false
    void getIndexChanges(vector<IndexChanges>& v, const char *ns, NamespaceDetails& d,
                         BSONObj newObj, BSONObj oldObj, bool &cangedId);
    void dupCheck(vector<IndexChanges>& v, NamespaceDetails& d, DiskLoc curObjLoc);
} // namespace mongo
