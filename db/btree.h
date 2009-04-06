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

#include "../stdafx.h"
#include "jsobj.h"
#include "storage.h"
#include "pdfile.h"

namespace mongo {

#pragma pack(1)

    struct _KeyNode {
        DiskLoc prevChildBucket;
        DiskLoc recordLoc;
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
        int isUnused() {
            return recordLoc.getOfs() & 1;
        }
        int isUsed() {
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
        friend class KeyNode;
    public:
        void dumpTree(DiskLoc thisLoc, const BSONObj &order);
        bool isHead() { return parent.isNull(); }
        void assertValid(const BSONObj &order, bool force = false);
        int fullValidate(const DiskLoc& thisLoc, const BSONObj &order); /* traverses everything */
    protected:
        void modified(const DiskLoc& thisLoc);
        DiskLoc& getChild(int pos) {
            assert( pos >= 0 && pos <= n );
            return pos == n ? nextChild : k(pos).prevChildBucket;
        }
        KeyNode keyNode(int i) const {
            assert( i < n );
            return KeyNode(*this, k(i));
        }

        char * dataAt(short ofs) {
            return data + ofs;
        }

        void init(); // initialize a new node

        /* returns false if node is full and must be split
           keypos is where to insert -- inserted after that key #.  so keypos=0 is the leftmost one.
        */
        bool basicInsert(const DiskLoc& thisLoc, int keypos, const DiskLoc& recordLoc, BSONObj& key, const BSONObj &order);
        void pushBack(const DiskLoc& recordLoc, BSONObj& key, const BSONObj &order, DiskLoc prevChild);
        void _delKeyAtPos(int keypos); // low level version that doesn't deal with child ptrs.

        /* !Packed means there is deleted fragment space within the bucket.
           We "repack" when we run out of space before considering the node
           to be full.
           */
        enum Flags { Packed=1 };

        DiskLoc childForPos(int p) {
            return p == n ? nextChild : k(p).prevChildBucket;
        }

        int totalDataSize() const;
        void pack( const BSONObj &order );
        void setNotPacked();
        void setPacked();
        int _alloc(int bytes);
        void truncateTo(int N, const BSONObj &order);
        void markUnused(int keypos);
    public:
        DiskLoc parent;

        string bucketSummary() const {
            stringstream ss;
            ss << "  Bucket info:" << endl;
            ss << "    n: " << n << endl;
            ss << "    parent: " << parent.toString() << endl;
            ss << "    nextChild: " << parent.toString() << endl;
            ss << "    Size: " << _Size << " flags:" << flags << endl;
            ss << "    emptySize: " << emptySize << " topSize: " << topSize << endl;
            return ss.str();
        }

    protected:
        void _shape(int level, stringstream&);
        DiskLoc nextChild; // child bucket off and to the right of the highest key.
        int _Size; // total size of this btree node in bytes. constant.
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

    class BtreeBucket : public BucketBasics {
        friend class BtreeCursor;
    public:
        void dump();

        bool exists(const IndexDetails& idx, DiskLoc thisLoc, BSONObj& key, BSONObj order);

        static DiskLoc addHead(IndexDetails&); /* start a new index off, empty */

        int bt_insert(DiskLoc thisLoc, DiskLoc recordLoc,
                   BSONObj& key, const BSONObj &order, bool dupsAllowed,
                   IndexDetails& idx, bool toplevel = true);

        bool unindex(const DiskLoc& thisLoc, IndexDetails& id, BSONObj& key, const DiskLoc& recordLoc);

        /* locate may return an "unused" key that is just a marker.  so be careful.
             looks for a key:recordloc pair.
        */
        DiskLoc locate(const IndexDetails& , const DiskLoc& thisLoc, BSONObj& key, const BSONObj &order, 
                       int& pos, bool& found, DiskLoc recordLoc, int direction=1);

        /* advance one key position in the index: */
        DiskLoc advance(const DiskLoc& thisLoc, int& keyOfs, int direction, const char *caller);
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
                        DiskLoc recordLoc, BSONObj& key, const BSONObj &order,
                        DiskLoc lchild, DiskLoc rchild, IndexDetails&);
        int _insert(DiskLoc thisLoc, DiskLoc recordLoc,
                    BSONObj& key, const BSONObj &order, bool dupsAllowed,
                    DiskLoc lChild, DiskLoc rChild, IndexDetails&);
        bool find(const IndexDetails& idx, BSONObj& key, DiskLoc recordLoc, const BSONObj &order, int& pos, bool assertIfDup);
        static void findLargestKey(const DiskLoc& thisLoc, DiskLoc& largestLoc, int& largestKey);
    };

    class BtreeCursor : public Cursor {
        friend class BtreeBucket;
        BSONObj startKey;
        BSONObj endKey;
    public:
        BtreeCursor( const IndexDetails&, const BSONObj &startKey, const BSONObj &endKey, int direction );
        virtual bool ok() {
            return !bucket.isNull();
        }
        bool eof() {
            return !ok();
        }
        virtual bool advance();

        virtual void noteLocation(); // updates keyAtKeyOfs...
        virtual void checkLocation();

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
            return s;
        }

        BSONObj prettyKey( const BSONObj &key ) const {
            return key.replaceFieldNames( indexDetails.keyPattern() ).clientReadable();
        }
        
        virtual BSONObj prettyStartKey() const {
            return prettyKey( startKey );
        }
        virtual BSONObj prettyEndKey() const {
            return prettyKey( endKey );
        }
        
        void forgetEndKey() { endKey = BSONObj(); }
        
    private:
        /* Our btrees may (rarely) have "unused" keys when items are deleted.
           Skip past them.
        */
        void skipUnusedKeys();

        /* Check if the current key is beyond endKey. */
        void checkEnd();

        const IndexDetails& indexDetails;
        BSONObj order;
        DiskLoc bucket;
        int keyOfs;
        int direction; // 1=fwd,-1=reverse
        BSONObj keyAtKeyOfs; // so we can tell if things moved around on us between the query and the getMore call
        DiskLoc locAtKeyOfs;
    };

#pragma pack()

} // namespace mongo;
