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

#include "../stdafx.h"
#include "diskloc.h"
#include "jsobj.h"
#include <map>

namespace mongo {

    class IndexSpec;
    class IndexType; // TODO: this name sucks

    /**
     * this represents an instance of a index plugin
     * done this way so parsing, etc... can be cached
     * so if there is a FTS IndexPlugin, for each index using FTS
     * there will be 1 of these, and it can have things pre-parsed, etc...
     */
    class IndexType : boost::noncopyable {
    public:
        virtual ~IndexType();
        virtual void getKeys( const BSONObj &obj, BSONObjSetDefaultOrder &keys ) const = 0;
        virtual int compare( const IndexSpec& spec , const BSONObj& l , const BSONObj& r ) const;
    
    };
    
    /**
     * this represents a plugin
     * a plugin could be something like full text search, sparse index, etc...
     * 1 of these exists per type of index per server
     * 1 IndexType is created per index using this plugin
     */
    class IndexPlugin : boost::noncopyable {
    public:
        IndexPlugin( const string& name );
        virtual ~IndexPlugin(){}
        
        virtual IndexType* generate( const IndexSpec& spec ) const = 0;

        static IndexPlugin* get( const string& name ){
            if ( ! _plugins )
                return 0;
            map<string,IndexPlugin*>::iterator i = _plugins->find( name );
            if ( i == _plugins->end() )
                return 0;
            return i->second;
        }
    private:
        string _name;
        static map<string,IndexPlugin*> * _plugins;
    };

    /* precomputed details about an index, used for inserting keys on updates
       stored/cached in NamespaceDetailsTransient, or can be used standalone
       */
    class IndexSpec {
    public:
        BSONObj keyPattern; // e.g., { name : 1 }
        BSONObj info; // this is the same as IndexDetails::info.obj()
        
        IndexSpec(){
        }

        IndexSpec( const BSONObj& k , const BSONObj& m = BSONObj() )
            : keyPattern(k) , info(m){
            _init();
        }

        /**
           this is a DiscLoc of an IndexDetails info
           should have a key field 
         */
        IndexSpec( const DiskLoc& loc ){
            reset( loc );
        }
        
        void reset( const DiskLoc& loc ){
            info = loc.obj();
            keyPattern = info["key"].embeddedObjectUserCheck();
            if ( keyPattern.objsize() == 0 ) {
                out() << info.toString() << endl;
                assert(false);
            }
            _init();
        }
        
        void getKeys( const BSONObj &obj, BSONObjSetDefaultOrder &keys ) const;

    private:
        void _getKeys( vector<const char*> fieldNames , vector<BSONElement> fixed , const BSONObj &obj, BSONObjSetDefaultOrder &keys ) const;

        vector<const char*> _fieldNames;
        vector<BSONElement> _fixed;
        BSONObj _nullKey;
        
        BSONObj _nullObj;
        BSONElement _nullElt;
        
        IndexType * _indexType;
        
        void _init();
    };

	/* Details about a particular index. There is one of these effectively for each object in 
	   system.namespaces (although this also includes the head pointer, which is not in that 
	   collection).

       ** MemoryMapped Record ** (i.e., this is on disk data)
	 */
    class IndexDetails {
    public:
        DiskLoc head; /* btree head disk location */

        /* Location of index info object. Format:

             { name:"nameofindex", ns:"parentnsname", key: {keypattobject}
               [, unique: <bool>, background: <bool>] 
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
        void getKeysFromObject( const BSONObj& obj, BSONObjSetDefaultOrder& keys) const;

        /* get the key pattern for this object.
           e.g., { lastname:1, firstname:1 }
        */
        BSONObj keyPattern() const {
            return info.obj().getObjectField("key");
        }

        /* true if the specified key is in the index */
        bool hasKey(const BSONObj& key);
        bool wouldCreateDup(const BSONObj& key, DiskLoc self);

        // returns name of this index's storage area
        // database.table.$index
        string indexNamespace() const {
            BSONObj io = info.obj();
            string s;
            s.reserve(Namespace::MaxNsLen);
            s = io.getStringField("ns");
            assert( !s.empty() );
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

        bool unique() const { 
            BSONObj io = info.obj();
            return io["unique"].trueValue() || 
                /* temp: can we juse make unique:true always be there for _id and get rid of this? */
                isIdIndex();
        }

        /* if set, when building index, if any duplicates, drop the duplicating object */
        bool dropDups() const {
            return info.obj().getBoolField( "dropDups" );
        }

        /* delete this index.  does NOT clean up the system catalog
           (system.indexes or system.namespaces) -- only NamespaceIndex.
        */
        void kill_idx();

        operator string() const {
            return info.obj().toString();
        }
    };

    struct IndexChanges/*on an update*/ {
        BSONObjSetDefaultOrder oldkeys;
        BSONObjSetDefaultOrder newkeys;
        vector<BSONObj*> removed; // these keys were removed as part of the change
        vector<BSONObj*> added;   // these keys were added as part of the change

        /** @curObjLoc - the object we want to add's location.  if it is already in the 
                         index, that is allowed here (for bg indexing case).
        */
        void dupCheck(IndexDetails& idx, DiskLoc curObjLoc) {
            if( added.empty() || !idx.unique() )
                return;
            for( vector<BSONObj*>::iterator i = added.begin(); i != added.end(); i++ ) {
                bool dup = idx.wouldCreateDup(**i, curObjLoc);
                uassert( 11001 , "E11001 duplicate key on update", !dup);
            }
        }
    };

    class NamespaceDetails;
    void getIndexChanges(vector<IndexChanges>& v, NamespaceDetails& d, BSONObj newObj, BSONObj oldObj);
    void dupCheck(vector<IndexChanges>& v, NamespaceDetails& d, DiskLoc curObjLoc);
} // namespace mongo
