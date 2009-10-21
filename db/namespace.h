// namespace.h

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
#include "jsobj.h"
#include "queryutil.h"
#include "storage.h"
#include "../util/hashtab.h"
#include "../util/mmap.h"

namespace mongo {

    class Cursor;

#pragma pack(1)

	/* in the mongo source code, "client" means "database". */

    const int MaxClientLen = 256; // max str len for the db name, including null char

	// "database.a.b.c" -> "database"
    inline void nsToClient(const char *ns, char *database) {
        const char *p = ns;
        char *q = database;
        while ( *p != '.' ) {
            if ( *p == 0 )
                break;
            *q++ = *p++;
        }
        *q = 0;
        if (q-database>=MaxClientLen) {
            log() << "nsToClient: ns too long. terminating, buf overrun condition" << endl;
            dbexit( EXIT_POSSIBLE_CORRUPTION );
        }
    }
    inline string nsToClient(const char *ns) {
        char buf[MaxClientLen];
        nsToClient(ns, buf);
        return buf;
    }

	/* e.g.
	   NamespaceString ns("acme.orders");
	   cout << ns.coll; // "orders"
	*/
    class NamespaceString {
    public:
        string db;
        string coll; // note collection names can have periods in them for organizing purposes (e.g. "system.indexes")
    private:
        void init(const char *ns) { 
            const char *p = strchr(ns, '.');
            if( p == 0 ) return;
            db = string(ns, p - ns);
            coll = p + 1;
        }
    public:
        NamespaceString( const char * ns ) { init(ns); }
        NamespaceString( const string& ns ) { init(ns.c_str()); }

        bool isSystem() { 
            return strncmp(coll.c_str(), "system.", 7) == 0;
        }
    };

	/* This helper class is used to make the HashMap below in NamespaceDetails */
    class Namespace {
    public:
        enum MaxNsLenValue { MaxNsLen = 128 };
        Namespace(const char *ns) {
            *this = ns;
        }
        Namespace& operator=(const char *ns) {
            uassert("ns name too long, max size is 128", strlen(ns) < MaxNsLen);
            //memset(buf, 0, MaxNsLen); /* this is just to keep stuff clean in the files for easy dumping and reading */
            strcpy_s(buf, MaxNsLen, ns);
            return *this;
        }

        /* for more than 10 indexes -- see NamespaceDetails::Extra */
        string extraName() { 
            string s = string(buf) + "$extra";
            massert("ns name too long", s.size() < MaxNsLen);
            return s;
        }

        void kill() {
            buf[0] = 0x7f;
        }

        bool operator==(const char *r) {
            return strcmp(buf, r) == 0;
        }
        bool operator==(const Namespace& r) {
            return strcmp(buf, r.buf) == 0;
        }
        int hash() const {
            unsigned x = 0;
            const char *p = buf;
            while ( *p ) {
                x = x * 131 + *p;
                p++;
            }
            return (x & 0x7fffffff) | 0x8000000; // must be > 0
        }

        /**
           ( foo.bar ).getSisterNS( "blah" ) == foo.blah
		   perhaps this should move to the NamespaceString helper?
         */
        string getSisterNS( const char * local ) {
            assert( local && local[0] != '.' );
            string old(buf);
            if ( old.find( "." ) != string::npos )
                old = old.substr( 0 , old.find( "." ) );
            return old + "." + local;
        }

        char buf[MaxNsLen];
    };

    /**
       @return true if a client can modify this namespace
       things like *.system.users
     */
    bool legalClientSystemNS( const string& ns , bool write );


    /* deleted lists -- linked lists of deleted records -- are placed in 'buckets' of various sizes
       so you can look for a deleterecord about the right size.
    */
    const int Buckets = 19;
    const int MaxBucket = 18;

	/* Maximum # of indexes per collection.  We need to raise this limit at some point.  
	   (Backward datafile compatibility is main issue with changing.)
	*/
//    const int MaxIndexes = 10;

	/* Details about a particular index. There is one of these effectively for each object in 
	   system.namespaces (although this also includes the head pointer, which is not in that 
	   collection).
	 */
    class IndexDetails {
    public:
        DiskLoc head; /* btree head disk location */

        /* Location of index info object. Format:

             { name:"nameofindex", ns:"parentnsname", key: {keypattobject}[, unique: <bool>] }

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

    extern int bucketSizes[];

    /* this is the "header" for a collection that has all its details.  in the .ns file.
    */
    class NamespaceDetails {
        friend class NamespaceIndex;
        enum { NIndexesExtra = 30,
               NIndexesBase  = 10
        };
        struct Extra { 
            // note we could use this field for more chaining later, so don't waste it:
            unsigned long long reserved1; 
            IndexDetails details[NIndexesExtra];
            unsigned reserved2;
            unsigned reserved3;
        };
        Extra* extra() { 
            assert( extraOffset );
            return (Extra *) (((char *) this) + extraOffset);
        }
    public:
        void copyingFrom(const char *thisns, NamespaceDetails *src); // must be called when renaming a NS to fix up extra

        enum { NIndexesMax = 40 };

        BOOST_STATIC_ASSERT( NIndexesMax == NIndexesBase + NIndexesExtra );

        NamespaceDetails( const DiskLoc &loc, bool _capped ) {
            /* be sure to initialize new fields here -- doesn't default to zeroes the way we use it */
            firstExtent = lastExtent = capExtent = loc;
            datasize = nrecords = 0;
            lastExtentSize = 0;
            nIndexes = 0;
            capped = _capped;
            max = 0x7fffffff;
            paddingFactor = 1.0;
            flags = 0;
            capFirstNewRecord = DiskLoc();
            // Signal that we are on first allocation iteration through extents.
            capFirstNewRecord.setInvalid();
            // For capped case, signal that we are doing initial extent allocation.
            if ( capped )
                deletedList[ 1 ].setInvalid();
			assert( sizeof(dataFileVersion) == 2 );
			dataFileVersion = 0;
			indexFileVersion = 0;
            multiKeyIndexBits = 0;
            reservedA = 0;
            extraOffset = 0;
            memset(reserved, 0, sizeof(reserved));
        }
        DiskLoc firstExtent;
        DiskLoc lastExtent;
        DiskLoc deletedList[Buckets];
        long long datasize;
        long long nrecords;
        int lastExtentSize;
        int nIndexes;
    private:
        IndexDetails _indexes[NIndexesBase];
    public:
        int capped;
        int max; // max # of objects for a capped table.
        double paddingFactor; // 1.0 = no padding.
        int flags;
        DiskLoc capExtent;
        DiskLoc capFirstNewRecord;

        /* NamespaceDetails version.  So we can do backward compatibility in the future.
		   See filever.h
        */
		unsigned short dataFileVersion;
		unsigned short indexFileVersion;

        unsigned long long multiKeyIndexBits;
    private:
        unsigned long long reservedA;
        long long extraOffset; // where the $extra info is located (bytes relative to this)
    public:
        char reserved[80];

        enum NamespaceFlags {
            Flag_HaveIdIndex = 1 << 0, // set when we have _id index (ONLY if ensureIdIndex was called -- 0 if that has never been called)
            Flag_CappedDisallowDelete = 1 << 1 // set when deletes not allowed during capped table allocation.
        };

        IndexDetails& idx(int idxNo) {
            if( idxNo < NIndexesBase ) 
                return _indexes[idxNo];
            return extra()->details[idxNo-NIndexesBase];
        }

        class IndexIterator { 
            friend class NamespaceDetails;
            int i;
            int n;
            NamespaceDetails *d;
            Extra *e;
            IndexIterator(NamespaceDetails *_d) { 
                d = _d;
                i = 0;
                n = d->nIndexes;
                if( n > NIndexesBase )
                    e = d->extra();
            }
        public:
            int pos() { return i; } // note this is the next one to come
            bool more() { return i < n; }
            IndexDetails& next() { 
                int k = i;
                i++;
                return k < NIndexesBase ? d->_indexes[k] : 
                    e->details[k-10];
            }
        };

        IndexIterator ii() { 
            return IndexIterator(this);
        }

        /* hackish - find our index # in the indexes array
        */
        int idxNo(IndexDetails& idx) { 
            IndexIterator i = ii();
            while( i.more() ) {
                if( &i.next() == &idx )
                    return i.pos()-1;
            }
            massert("E12000 idxNo fails", false);
            return -1;
        }

        /* multikey indexes are indexes where there are more than one key in the index
             for a single document. see multikey in wiki.
           for these, we have to do some dedup object on queries.
        */
        bool isMultikey(int i) {
            return (multiKeyIndexBits & (((unsigned long long) 1) << i)) != 0;
        }
        void setIndexIsMultikey(int i) { 
            dassert( i < NIndexesMax );
            multiKeyIndexBits |= (((unsigned long long) 1) << i);
        }
        void clearIndexIsMultikey(int i) { 
            dassert( i < NIndexesMax );
            multiKeyIndexBits &= ~(((unsigned long long) 1) << i);
        }

        /* add a new index.  does not add to system.indexes etc. - just to NamespaceDetails.
           caller must populate returned object. 
         */
        IndexDetails& addIndex(const char *thisns);

        void aboutToDeleteAnIndex() {
            flags &= ~Flag_HaveIdIndex;
        }

        void cappedDisallowDelete() {
            flags |= Flag_CappedDisallowDelete;
        }
        
        /* returns index of the first index in which the field is present. -1 if not present. */
        int fieldIsIndexed(const char *fieldName);

        void paddingFits() {
            double x = paddingFactor - 0.01;
            if ( x >= 1.0 )
                paddingFactor = x;
        }
        void paddingTooSmall() {
            double x = paddingFactor + 0.6;
            if ( x <= 2.0 )
                paddingFactor = x;
        }

        //returns offset in indexes[]
        int findIndexByName(const char *name) {
            IndexIterator i = ii();
            while( i.more() ) {
                if ( strcmp(i.next().info.obj().getStringField("name"),name) == 0 )
                    return i.pos()-1;
            }
            return -1;
        }

        /* @return -1 = not found 
           generally id is first index, so not that expensive an operation (assuming present).
        */
        int findIdIndex() {
            IndexIterator i = ii();
            while( i.more() ) {
                if( i.next().isIdIndex() ) 
                    return i.pos()-1;
            }
            return -1;
        }

        /* return which "deleted bucket" for this size object */
        static int bucket(int n) {
            for ( int i = 0; i < Buckets; i++ )
                if ( bucketSizes[i] > n )
                    return i;
            return Buckets-1;
        }

        /* allocate a new record.  lenToAlloc includes headers. */
        DiskLoc alloc(const char *ns, int lenToAlloc, DiskLoc& extentLoc);

        /* add a given record to the deleted chains for this NS */
        void addDeletedRec(DeletedRecord *d, DiskLoc dloc);

        void dumpDeleted(set<DiskLoc> *extents = 0);

        bool capLooped() const {
            return capped && capFirstNewRecord.isValid();
        }

        // Start from firstExtent by default.
        DiskLoc firstRecord( const DiskLoc &startExtent = DiskLoc() ) const;

        // Start from lastExtent by default.
        DiskLoc lastRecord( const DiskLoc &startExtent = DiskLoc() ) const;

        bool inCapExtent( const DiskLoc &dl ) const;

        void checkMigrate();

        long long storageSize();

    private:
        bool cappedMayDelete() const {
            return !( flags & Flag_CappedDisallowDelete );
        }
        Extent *theCapExtent() const {
            return capExtent.ext();
        }
        void advanceCapExtent( const char *ns );
        void maybeComplain( const char *ns, int len ) const;
        DiskLoc __stdAlloc(int len);
        DiskLoc __capAlloc(int len);
        DiskLoc _alloc(const char *ns, int len);
        void compact();

        DiskLoc &firstDeletedInCapExtent();
        bool nextIsInCapExtent( const DiskLoc &dl ) const;
    };

#pragma pack()

    /* these are things we know / compute about a namespace that are transient -- things
       we don't actually store in the .ns file.  so mainly caching of frequently used
       information.

       CAUTION: Are you maintaining this properly on a collection drop()?  A dropdatabase()?  Be careful.
                The current field "allIndexKeys" may have too many keys in it on such an occurrence;
                as currently used that does not cause anything terrible to happen.
    */
    class NamespaceDetailsTransient : boost::noncopyable {
        string ns;
        bool haveIndexKeys;
        set<string> allIndexKeys;
        void computeIndexKeys();
        int writeCount_;
        map< QueryPattern, pair< BSONObj, long long > > queryCache_;
        string logNS_;
        bool logValid_;
    public:
        NamespaceDetailsTransient(const char *_ns) : ns(_ns), haveIndexKeys(), writeCount_(), logValid_() {
            haveIndexKeys=false; /*lazy load them*/
        }

        /* get set of index keys for this namespace.  handy to quickly check if a given
           field is indexed (Note it might be a seconary component of a compound index.)
        */
        set<string>& indexKeys() {
            if ( !haveIndexKeys ) {
                haveIndexKeys=true;
                computeIndexKeys();
            }
            return allIndexKeys;
        }

        void addedIndex() { reset(); }
        void deletedIndex() { reset(); }
        void registerWriteOp() {
            if ( queryCache_.empty() )
                return;
            if ( ++writeCount_ >= 100 )
                clearQueryCache();
        }
        void clearQueryCache() {
            queryCache_.clear();
            writeCount_ = 0;
        }
        BSONObj indexForPattern( const QueryPattern &pattern ) {
            return queryCache_[ pattern ].first;
        }
        long long nScannedForPattern( const QueryPattern &pattern ) {
            return queryCache_[ pattern ].second;
        }
        void registerIndexForPattern( const QueryPattern &pattern, const BSONObj &indexKey, long long nScanned ) {
            queryCache_[ pattern ] = make_pair( indexKey, nScanned );
        }
        
        void startLog( int logSizeMb = 128 );
        void invalidateLog();
        bool validateCompleteLog();
        string logNS() const { return logNS_; }
        bool logValid() const { return logValid_; }
        
    private:
        void reset();
        void dropLog();
        static std::map< string, shared_ptr< NamespaceDetailsTransient > > map_;
    public:
        static NamespaceDetailsTransient& get(const char *ns);
        // Drop cached information on all namespaces beginning with the specified prefix.
        static void drop(const char *prefix);
    };

    /* NamespaceIndex is the ".ns" file you see in the data directory.  It is the "system catalog"
       if you will: at least the core parts.  (Additional info in system.* collections.)
    */
    class NamespaceIndex {
        friend class NamespaceCursor;
        BOOST_STATIC_ASSERT( sizeof(NamespaceDetails::Extra) <= sizeof(NamespaceDetails) );
    public:
        NamespaceIndex(const string &dir, const string &database) :
        ht( 0 ),
        dir_( dir ),
        database_( database ) {}

        /* returns true if new db will be created if we init lazily */
        bool exists() const;
        
        void init();

        void add_ns(const char *ns, DiskLoc& loc, bool capped) {
            NamespaceDetails details( loc, capped );
			add_ns( ns, details );
        }

		void add_ns( const char *ns, const NamespaceDetails &details ) {
            init();
            Namespace n(ns);
            uassert("too many namespaces/collections", ht->put(n, details));
		}

        /* just for diagnostics */
        size_t detailsOffset(NamespaceDetails *d) {
            if ( !ht )
                return -1;
            return ((char *) d) -  (char *) ht->nodes;
        }

        /* extra space for indexes when more than 10 */
        NamespaceDetails::Extra* allocExtra(const char *ns) { 
            Namespace n(ns);
            Namespace extra(n.extraName().c_str()); // throws userexception if ns name too long
            NamespaceDetails *d = details(ns);
            massert( "allocExtra: base ns missing?", d );
            assert( d->extraOffset == 0 );
            massert( "allocExtra: extra already exists", ht->get(extra) == 0 );
            NamespaceDetails::Extra temp;
            memset(&temp, 0, sizeof(temp));
            uassert( "allocExtra: too many namespaces/collections", ht->put(extra, (NamespaceDetails&) temp));
            NamespaceDetails::Extra *e = (NamespaceDetails::Extra *) ht->get(extra);
            d->extraOffset = ((char *) e) - ((char *) d);
            assert( d->extra() == e );
            return e;
        }

        NamespaceDetails* details(const char *ns) {
            if ( !ht )
                return 0;
            Namespace n(ns);
            NamespaceDetails *d = ht->get(n);
            if ( d )
                d->checkMigrate();
            return d;
        }

        void kill_ns(const char *ns) {
            if ( !ht )
                return;
            Namespace n(ns);
            ht->kill(n);

            try {
                Namespace extra(n.extraName().c_str());
                ht->kill(extra);
            }
            catch(DBException&) { }
        }

        bool find(const char *ns, DiskLoc& loc) {
            NamespaceDetails *l = details(ns);
            if ( l ) {
                loc = l->firstExtent;
                return true;
            }
            return false;
        }

        bool allocated() const {
            return ht != 0;
        }

    private:
        boost::filesystem::path path() const;
        
        MemoryMappedFile f;
        HashTable<Namespace,NamespaceDetails> *ht;
        string dir_;
        string database_;
    };

    extern string dbpath; // --dbpath parm 

    // Rename a namespace within current 'client' db.
    // (Arguments should include db name)
    void renameNamespace( const char *from, const char *to );

} // namespace mongo
