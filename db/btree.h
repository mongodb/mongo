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

#pragma pack(1)
    struct _KeyNode {
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
        void setUsed() { 
            recordLoc.GETOFS() &= ~1;
        }
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

    /* wrapper - this is our in memory representation of the key.  _KeyNode is the disk representation. */
    class KeyNode {
    public:
        KeyNode(const BucketBasics& bb, const _KeyNode &k);
        const DiskLoc& prevChildBucket;
        const DiskLoc& recordLoc;
        BSONObj key;
    };

#pragma pack(1)
    /* this class is all about the storage management */
    class BucketBasics {
        friend class BtreeBuilder;
        friend class KeyNode;
    public:
        void dumpTree(DiskLoc thisLoc, const BSONObj &order);
        bool isHead() { return parent.isNull(); }
        void assertValid(const Ordering &order, bool force = false);
        void assertValid(const BSONObj &orderObj, bool force = false) { 
            return assertValid(Ordering::make(orderObj),force); 
        }
        int fullValidate(const DiskLoc& thisLoc, const BSONObj &order, int *unusedCount = 0); /* traverses everything */

        KeyNode keyNode(int i) const {
            if ( i >= n ){
                massert( 13000 , (string)"invalid keyNode: " +  BSON( "i" << i << "n" << n ).jsonString() , i < n );
            }
            return KeyNode(*this, k(i));
        }

    protected:

        void modified(const DiskLoc& thisLoc);

        char * dataAt(short ofs) {
            return data + ofs;
        }

        void init(); // initialize a new node

        /* returns false if node is full and must be split
           keypos is where to insert -- inserted after that key #.  so keypos=0 is the leftmost one.
        */
        bool basicInsert(const DiskLoc& thisLoc, int &keypos, const DiskLoc& recordLoc, const BSONObj& key, const Ordering &order);
        
        /**
         * @return true if works, false if not enough space
         */
        bool _pushBack(const DiskLoc& recordLoc, BSONObj& key, const Ordering &order, DiskLoc prevChild);
        void pushBack(const DiskLoc& recordLoc, BSONObj& key, const Ordering &order, DiskLoc prevChild){
            bool ok = _pushBack( recordLoc , key , order , prevChild );
            assert(ok);
        }
        void popBack(DiskLoc& recLoc, BSONObj& key);
        void _delKeyAtPos(int keypos); // low level version that doesn't deal with child ptrs.

        /* !Packed means there is deleted fragment space within the bucket.
           We "repack" when we run out of space before considering the node
           to be full.
           */
        enum Flags { Packed=1 };

        DiskLoc& childForPos(int p) {
            return p == n ? nextChild : k(p).prevChildBucket;
        }

        int totalDataSize() const;
        void pack( const Ordering &order, int &refPos);
        void setNotPacked();
        void setPacked();
        int _alloc(int bytes);
        void _unalloc(int bytes);
        void truncateTo(int N, const Ordering &order, int &refPos);
        void markUnused(int keypos);

        /* BtreeBuilder uses the parent var as a temp place to maintain a linked list chain. 
           we use tempNext() when we do that to be less confusing. (one might have written a union in C)
           */
        DiskLoc& tempNext() { return parent; }

    public:
        DiskLoc parent;

        string bucketSummary() const {
            stringstream ss;
            ss << "  Bucket info:" << endl;
            ss << "    n: " << n << endl;
            ss << "    parent: " << parent.toString() << endl;
            ss << "    nextChild: " << parent.toString() << endl;
            ss << "    flags:" << flags << endl;
            ss << "    emptySize: " << emptySize << " topSize: " << topSize << endl;
            return ss.str();
        }
        
        bool isUsed( int i ) const {
            return k(i).isUsed();
        }

    protected:
        void _shape(int level, stringstream&);
        DiskLoc nextChild; // child bucket off and to the right of the highest key.

    private:
        unsigned short _wasSize; // can be reused, value is 8192 in current pdfile version Apr2010
        unsigned short _reserved1; // zero

    protected:
        int Size() const;
        int flags;
        int emptySize; // size of the empty region
        int topSize; // size of the data at the top of the bucket (keys are at the beginning or 'bottom')
        int n; // # of keys so far.
        int reserved;
        const _KeyNode& k(int i) const {
            return ((_KeyNode*)data)[i];
        }
        _KeyNode& k(int i) {
            return ((_KeyNode*)data)[i];
        }
        char data[4];
    };
#pragma pack()

#pragma pack(1)
    class BtreeBucket : public BucketBasics {
        friend class BtreeCursor;
    public:
        void dump();

        /* @return true if key exists in index 

           order - indicates order of keys in the index.  this is basically the index's key pattern, e.g.:
             BSONObj order = ((IndexDetails&)idx).keyPattern();
           likewise below in bt_insert() etc.
        */
        bool exists(const IndexDetails& idx, DiskLoc thisLoc, const BSONObj& key, const Ordering& order);

        bool wouldCreateDup(
            const IndexDetails& idx, DiskLoc thisLoc, 
            const BSONObj& key, const Ordering& order,
            DiskLoc self); 

        static DiskLoc addBucket(IndexDetails&); /* start a new index off, empty */
        void deallocBucket(const DiskLoc &thisLoc, IndexDetails &id);
        
        static void renameIndexNamespace(const char *oldNs, const char *newNs);

        int bt_insert(DiskLoc thisLoc, DiskLoc recordLoc,
                   const BSONObj& key, const Ordering &order, bool dupsAllowed,
                   IndexDetails& idx, bool toplevel = true);

        bool unindex(const DiskLoc& thisLoc, IndexDetails& id, BSONObj& key, const DiskLoc& recordLoc);

        /* locate may return an "unused" key that is just a marker.  so be careful.
             looks for a key:recordloc pair.

           found - returns true if exact match found.  note you can get back a position 
                   result even if found is false.
        */
        DiskLoc locate(const IndexDetails& , const DiskLoc& thisLoc, const BSONObj& key, const Ordering &order, 
                       int& pos, bool& found, DiskLoc recordLoc, int direction=1);
        
        /**
         * find the first instance of the key
         * does not handle dups
         * returned DiskLock isNull if can't find anything with that
         */
        DiskLoc findSingle( const IndexDetails& , const DiskLoc& thisLoc, const BSONObj& key );

        /* advance one key position in the index: */
        DiskLoc advance(const DiskLoc& thisLoc, int& keyOfs, int direction, const char *caller);
        
        void advanceTo(DiskLoc &thisLoc, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction );
        void customLocate(DiskLoc &thisLoc, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction, pair< DiskLoc, int > &bestParent );
        
        DiskLoc getHead(const DiskLoc& thisLoc);

        /* get tree shape */
        void shape(stringstream&);

        static void a_test(IndexDetails&);

    private:
        void fixParentPtrs(const DiskLoc& thisLoc);
        void delBucket(const DiskLoc& thisLoc, IndexDetails&);
        void delKeyAtPos(const DiskLoc& thisLoc, IndexDetails& id, int p);
        BSONObj keyAt(int keyOfs) {
            return keyOfs >= n ? BSONObj() : keyNode(keyOfs).key;
        }
        static BtreeBucket* allocTemp(); /* caller must release with free() */
        void insertHere(DiskLoc thisLoc, int keypos,
                        DiskLoc recordLoc, const BSONObj& key, const Ordering &order,
                        DiskLoc lchild, DiskLoc rchild, IndexDetails&);
        int _insert(DiskLoc thisLoc, DiskLoc recordLoc,
                    const BSONObj& key, const Ordering &order, bool dupsAllowed,
                    DiskLoc lChild, DiskLoc rChild, IndexDetails&);
        bool find(const IndexDetails& idx, const BSONObj& key, DiskLoc recordLoc, const Ordering &order, int& pos, bool assertIfDup);
        bool customFind( int l, int h, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction, DiskLoc &thisLoc, int &keyOfs, pair< DiskLoc, int > &bestParent );
        static void findLargestKey(const DiskLoc& thisLoc, DiskLoc& largestLoc, int& largestKey);
        static int customBSONCmp( const BSONObj &l, const BSONObj &rBegin, int rBeginLen, bool rSup, const vector< const BSONElement * > &rEnd, const vector< bool > &rEndInclusive, const Ordering &o, int direction );
    public:
        // simply builds and returns a dup key error message string
        static string dupKeyError( const IndexDetails& idx , const BSONObj& key );
    };
#pragma pack()

    class BtreeCursor : public Cursor {
    public:
        BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails&, const BSONObj &startKey, const BSONObj &endKey, bool endKeyInclusive, int direction );

        BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails& _id, const shared_ptr< FieldRangeVector > &_bounds, int _direction );
        ~BtreeCursor(){
        }
        virtual bool ok() {
            return !bucket.isNull();
        }
        bool eof() {
            return !ok();
        }
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
            if( multikey ) { 
                pair<set<DiskLoc>::iterator, bool> p = dups.insert(loc);
                return !p.second;
            }
            return false;
        }

        _KeyNode& _currKeyNode() {
            assert( !bucket.isNull() );
            _KeyNode& kn = bucket.btree()->k(keyOfs);
            assert( kn.isUsed() );
            return kn;
        }
        KeyNode currKeyNode() const {
            assert( !bucket.isNull() );
            return bucket.btree()->keyNode(keyOfs);
        }
        virtual BSONObj currKey() const {
            return currKeyNode().key;
        }

        virtual BSONObj indexKeyPattern() {
            return indexDetails.keyPattern();
        }

        virtual void aboutToDeleteBucket(const DiskLoc& b) {
            if ( bucket == b )
                keyOfs = -1;
        }

        virtual DiskLoc currLoc() {
            return !bucket.isNull() ? _currKeyNode().recordLoc : DiskLoc();
        }
        virtual DiskLoc refLoc() {
            return currLoc();
        }
        virtual Record* _current() {
            return currLoc().rec();
        }
        virtual BSONObj current() {
            return BSONObj(_current());
        }
        virtual string toString() {
            string s = string("BtreeCursor ") + indexDetails.indexName();
            if ( direction < 0 ) s += " reverse";
            if ( bounds_.get() && bounds_->size() > 1 ) s += " multi";
            return s;
        }

        BSONObj prettyKey( const BSONObj &key ) const {
            return key.replaceFieldNames( indexDetails.keyPattern() ).clientReadable();
        }

        virtual BSONObj prettyIndexBounds() const {
            if ( !_independentFieldRanges ) {
                return BSON( "start" << prettyKey( startKey ) << "end" << prettyKey( endKey ) );
            } else {
                return bounds_->obj();
            }
        }
        
        void forgetEndKey() { endKey = BSONObj(); }

        virtual CoveredIndexMatcher *matcher() const { return _matcher.get(); }
        
        virtual void setMatcher( shared_ptr< CoveredIndexMatcher > matcher ) {
            _matcher = matcher;
        }

        virtual long long nscanned() { return _nscanned; }
        
        // for debugging only
        DiskLoc getBucket() const { return bucket; }
        
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
        set<DiskLoc> dups;
        NamespaceDetails *d;
        int idxNo;
        
        BSONObj startKey;
        BSONObj endKey;
        bool endKeyInclusive_;
        
        bool multikey; // note this must be updated every getmore batch in case someone added a multikey...

        const IndexDetails& indexDetails;
        BSONObj order;
        Ordering _ordering;
        DiskLoc bucket;
        int keyOfs;
        int direction; // 1=fwd,-1=reverse
        BSONObj keyAtKeyOfs; // so we can tell if things moved around on us between the query and the getMore call
        DiskLoc locAtKeyOfs;
        shared_ptr< FieldRangeVector > bounds_;
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
