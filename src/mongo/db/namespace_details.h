// namespace_details.h

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

#include "mongo/db/d_concurrency.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/index.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/mongommf.h"
#include "mongo/db/namespace.h"
#include "mongo/db/queryoptimizercursor.h"
#include "mongo/db/querypattern.h"
#include "mongo/util/hashtab.h"

namespace mongo {
    class Database;

    /** @return true if a client can modify this namespace even though it is under ".system."
        For example <dbname>.system.users is ok for regular clients to update.
        @param write used when .system.js
    */
    bool legalClientSystemNS( const string& ns , bool write );

    /* deleted lists -- linked lists of deleted records -- are placed in 'buckets' of various sizes
       so you can look for a deleterecord about the right size.
    */
    const int Buckets = 19;
    const int MaxBucket = 18;

    extern int bucketSizes[];

#pragma pack(1)
    /* NamespaceDetails : this is the "header" for a collection that has all its details.
       It's in the .ns file and this is a memory mapped region (thus the pack pragma above).
    */
    class NamespaceDetails {
    public:
        enum { NIndexesMax = 64, NIndexesExtra = 30, NIndexesBase  = 10 };

        /*-------- data fields, as present on disk : */
        DiskLoc firstExtent;
        DiskLoc lastExtent;
        /* NOTE: capped collections v1 override the meaning of deletedList.
                 deletedList[0] points to a list of free records (DeletedRecord's) for all extents in
                 the capped namespace.
                 deletedList[1] points to the last record in the prev extent.  When the "current extent"
                 changes, this value is updated.  !deletedList[1].isValid() when this value is not
                 yet computed.
        */
        DiskLoc deletedList[Buckets];
        // ofs 168 (8 byte aligned)
        struct Stats {
            // datasize and nrecords MUST Be adjacent code assumes!
            little<long long> datasize; // this includes padding, but not record headers
            little<long long> nrecords;
        } stats;
        little<int> lastExtentSize;
        little<int> nIndexes;
    private:
        // ofs 192
        IndexDetails _indexes[NIndexesBase];

        // ofs 352 (16 byte aligned)
        little<int> _isCapped;                         // there is wasted space here if I'm right (ERH)
        little<int> _maxDocsInCapped;                  // max # of objects for a capped table.  TODO: should this be 64 bit?

        little<double> _paddingFactor;                 // 1.0 = no padding.
        // ofs 386 (16)
        little<int> _systemFlags; // things that the system sets/cares about
    public:
        DiskLoc capExtent;
        DiskLoc capFirstNewRecord;
        little<unsigned short> dataFileVersion;       // NamespaceDetails version.  So we can do backward compatibility in the future. See filever.h
        little<unsigned short> indexFileVersion;
        little<unsigned long long> multiKeyIndexBits;
    private:
        // ofs 400 (16)
        little<unsigned long long> reservedA;
        little<long long> extraOffset;                // where the $extra info is located (bytes relative to this)
    public:
        little<int> indexBuildInProgress;             // 1 if in prog
    private:
        little<int> _userFlags;
        char reserved[72];
        /*-------- end data 496 bytes */
    public:
        explicit NamespaceDetails( const DiskLoc &loc, bool _capped );

        class Extra {
            little<long long> _next;
        public:
            IndexDetails details[NIndexesExtra];
        private:
            little<unsigned> reserved2;
            little<unsigned> reserved3;
            Extra(const Extra&) { verify(false); }
            Extra& operator=(const Extra& r) { verify(false); return *this; }
        public:
            Extra() { }
            long ofsFrom(NamespaceDetails *d) {
                return ((char *) this) - ((char *) d);
            }
            void init() { memset(this, 0, sizeof(Extra)); }
            Extra* next(NamespaceDetails *d) {
                if( _next == 0 ) return 0;
                return (Extra*) (((char *) d) + _next);
            }
            void setNext(long ofs) { *getDur().writing(&_next) = ofs;  }
            void copy(NamespaceDetails *d, const Extra& e) {
                memcpy(this, &e, sizeof(Extra));
                _next = 0;
            }
        };
        Extra* extra() {
            if( extraOffset == 0 ) return 0;
            return (Extra *) (((char *) this) + extraOffset);
        }
        /* add extra space for indexes when more than 10 */
        Extra* allocExtra(const char *ns, int nindexessofar);
        void copyingFrom(const char *thisns, NamespaceDetails *src); // must be called when renaming a NS to fix up extra

        /* called when loaded from disk */
        void onLoad(const Namespace& k);

        /* dump info on this namespace.  for debugging. */
        void dump(const Namespace& k);

        /* dump info on all extents for this namespace.  for debugging. */
        void dumpExtents();

    private:
        Extent *theCapExtent() const { return capExtent.ext(); }
        void advanceCapExtent( const char *ns );
        DiskLoc __capAlloc(int len);
        DiskLoc cappedAlloc(const char *ns, int len);
        DiskLoc &cappedFirstDeletedInCurExtent();
        bool nextIsInCapExtent( const DiskLoc &dl ) const;

    public:

        bool isCapped() const { return _isCapped; }
        long long maxCappedDocs() const { verify( isCapped() ); return _maxDocsInCapped; }
        void setMaxCappedDocs( long long max );


        DiskLoc& cappedListOfAllDeletedRecords() { return deletedList[0]; }
        DiskLoc& cappedLastDelRecLastExtent()    { return deletedList[1]; }
        void cappedDumpDelInfo();
        bool capLooped() const { return _isCapped && capFirstNewRecord.isValid();  }
        bool inCapExtent( const DiskLoc &dl ) const;
        void cappedCheckMigrate();
        /**
         * Truncate documents newer than the document at 'end' from the capped
         * collection.  The collection cannot be completely emptied using this
         * function.  An assertion will be thrown if that is attempted.
         * @param inclusive - Truncate 'end' as well iff true
         */
        void cappedTruncateAfter(const char *ns, DiskLoc end, bool inclusive);
        /** Remove all documents from the capped collection */
        void emptyCappedCollection(const char *ns);

        /* when a background index build is in progress, we don't count the index in nIndexes until
           complete, yet need to still use it in _indexRecord() - thus we use this function for that.
        */
        int nIndexesBeingBuilt() const { return nIndexes + indexBuildInProgress; }

        /* NOTE: be careful with flags.  are we manipulating them in read locks?  if so,
                 this isn't thread safe.  TODO
        */
        enum SystemFlags {
            Flag_HaveIdIndex = 1 << 0 // set when we have _id index (ONLY if ensureIdIndex was called -- 0 if that has never been called)
        };

        enum UserFlags {
            Flag_UsePowerOf2Sizes = 1 << 0
        };

        IndexDetails& idx(int idxNo, bool missingExpected = false );

        /** get the IndexDetails for the index currently being built in the background. (there is at most one) */
        IndexDetails& inProgIdx() {
            DEV verify(indexBuildInProgress);
            return idx(nIndexes);
        }

        class IndexIterator {
        public:
            int pos() { return i; } // note this is the next one to come
            bool more() { return i < n; }
            IndexDetails& next() { return d->idx(i++); }
        private:
            friend class NamespaceDetails;
            int i, n;
            NamespaceDetails *d;
            IndexIterator(NamespaceDetails *_d);
        };

        IndexIterator ii() { return IndexIterator(this); }

        /* hackish - find our index # in the indexes array */
        int idxNo(IndexDetails& idx);

        /* multikey indexes are indexes where there are more than one key in the index
             for a single document. see multikey in wiki.
           for these, we have to do some dedup work on queries.
        */
        bool isMultikey(int i) const { return (multiKeyIndexBits & (((unsigned long long) 1) << i)) != 0; }
        void setIndexIsMultikey(const char *thisns, int i);

        /* add a new index.  does not add to system.indexes etc. - just to NamespaceDetails.
           caller must populate returned object.
         */
        IndexDetails& addIndex(const char *thisns, bool resetTransient=true);

        void aboutToDeleteAnIndex() { 
            clearSystemFlag( Flag_HaveIdIndex );
        }

        /* returns index of the first index in which the field is present. -1 if not present. */
        int fieldIsIndexed(const char *fieldName);

        /**
         * @return the actual size to create
         *         will be >= oldRecordSize
         *         based on padding and any other flags
         */
        int getRecordAllocationSize( int minRecordSize );

        double paddingFactor() const { return _paddingFactor; }

        void setPaddingFactor( double paddingFactor ) {
            *getDur().writing(&_paddingFactor) = paddingFactor;
        }

        /* called to indicate that an update fit in place.  
           fits also called on an insert -- idea there is that if you had some mix and then went to
           pure inserts it would adapt and PF would trend to 1.0.  note update calls insert on a move
           so there is a double count there that must be adjusted for below.

           todo: greater sophistication could be helpful and added later.  for example the absolute 
                 size of documents might be considered -- in some cases smaller ones are more likely 
                 to grow than larger ones in the same collection? (not always)
        */
        void paddingFits() {
            MONGO_SOMETIMES(sometimes, 4) { // do this on a sampled basis to journal less
                double x = _paddingFactor - 0.001;
                if ( x >= 1.0 ) {
                    setPaddingFactor( x );
                }
            }
        }
        void paddingTooSmall() {            
            MONGO_SOMETIMES(sometimes, 4) { // do this on a sampled basis to journal less       
                /* the more indexes we have, the higher the cost of a move.  so we take that into 
                   account herein.  note on a move that insert() calls paddingFits(), thus
                   here for example with no inserts and nIndexes = 1 we have
                   .001*4-.001 or a 3:1 ratio to non moves -> 75% nonmoves.  insert heavy 
                   can pushes this down considerably. further tweaking will be a good idea but 
                   this should be an adequate starting point.
                */
                double N = min( nIndexes + 0, 7 ) + 3;
                double x = _paddingFactor + (0.001 * N);
                if ( x <= 2.0 ) {
                    setPaddingFactor( x );
                }
            }
        }

        // @return offset in indexes[]
        int findIndexByName(const char *name);

        // @return offset in indexes[]
        int findIndexByKeyPattern(const BSONObj& keyPattern);

        void findIndexByType( const string& name , vector<int>& matches ) {
            IndexIterator i = ii();
            while ( i.more() ) {
                if ( i.next().getSpec().getTypeName() == name )
                    matches.push_back( i.pos() - 1 );
            }
        }

        const int systemFlags() const { return _systemFlags; }
        bool isSystemFlagSet( int flag ) const { return _systemFlags & flag; }
        void setSystemFlag( int flag );
        void clearSystemFlag( int flag );

        const int userFlags() const { return _userFlags; }
        bool isUserFlagSet( int flag ) const { return _userFlags & flag; }
        void setUserFlag( int flag );
        void clearUserFlag( int flag );

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

        bool haveIdIndex() { 
            return isSystemFlagSet( NamespaceDetails::Flag_HaveIdIndex ) || findIdIndex() >= 0;
        }

        /* return which "deleted bucket" for this size object */
        static int bucket(int n) {
            for ( int i = 0; i < Buckets; i++ )
                if ( bucketSizes[i] > n )
                    return i;
            return Buckets-1;
        }

        /* predetermine location of the next alloc without actually doing it. 
           if cannot predetermine returns null (so still call alloc() then)
        */
        DiskLoc allocWillBeAt(const char *ns, int lenToAlloc);

        /* allocate a new record.  lenToAlloc includes headers. */
        DiskLoc alloc(const char *ns, int lenToAlloc, DiskLoc& extentLoc);

        /* add a given record to the deleted chains for this NS */
        void addDeletedRec(DeletedRecord *d, DiskLoc dloc);
        void dumpDeleted(set<DiskLoc> *extents = 0);
        // Start from firstExtent by default.
        DiskLoc firstRecord( const DiskLoc &startExtent = DiskLoc() ) const;
        // Start from lastExtent by default.
        DiskLoc lastRecord( const DiskLoc &startExtent = DiskLoc() ) const;
        long long storageSize( int * numExtents = 0 , BSONArrayBuilder * extentInfo = 0 ) const;

        int averageObjectSize() {
            if ( stats.nrecords == 0 )
                return 5;
            return (int) (stats.datasize / stats.nrecords);
        }

        NamespaceDetails *writingWithoutExtra() {
            return ( NamespaceDetails* ) getDur().writingPtr( this, sizeof( NamespaceDetails ) );
        }
        /** Make all linked Extra objects writeable as well */
        NamespaceDetails *writingWithExtra();

    private:
        DiskLoc _alloc(const char *ns, int len);
        void maybeComplain( const char *ns, int len ) const;
        DiskLoc __stdAlloc(int len, bool willBeAt);
        void compact(); // combine adjacent deleted records
        friend class NamespaceIndex;
        struct ExtraOld {
            // note we could use this field for more chaining later, so don't waste it:
            unsigned long long reserved1;
            IndexDetails details[NIndexesExtra];
            unsigned reserved2;
            unsigned reserved3;
        };
        /** Update cappedLastDelRecLastExtent() after capExtent changed in cappedTruncateAfter() */
        void cappedTruncateLastDelUpdate();
        BOOST_STATIC_ASSERT( NIndexesMax <= NIndexesBase + NIndexesExtra*2 );
        BOOST_STATIC_ASSERT( NIndexesMax <= 64 ); // multiKey bits
        BOOST_STATIC_ASSERT( sizeof(NamespaceDetails::ExtraOld) == 496 );
        BOOST_STATIC_ASSERT( sizeof(NamespaceDetails::Extra) == 496 );
    }; // NamespaceDetails
#pragma pack()

    class ParsedQuery;
    class QueryPlanSummary;
    
    /* NamespaceDetailsTransient

       these are things we know / compute about a namespace that are transient -- things
       we don't actually store in the .ns file.  so mainly caching of frequently used
       information.

       CAUTION: Are you maintaining this properly on a collection drop()?  A dropdatabase()?  Be careful.
                The current field "allIndexKeys" may have too many keys in it on such an occurrence;
                as currently used that does not cause anything terrible to happen.

       todo: cleanup code, need abstractions and separation
    */
    // todo: multiple db's with the same name (repairDatbase) is not handled herein.  that may be 
    //       the way to go, if not used by repair, but need some sort of enforcement / asserts.
    class NamespaceDetailsTransient : boost::noncopyable {
        BOOST_STATIC_ASSERT( sizeof(NamespaceDetails) == 496 );

        //Database *database;
        const string _ns;
        void reset();
        static std::map< string, shared_ptr< NamespaceDetailsTransient > > _nsdMap;

        NamespaceDetailsTransient(Database*,const char *ns);
    public:
        ~NamespaceDetailsTransient();
        void addedIndex() { reset(); }
        void deletedIndex() { reset(); }
        /* Drop cached information on all namespaces beginning with the specified prefix.
           Can be useful as index namespaces share the same start as the regular collection.
           SLOW - sequential scan of all NamespaceDetailsTransient objects */
        static void clearForPrefix(const char *prefix);
        static void eraseForPrefix(const char *prefix);

        /**
         * @return a cursor interface to the query optimizer.  The implementation may utilize a
         * single query plan or interleave results from multiple query plans before settling on a
         * single query plan.  Note that the schema of currKey() documents, indexKeyPattern(), the
         * matcher(), and the isMultiKey() nature of the cursor may change over the course of
         * iteration.
         *
         * @param query - Query used to select indexes and populate matchers; not copied if unowned
         * (see bsonobj.h).
         *
         * @param order - Required ordering spec for documents produced by this cursor, empty object
         * default indicates no order requirement.  If no index exists that satisfies the required
         * sort order, an empty shared_ptr is returned unless parsedQuery is also provided.  This is
         * not copied if unowned.
         *
         * @param planPolicy - A policy for selecting query plans - see queryoptimizercursor.h
         *
         * @param simpleEqualityMatch - Set to true for certain simple queries - see
         * queryoptimizer.cpp.
         *
         * @param parsedQuery - Additional query parameters, as from a client query request.  If
         * specified, the resulting cursor may return results from out of order plans.  See
         * queryoptimizercursor.h for information on handling these results.
         *
         * @param singlePlanSummary - Query plan summary information that may be provided when a
         * cursor running a single plan is returned.
         *
         * The returned cursor may @throw inside of advance() or recoverFromYield() in certain error
         * cases, for example if a capped overrun occurred during a yield.  This indicates that the
         * cursor was unable to perform a complete scan.
         *
         * This is a work in progress.  Partial list of features not yet implemented through this
         * interface:
         * 
         * - covered indexes
         * - in memory sorting
         */
        static shared_ptr<Cursor> getCursor( const char *ns, const BSONObj &query,
                                            const BSONObj &order = BSONObj(),
                                            const QueryPlanSelectionPolicy &planPolicy =
                                            QueryPlanSelectionPolicy::any(),
                                            bool *simpleEqualityMatch = 0,
                                            const shared_ptr<const ParsedQuery> &parsedQuery =
                                            shared_ptr<const ParsedQuery>(),
                                            QueryPlanSummary *singlePlanSummary = 0 );

        /**
         * @return a single cursor that may work well for the given query.  A $or style query will
         * produce a single cursor, not a MultiCursor.
         * It is possible no cursor is returned if the sort is not supported by an index.  Clients are responsible
         * for checking this if they are not sure an index for a sort exists, and defaulting to a non-sort if
         * no suitable indices exist.
         */
        static shared_ptr<Cursor> bestGuessCursor( const char *ns, const BSONObj &query, const BSONObj &sort );

        /* indexKeys() cache ---------------------------------------------------- */
        /* assumed to be in write lock for this */
    private:
        bool _keysComputed;
        set<string> _indexKeys;
        void computeIndexKeys();
    public:
        /* get set of index keys for this namespace.  handy to quickly check if a given
           field is indexed (Note it might be a secondary component of a compound index.)
        */
        set<string>& indexKeys() {
            DEV Lock::assertWriteLocked(_ns);
            if ( !_keysComputed )
                computeIndexKeys();
            return _indexKeys;
        }

        /* IndexSpec caching */
    private:
        map<const IndexDetails*,IndexSpec> _indexSpecs;
        static SimpleMutex _isMutex;
    public:
        const IndexSpec& getIndexSpec( const IndexDetails * details ) {
            IndexSpec& spec = _indexSpecs[details];
            if ( ! spec._finishedInit ) {
                SimpleMutex::scoped_lock lk(_isMutex);
                if ( ! spec._finishedInit ) {
                    spec.reset( details );
                    verify( spec._finishedInit );
                }
            }
            return spec;
        }

        /* query cache (for query optimizer) ------------------------------------- */
    private:
        int _qcWriteCount;
        map<QueryPattern,CachedQueryPlan> _qcCache;
        static NamespaceDetailsTransient& make_inlock(const char *ns);
    public:
        static SimpleMutex _qcMutex;

        /* you must be in the qcMutex when calling this.
           A NamespaceDetailsTransient object will not go out of scope on you if you are
           d.dbMutex.atLeastReadLocked(), so you do't have to stay locked.
           Creates a NamespaceDetailsTransient before returning if one DNE. 
           todo: avoid creating too many on erroneous ns queries.
           */
        static NamespaceDetailsTransient& get_inlock(const char *ns);

        static NamespaceDetailsTransient& get(const char *ns) {
            // todo : _qcMutex will create bottlenecks in our parallelism
            SimpleMutex::scoped_lock lk(_qcMutex);
            return get_inlock(ns);
        }

        void clearQueryCache() {
            _qcCache.clear();
            _qcWriteCount = 0;
        }
        /* you must notify the cache if you are doing writes, as query plan utility will change */
        void notifyOfWriteOp() {
            if ( _qcCache.empty() )
                return;
            if ( ++_qcWriteCount >= 100 )
                clearQueryCache();
        }
        CachedQueryPlan cachedQueryPlanForPattern( const QueryPattern &pattern ) {
            return _qcCache[ pattern ];
        }
        void registerCachedQueryPlanForPattern( const QueryPattern &pattern,
                                               const CachedQueryPlan &cachedQueryPlan ) {
            _qcCache[ pattern ] = cachedQueryPlan;
        }

    }; /* NamespaceDetailsTransient */

    inline NamespaceDetailsTransient& NamespaceDetailsTransient::get_inlock(const char *ns) {
        std::map< string, shared_ptr< NamespaceDetailsTransient > >::iterator i = _nsdMap.find(ns);
        if( i != _nsdMap.end() && 
            i->second.get() ) { // could be null ptr from clearForPrefix
            return *i->second;
        }
        return make_inlock(ns);
    }

    /* NamespaceIndex is the ".ns" file you see in the data directory.  It is the "system catalog"
       if you will: at least the core parts.  (Additional info in system.* collections.)
    */
    class NamespaceIndex {
    public:
        NamespaceIndex(const string &dir, const string &database) :
            ht( 0 ), dir_( dir ), database_( database ) {}

        /* returns true if new db will be created if we init lazily */
        bool exists() const;

        void init() {
            if( !ht ) 
                _init();
        }

        void add_ns(const char *ns, DiskLoc& loc, bool capped);
        void add_ns( const char *ns, const NamespaceDetails &details );

        NamespaceDetails* details(const char *ns) {
            if ( !ht )
                return 0;
            Namespace n(ns);
            NamespaceDetails *d = ht->get(n);
            if ( d && d->isCapped() )
                d->cappedCheckMigrate();
            return d;
        }

        void kill_ns(const char *ns);

        bool find(const char *ns, DiskLoc& loc) {
            NamespaceDetails *l = details(ns);
            if ( l ) {
                loc = l->firstExtent;
                return true;
            }
            return false;
        }

        bool allocated() const { return ht != 0; }

        void getNamespaces( list<string>& tofill , bool onlyCollections = true ) const;

        NamespaceDetails::Extra* newExtra(const char *ns, int n, NamespaceDetails *d);

        boost::filesystem::path path() const;

        unsigned long long fileLength() const { return f.length(); }

    private:
        void _init();
        void maybeMkdir() const;

        MongoMMF f;
        HashTable<Namespace,NamespaceDetails> *ht;
        string dir_;
        string database_;
    };

    extern string dbpath; // --dbpath parm
    extern bool directoryperdb;

    // Rename a namespace within current 'client' db.
    // (Arguments should include db name)
    void renameNamespace( const char *from, const char *to, bool stayTemp);


} // namespace mongo
