// btree.h

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
#include "jsobj.h"
#include "diskloc.h"
#include "pdfile.h"

namespace mongo {

    const int BucketSize = 8192;

#pragma pack(1)
    struct _KeyNode {
        // Signals that we are writing this _KeyNode and casts away const
        _KeyNode& writing() const;
        DiskLoc prevChildBucket; // the lchild
        DiskLoc recordLoc; // location of the record associated with the key
        short keyDataOfs() const {
            return (short) _kdo;
        }
        unsigned short _kdo;
        void setKeyDataOfs(short s) {
            _kdo = s;
            assert(s>=0);
        }
        void setKeyDataOfsSavingUse(short s) {
            _kdo = s;
            assert(s>=0);
        }
        void setUsed() { recordLoc.GETOFS() &= ~1; }
        void setUnused() {
            /* Setting ofs to odd is the sentinel for unused, as real recordLoc's are always
               even numbers.
               Note we need to keep its value basically the same as we use the recordLoc
               as part of the key in the index (to handle duplicate keys efficiently).
            */
            recordLoc.GETOFS() |= 1;
        }
        int isUnused() const {
            return recordLoc.getOfs() & 1;
        }
        int isUsed() const {
            return !isUnused();
        }
    };
#pragma pack()

    class BucketBasics;

    /**
     * wrapper - this is our in memory representation of the key.
     * _KeyNode is the disk representation.
     * 
     * This object and its bson key will become invalid if the key is moved.
     */
    class KeyNode {
    public:
        KeyNode(const BucketBasics& bb, const _KeyNode &k);
        const DiskLoc& prevChildBucket;
        const DiskLoc& recordLoc;
        BSONObj key;
    };

#pragma pack(1)
    class BtreeData { 
    protected:
        DiskLoc parent;
        DiskLoc nextChild; // child bucket off and to the right of the highest key.
        unsigned short _wasSize; // can be reused, value is 8192 in current pdfile version Apr2010
        unsigned short _reserved1; // zero
        int flags;

        // basicInsert() assumes these three are together and in this order:
        int emptySize; // size of the empty region
        int topSize; // size of the data at the top of the bucket (keys are at the beginning or 'bottom')
        int n; // # of keys so far.

        int reserved;
        char data[4];
    };

    /**
     * This class is all about the storage management
     *
     * Const member functions of this class are those which may be called on
     * an object for which writing has not been signaled.  Non const member
     * functions may only be called on objects for which writing has been
     * signaled.
     *
     * DiskLoc parameters that may shadow references within the btree should
     * be passed by value rather than by reference to non const member
     * functions.  This way a callee need not worry that write operations will
     * change or invalidate its arguments.
     *
     * The current policy for dealing with bson arguments is the opposite of
     * what is described above for DiskLoc arguments.  We do
     * not want to want to copy bson into memory as an intermediate step for
     * btree changes, so if bson is to be moved it must be copied to the new
     * location before the old location is invalidated.
     */
    class BucketBasics : public BtreeData {
        friend class BtreeBuilder;
        friend class KeyNode;
    public:
        void assertValid(const Ordering &order, bool force = false) const;
        void assertValid(const BSONObj &orderObj, bool force = false) const { return assertValid(Ordering::make(orderObj),force); }

        /**
         * @return KeyNode for key at index i.  The KeyNode will become invalid
         * if the key is moved or reassigned, or if the node is packed.
         */
        const KeyNode keyNode(int i) const {
            if ( i >= n ){
                massert( 13000 , (string)"invalid keyNode: " +  BSON( "i" << i << "n" << n ).jsonString() , i < n );
            }
            return KeyNode(*this, k(i));
        }
        
        static int headerSize() {
            const BucketBasics *d = 0;
            return (char*)&(d->data) - (char*)&(d->parent);
        }
        static int bodySize() { return BucketSize - headerSize(); }
        
        // for testing
        int nKeys() const { return n; }
        const DiskLoc getNextChild() const { return nextChild; }
        
    protected:
        char * dataAt(short ofs) { return data + ofs; }

        void init(); // initialize a new node

        /**
         * @return false if node is full and must be split
         * @keypos is where to insert -- inserted before that key #.  so keypos=0 is the leftmost one.
         *  keypos will be updated if keys are moved as a result of pack()
        */
        bool basicInsert(const DiskLoc thisLoc, int &keypos, const DiskLoc recordLoc, const BSONObj& key, const Ordering &order) const;
        
        /**
         * @return true if works, false if not enough space
         */
        bool _pushBack(const DiskLoc recordLoc, const BSONObj& key, const Ordering &order, const DiskLoc prevChild);
        void pushBack(const DiskLoc recordLoc, const BSONObj& key, const Ordering &order, const DiskLoc prevChild){
            bool ok = _pushBack( recordLoc , key , order , prevChild );
            assert(ok);
        }

        /**
         * This is a special purpose function used by BtreeBuilder.  The
         * interface is quite dangerous if you're not careful.  The bson key
         * returned here points to bucket memory that has been invalidated but
         * not yet reclaimed.
         *
         * TODO Maybe this could be replaced with two functions, one which
         * returns the last key without deleting it and another which simply
         * deletes the last key.  Then the caller would have enough control to
         * ensure proper memory integrity.
         */
        void popBack(DiskLoc& recLoc, BSONObj& key);

        void _delKeyAtPos(int keypos, bool mayEmpty = false); // low level version that doesn't deal with child ptrs.

        /* !Packed means there is deleted fragment space within the bucket.
           We "repack" when we run out of space before considering the node
           to be full.
           */
        enum Flags { Packed=1 };

        const DiskLoc& childForPos(int p) const { return p == n ? nextChild : k(p).prevChildBucket; }
        DiskLoc& childForPos(int p) { return p == n ? nextChild : k(p).prevChildBucket; }

        int totalDataSize() const;
        // @return true if the key may be dropped by pack()
        bool mayDropKey( int index, int refPos ) const;

        /**
         * Pack the bucket to reclaim space from invalidated memory.
         * @refPos is an index in the bucket which will may be updated if we
         *  delete keys from the bucket
         */
        void _pack(const DiskLoc thisLoc, const Ordering &order, int &refPos) const;
        void _packReadyForMod(const Ordering &order, int &refPos);

        /**
         * @return the size of non header data in this bucket if we were to
         * call pack().
         */
        int packedDataSize( int refPos ) const;
        void setNotPacked() { flags &= ~Packed; }
        void setPacked() { flags |= Packed; }
        int _alloc(int bytes);
        void _unalloc(int bytes);
        void truncateTo(int N, const Ordering &order, int &refPos);
        // drop specified number of keys from beginning of key array, and pack
        void dropFront(int nDrop, const Ordering &order, int &refPos);
        void markUnused(int keypos);

        /* BtreeBuilder uses the parent var as a temp place to maintain a linked list chain. 
           we use tempNext() when we do that to be less confusing. (one might have written a union in C)
           */
        const DiskLoc& tempNext() const { return parent; }
        DiskLoc& tempNext() { return parent; }

        void _shape(int level, stringstream&) const;
        int Size() const;
        const _KeyNode& k(int i) const { return ((const _KeyNode*)data)[i]; }
        _KeyNode& k(int i) { return ((_KeyNode*)data)[i]; }
        
        // @return the key position where a split should occur on insert
        int splitPos( int keypos ) const;
        
        /**
         * Adds new entries to beginning of key array, shifting existing
         * entries to the right.  After this is called, setKey() must be called
         * on all the newly created entries in the key array.
         */
        void reserveKeysFront( int nAdd );
        
        /**
         * Sets an existing key using the given parameters.
         * @i index of key to set
         */
        void setKey( int i, const DiskLoc recordLoc, const BSONObj &key, const DiskLoc prevChildBucket );
    };

    /**
     * This class adds functionality for manipulating buckets that are assembled
     * in a tree.  The requirements for const and non const functions and
     * arguments are generally the same as in BtreeBucket.  Because this class
     * deals with tree structure, some functions that are marked const may
     * trigger modification of another node in the btree or potentially of the
     * current node.  In such cases, the function's implementation explicitly
     * casts away const when indicating an intent to write to the durability
     * layer.  The DiskLocs provided to such functions should be passed by
     * value if they shadow pointers within the btree.
     *
     * To clarify enforcement of referential integrity in this implementation,
     * we use the following pattern when deleting data we have a persistent
     * pointer to.  The pointer is cleared or removed explicitly, then the data
     * it pointed to is cleaned up with a helper function.
     *
     * TODO It might make sense to put some of these functions in a class
     * representing a full btree instead of a single btree bucket.  That would
     * allow us to use the const qualifier in a manner more consistent with
     * standard usage.  Right now the interface is for both a node and a tree,
     * so assignment of const is sometimes nonideal.
     *
     * TODO There are several cases in which the this pointer is invalidated
     * as a result of deallocation.  A seperate class representing a btree would
     * alleviate some fragile cases where the implementation must currently
     * behave correctly if the this pointer is suddenly invalidated by a
     * callee.
     */
    class BtreeBucket : public BucketBasics {
        friend class BtreeCursor;
    public:
        bool isHead() const { return parent.isNull(); }
        void dumpTree(const DiskLoc &thisLoc, const BSONObj &order) const;
        int fullValidate(const DiskLoc& thisLoc, const BSONObj &order, int *unusedCount = 0, bool strict = false) const; /* traverses everything */

        bool isUsed( int i ) const { return k(i).isUsed(); }
        string bucketSummary() const;
        void dump() const;

        /* @return true if key exists in index 

           order - indicates order of keys in the index.  this is basically the index's key pattern, e.g.:
             BSONObj order = ((IndexDetails&)idx).keyPattern();
           likewise below in bt_insert() etc.
        */
        bool exists(const IndexDetails& idx, const DiskLoc &thisLoc, const BSONObj& key, const Ordering& order) const;

        bool wouldCreateDup(
            const IndexDetails& idx, const DiskLoc &thisLoc, 
            const BSONObj& key, const Ordering& order,
            const DiskLoc &self) const; 

        static DiskLoc addBucket(const IndexDetails&); /* start a new index off, empty */
        // invalidates 'this' and thisLoc
        void deallocBucket(const DiskLoc thisLoc, const IndexDetails &id);
        
        static void renameIndexNamespace(const char *oldNs, const char *newNs);

        // This function may change the btree root
        int bt_insert(const DiskLoc thisLoc, const DiskLoc recordLoc,
                   const BSONObj& key, const Ordering &order, bool dupsAllowed,
                   IndexDetails& idx, bool toplevel = true) const;

        // This function may change the btree root
        bool unindex(const DiskLoc thisLoc, IndexDetails& id, const BSONObj& key, const DiskLoc recordLoc) const;

        /* locate may return an "unused" key that is just a marker.  so be careful.
             looks for a key:recordloc pair.

           found - returns true if exact match found.  note you can get back a position 
                   result even if found is false.
        */
        DiskLoc locate(const IndexDetails &idx , const DiskLoc& thisLoc, const BSONObj& key, const Ordering &order, 
                       int& pos, bool& found, const DiskLoc &recordLoc, int direction=1) const;
        
        /**
         * find the first instance of the key
         * does not handle dups
         * returned DiskLoc isNull if can't find anything with that
         * @return the record location of the first match
         */
        DiskLoc findSingle( const IndexDetails &indexdetails , const DiskLoc& thisLoc, const BSONObj& key ) const;

        /* advance one key position in the index: */
        DiskLoc advance(const DiskLoc& thisLoc, int& keyOfs, int direction, const char *caller) const;
        
        void advanceTo(DiskLoc &thisLoc, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction ) const;
        void customLocate(DiskLoc &thisLoc, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction, pair< DiskLoc, int > &bestParent ) const;
        
        const DiskLoc getHead(const DiskLoc& thisLoc) const;

        /* get tree shape */
        void shape(stringstream&) const;

        static void a_test(IndexDetails&);

        static int getLowWaterMark();
        static int getKeyMax();
        
    protected:
        /**
         * Fix parent pointers for children
         * @firstIndex first index to modify
         * @lastIndex last index to modify (-1 means last index is n)
         */
        void fixParentPtrs(const DiskLoc thisLoc, int firstIndex = 0, int lastIndex = -1) const;

        // invalidates this and thisLoc
        void delBucket(const DiskLoc thisLoc, const IndexDetails&);
        // may invalidate this and thisLoc
        void delKeyAtPos(const DiskLoc thisLoc, IndexDetails& id, int p, const Ordering &order);

        // may invalidate this and thisLoc
        void balanceWithNeighbors(const DiskLoc thisLoc, IndexDetails &id, const Ordering &order) const;        

        // @return true if balance succeeded
        bool tryBalanceChildren( const DiskLoc thisLoc, int leftIndex, IndexDetails &id, const Ordering &order ) const;
        void doBalanceChildren( const DiskLoc thisLoc, int leftIndex, IndexDetails &id, const Ordering &order );
        void doBalanceLeftToRight( const DiskLoc thisLoc, int leftIndex, int split,
                                  BtreeBucket *l, const DiskLoc lchild,
                                  BtreeBucket *r, const DiskLoc rchild,
                                  IndexDetails &id, const Ordering &order );
        void doBalanceRightToLeft( const DiskLoc thisLoc, int leftIndex, int split,
                                  BtreeBucket *l, const DiskLoc lchild,
                                  BtreeBucket *r, const DiskLoc rchild,
                                  IndexDetails &id, const Ordering &order );

        // may invalidate this and thisLoc
        void doMergeChildren( const DiskLoc thisLoc, int leftIndex, IndexDetails &id, const Ordering &order);

        // will invalidate this and thisLoc
        void replaceWithNextChild( const DiskLoc thisLoc, IndexDetails &id );

        // @return true iff left and right child can be merged into one node
        bool mayMergeChildren( const DiskLoc &thisLoc, int leftIndex ) const;
        
        /**
         * @return index of the rebalanced separator; the index value is
         * determined as if we had an array
         * <left bucket keys array>.push( <old separator> ).concat( <right bucket keys array> )
         * This is only expected to be called if the left and right child
         * cannot be merged.
         * This function is expected to be called on packed buckets, see also
         * comments for splitPos().
         */
        int rebalancedSeparatorPos( const DiskLoc &thisLoc, int leftIndex ) const;
        
        int indexInParent( const DiskLoc &thisLoc ) const;
        BSONObj keyAt(int keyOfs) const {
            return keyOfs >= n ? BSONObj() : keyNode(keyOfs).key;
        }
        static BtreeBucket* allocTemp(); /* caller must release with free() */

        /** split bucket */
        void split(const DiskLoc thisLoc, int keypos, 
                   const DiskLoc recordLoc, const BSONObj& key,
                   const Ordering& order, const DiskLoc lchild, const DiskLoc rchild, IndexDetails& idx);

        void _insertHere(const DiskLoc thisLoc, int keypos,
                        const DiskLoc recordLoc, const BSONObj& key, const Ordering &order,
                        const DiskLoc lchild, const DiskLoc rchild, IndexDetails &idx) const;
        void insertHere(const DiskLoc thisLoc, int keypos,
                        const DiskLoc recordLoc, const BSONObj& key, const Ordering &order,
                        const DiskLoc lchild, const DiskLoc rchild, IndexDetails &idx) const;

        int _insert(const DiskLoc thisLoc, const DiskLoc recordLoc,
                    const BSONObj& key, const Ordering &order, bool dupsAllowed,
                    const DiskLoc lChild, const DiskLoc rChild, IndexDetails &idx) const;
        bool find(const IndexDetails& idx, const BSONObj& key, const DiskLoc &recordLoc, const Ordering &order, int& pos, bool assertIfDup) const;
        bool customFind( int l, int h, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction, DiskLoc &thisLoc, int &keyOfs, pair< DiskLoc, int > &bestParent ) const;
        static void findLargestKey(const DiskLoc& thisLoc, DiskLoc& largestLoc, int& largestKey);
        static int customBSONCmp( const BSONObj &l, const BSONObj &rBegin, int rBeginLen, bool rSup, const vector< const BSONElement * > &rEnd, const vector< bool > &rEndInclusive, const Ordering &o, int direction );
        static void fix(const DiskLoc thisLoc, const DiskLoc child);
        
        // Replaces an existing key with the new specified key, splitting if necessary
        void setInternalKey( const DiskLoc thisLoc, int keypos,
                            const DiskLoc recordLoc, const BSONObj &key, const Ordering &order,
                            const DiskLoc lchild, const DiskLoc rchild, IndexDetails &idx);
    public:
        // simply builds and returns a dup key error message string
        static string dupKeyError( const IndexDetails& idx , const BSONObj& key );
    };
#pragma pack()

    class BtreeCursor : public Cursor {
    public:
        BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails&, const BSONObj &startKey, const BSONObj &endKey, bool endKeyInclusive, int direction );
        BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails& _id, const shared_ptr< FieldRangeVector > &_bounds, int _direction );
        virtual bool ok() { return !bucket.isNull(); }
        virtual bool advance();
        virtual void noteLocation(); // updates keyAtKeyOfs...
        virtual void checkLocation();
        virtual bool supportGetMore() { return true; }
        virtual bool supportYields() { return true; }

        /* used for multikey index traversal to avoid sending back dups. see Matcher::matches().
           if a multikey index traversal:
             if loc has already been sent, returns true.
             otherwise, marks loc as sent.
             @return true if the loc has not been seen
        */
        virtual bool getsetdup(DiskLoc loc) {
            if( _multikey ) { 
                pair<set<DiskLoc>::iterator, bool> p = _dups.insert(loc);
                return !p.second;
            }
            return false;
        }
        
        virtual bool modifiedKeys() const { return _multikey; }
        virtual bool isMultiKey() const { return _multikey; }

        const _KeyNode& _currKeyNode() const {
            assert( !bucket.isNull() );
            const _KeyNode& kn = bucket.btree()->k(keyOfs);
            assert( kn.isUsed() );
            return kn;
        }
        const KeyNode currKeyNode() const {
            assert( !bucket.isNull() );
            return bucket.btree()->keyNode(keyOfs);
        }

        virtual BSONObj currKey() const { return currKeyNode().key; }
        virtual BSONObj indexKeyPattern() { return indexDetails.keyPattern(); }

        virtual void aboutToDeleteBucket(const DiskLoc& b) {
            if ( bucket == b )
                keyOfs = -1;
        }

        virtual DiskLoc currLoc()  { return !bucket.isNull() ? _currKeyNode().recordLoc : DiskLoc();  }
        virtual DiskLoc refLoc()   { return currLoc(); }
        virtual Record* _current() { return currLoc().rec(); }
        virtual BSONObj current()  { return BSONObj(_current()); }
        virtual string toString() {
            string s = string("BtreeCursor ") + indexDetails.indexName();
            if ( _direction < 0 ) s += " reverse";
            if ( _bounds.get() && _bounds->size() > 1 ) s += " multi";
            return s;
        }

        BSONObj prettyKey( const BSONObj &key ) const {
            return key.replaceFieldNames( indexDetails.keyPattern() ).clientReadable();
        }

        virtual BSONObj prettyIndexBounds() const {
            if ( !_independentFieldRanges ) {
                return BSON( "start" << prettyKey( startKey ) << "end" << prettyKey( endKey ) );
            } else {
                return _bounds->obj();
            }
        }
        
        void forgetEndKey() { endKey = BSONObj(); }

        virtual CoveredIndexMatcher *matcher() const { return _matcher.get(); }
        
        virtual void setMatcher( shared_ptr< CoveredIndexMatcher > matcher ) { _matcher = matcher;  }

        virtual long long nscanned() { return _nscanned; }
        
        // for debugging only
        const DiskLoc getBucket() const { return bucket; }
        
    private:
        /* Our btrees may (rarely) have "unused" keys when items are deleted.
           Skip past them.
        */
        bool skipUnusedKeys( bool mayJump );
        bool skipOutOfRangeKeysAndCheckEnd();
        void skipAndCheck();
        void checkEnd();

        // selective audits on construction
        void audit();

        // set initial bucket
        void init();

        // if afterKey is true, we want the first key with values of the keyBegin fields greater than keyBegin
        void advanceTo( const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive );
        
        friend class BtreeBucket;

        set<DiskLoc> _dups;
        NamespaceDetails * const d;
        const int idxNo;        
        BSONObj startKey;
        BSONObj endKey;
        bool _endKeyInclusive;        
        bool _multikey; // this must be updated every getmore batch in case someone added a multikey
        const IndexDetails& indexDetails;
        const BSONObj _order;
        const Ordering _ordering;
        DiskLoc bucket;
        int keyOfs;
        const int _direction; // 1=fwd,-1=reverse
        BSONObj keyAtKeyOfs; // so we can tell if things moved around on us between the query and the getMore call
        DiskLoc locAtKeyOfs;
        const shared_ptr< FieldRangeVector > _bounds;
        auto_ptr< FieldRangeVector::Iterator > _boundsIterator;
        const IndexSpec& _spec;
        shared_ptr< CoveredIndexMatcher > _matcher;
        bool _independentFieldRanges;
        long long _nscanned;
    };


    inline bool IndexDetails::hasKey(const BSONObj& key) { 
        return head.btree()->exists(*this, head, key, Ordering::make(keyPattern()));
    }
    inline bool IndexDetails::wouldCreateDup(const BSONObj& key, DiskLoc self) { 
        return head.btree()->wouldCreateDup(*this, head, key, Ordering::make(keyPattern()), self);
    }

    /* build btree from the bottom up */
    /* _ TODO dropDups */
    class BtreeBuilder {
        bool dupsAllowed; 
        IndexDetails& idx;
        unsigned long long n;
        BSONObj keyLast;
        BSONObj order;
        Ordering ordering;
        bool committed;

        DiskLoc cur, first;
        BtreeBucket *b;

        void newBucket();
        void buildNextLevel(DiskLoc);

    public:
        ~BtreeBuilder();

        BtreeBuilder(bool _dupsAllowed, IndexDetails& _idx);

        /* keys must be added in order */
        void addKey(BSONObj& key, DiskLoc loc);

        /* commit work.  if not called, destructor will clean up partially completed work 
           (in case exception has happened).
        */
        void commit();

        unsigned long long getn() { return n; }
    };

} // namespace mongo;
