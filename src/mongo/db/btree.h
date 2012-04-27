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

#include "pch.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/key.h"

namespace mongo {

    /**
     * Our btree implementation generally follows the standard btree algorithm,
     * which is described in many places.  The nodes of our btree are referred to
     * as buckets below.  These buckets are of size BucketSize and their body is
     * an ordered array of <bson key, disk loc> pairs, where disk loc is the disk
     * location of a document and bson key is a projection of this document into
     * the schema of the index for this btree.  Ordering is determined on the
     * basis of bson key first and then disk loc in case of a tie.  All bson keys
     * for a btree have identical schemas with empty string field names and may
     * not have an objsize() exceeding KeyMax.  The btree's buckets are
     * themselves organized into an ordered tree.  Although there are exceptions,
     * generally buckets with n keys have n+1 children and the body of a bucket is
     * at least lowWaterMark bytes.  A more strictly enforced requirement is that
     * a non root bucket must have at least one key except in certain transient
     * states.
     *
     * Our btrees support the following primary read operations: finding a
     * specified key; iterating from a starting key to the next or previous
     * ordered key; and skipping from a starting key to another specified key
     * without checking every intermediate key.  The primary write operations
     * are insertion and deletion of keys.  Insertion may trigger a bucket split
     * if necessary to avoid bucket overflow.  In such a case, subsequent splits
     * will occur recursively as necessary.  Deletion may trigger a bucket
     * rebalance, in which a size deficient bucket is filled with keys from an
     * adjacent bucket.  In this case, splitting may potentially occur in the
     * parent.  Deletion may alternatively trigger a merge, in which the keys
     * from two buckets and a key from their shared parent are combined into the
     * same bucket.  In such a case, rebalancing or merging may proceed
     * recursively from the parent.
     *
     * While the btree data format has been relatively constant over time, btrees
     * initially created by versions of mongo earlier than the current version
     * may embody different properties than freshly created btrees (while
     * following the same data format).  These older btrees are referred to
     * below as legacy btrees.
     */
    
    const int OldBucketSize = 8192;

#pragma pack(1)
    template< class Version > class BucketBasics;

    /**
     * This is the fixed width data component for storage of a key within a
     * bucket.  It contains an offset pointer to the variable width bson
     * data component.  A _KeyNode may be 'unused', please see below.
     */
    template< class Loc >
    struct __KeyNode {
        /** Signals that we are writing this _KeyNode and casts away const */
        __KeyNode<Loc> & writing() const;
        /**
         * The 'left' child bucket of this key.  If this is the i-th key, it
         * points to the i index child bucket.
         */
        Loc prevChildBucket;
        /** The location of the record associated with this key. */
        Loc recordLoc;
        short keyDataOfs() const { return (short) _kdo; }

        /** Offset within current bucket of the variable width bson key for this _KeyNode. */
        little<unsigned short> _kdo;
        void setKeyDataOfs(short s) {
            _kdo = s;
            verify(s>=0);
        }
        /** Seems to be redundant. */
        void setKeyDataOfsSavingUse(short s) {
            _kdo = s;
            verify(s>=0);
        }
        /**
         * Unused keys are not returned by read operations.  Keys may be marked
         * as unused in cases where it is difficult to delete them while
         * maintaining the constraints required of a btree.
         *
         * Setting ofs to odd is the sentinel for unused, as real recordLoc's
         * are always even numbers.  Note we need to keep its value basically
         * the same as we use the recordLoc as part of the key in the index
         * (to handle duplicate keys efficiently).
         *
         * Flagging keys as unused is a feature that is being phased out in favor
         * of deleting the keys outright.  The current btree implementation is
         * not expected to mark a key as unused in a non legacy btree.
         */
        void setUnused() {
            recordLoc.GETOFS() |= 1;
        }
        void setUsed() { recordLoc.GETOFS() &= ~1; }
        int isUnused() const {
            return recordLoc.getOfs() & 1;
        }
        int isUsed() const {
            return !isUnused();
        }
    };

    /**
     * This structure represents header data for a btree bucket.  An object of
     * this type is typically allocated inside of a buffer of size BucketSize,
     * resulting in a full bucket with an appropriate header.
     *
     * The body of a btree bucket contains an array of _KeyNode objects starting
     * from its lowest indexed bytes and growing to higher indexed bytes.  The
     * body also contains variable width bson keys, which are allocated from the
     * highest indexed bytes toward lower indexed bytes.
     *
     * |hhhh|kkkkkkk--------bbbbbbbbbbbuuubbbuubbb|
     * h = header data
     * k = KeyNode data
     * - = empty space
     * b = bson key data
     * u = unused (old) bson key data, that may be garbage collected
     */
    class BtreeData_V0 {
    protected:
        /** Parent bucket of this bucket, which isNull() for the root bucket. */
        DiskLoc parent;
        /** Given that there are n keys, this is the n index child. */
        DiskLoc nextChild;
        /** can be reused, value is 8192 in current pdfile version Apr2010 */
        little<unsigned short> _wasSize;
        /** zero */
        little<unsigned short> _reserved1;
        little<int> flags;

        void _init() {
            _reserved1 = 0;
            _wasSize = BucketSize;
            reserved = 0;
        }

        /** basicInsert() assumes the next three members are consecutive and in this order: */

        /** Size of the empty region. */
        little<int> emptySize;
        /** Size used for bson storage, including storage of old keys. */
        little<int> topSize;
        /* Number of keys in the bucket. */
        little<int> n;

        little<int> reserved;
        /* Beginning of the bucket's body */
        char data[4];

    public:
        typedef __KeyNode<DiskLoc> _KeyNode;
        typedef DiskLoc Loc;
        typedef KeyBson Key;
        typedef KeyBson KeyOwned;
        enum { BucketSize = 8192 };

        // largest key size we allow.  note we very much need to support bigger keys (somehow) in the future.
        static const int KeyMax = OldBucketSize / 10;
        // A sentinel value sometimes used to identify a deallocated bucket.
        enum { INVALID_N_SENTINEL = -1 };
    };

    // a a a ofs ofs ofs ofs
    class DiskLoc56Bit {
        little<int> ofs;
        unsigned char _a[3];
        unsigned long long Z() const { 
            // endian
            return little<unsigned long long>::ref(this) & 0x00ffffffffffffffULL;
        }
        enum { 
            // first bit of offsets used in _KeyNode we don't use -1 here.
            OurNullOfs = -2
        };
    public:
        template< class V >
        const BtreeBucket<V> * btree() const { 
            return DiskLoc(*this).btree<V>();
        }
        template< class V >
        BtreeBucket<V> * btreemod() const { 
            return DiskLoc(*this).btreemod<V>();
        }
        operator const DiskLoc() const { 
            // endian
            if( isNull() ) return DiskLoc();
            unsigned a = little<unsigned>::ref( _a - 1 );
            return DiskLoc(a >> 8, ofs);
        }
        little<int>& GETOFS()      { return ofs; }
        int getOfs() const { return ofs; }
        bool operator<(const DiskLoc56Bit& rhs) const {
            // the orderering of dup keys in btrees isn't too critical, but we'd like to put items that are 
            // close together on disk close together in the tree, so we do want the file # to be the most significant
            // bytes
            return Z() < rhs.Z();
        }
        int compare(const DiskLoc56Bit& rhs) const {
            unsigned long long a = Z();
            unsigned long long b = rhs.Z();
            if( a < b ) return -1;
            return a == b ? 0 : 1;
        }
        bool operator==(const DiskLoc56Bit& rhs) const { return Z() == rhs.Z(); }
        bool operator!=(const DiskLoc56Bit& rhs) const { return Z() != rhs.Z(); }
        bool operator==(const DiskLoc& rhs) const {
            return DiskLoc(*this) == rhs;
        }
        bool operator!=(const DiskLoc& rhs) const { return !(*this==rhs); }
        bool isNull() const { return ofs < 0; }
        void Null() { 
            ofs = OurNullOfs; 
            _a[0] = _a[1] = _a[2] = 0;
        }
        string toString() const { return DiskLoc(*this).toString(); }

        void operator=(const DiskLoc& loc) {
            ofs = loc.getOfs();
            int la = loc.a();
            verify( la <= 0xffffff ); // must fit in 3 bytes
            if( la < 0 ) {
                if ( la != -1 ) {
                    log() << "btree diskloc isn't negative 1: " << la << endl;
                    verify ( la == -1 );
                }
                la = 0;
                ofs = OurNullOfs;
            }
            little<int> lila = la;
            memcpy(_a, &lila, 3); // endian
            dassert( ofs != 0 );
        }
        DiskLoc56Bit& writing() const { 
            return *((DiskLoc56Bit*) getDur().writingPtr((void*)this, 7));
        }
    };

    class BtreeData_V1 {
    public:
        typedef DiskLoc56Bit Loc;
        //typedef DiskLoc Loc;
        typedef __KeyNode<Loc> _KeyNode;
        typedef KeyV1 Key;
        typedef KeyV1Owned KeyOwned;
        enum { BucketSize = 8192-16 }; // leave room for Record header
        // largest key size we allow.  note we very much need to support bigger keys (somehow) in the future.
        static const int KeyMax = 1024;
        // A sentinel value sometimes used to identify a deallocated bucket.
        static const unsigned short INVALID_N_SENTINEL = 0xffff;
    protected:
        /** Parent bucket of this bucket, which isNull() for the root bucket. */
        Loc parent;
        /** Given that there are n keys, this is the n index child. */
        Loc nextChild;

        little<unsigned short> flags;

        /** basicInsert() assumes the next three members are consecutive and in this order: */

        /** Size of the empty region. */
        little<unsigned short> emptySize;
        /** Size used for bson storage, including storage of old keys. */
        little<unsigned short> topSize;
        /* Number of keys in the bucket. */
        unsigned short n;

        /* Beginning of the bucket's body */
        char data[4];

        void _init() { }
    };

    typedef BtreeData_V0 V0;
    typedef BtreeData_V1 V1;

    /**
     * This class adds functionality to BtreeData for managing a single bucket.
     * The following policies are used in an attempt to encourage simplicity:
     *
     * Const member functions of this class are those which may be called on
     * an object for which writing has not been signaled.  Non const member
     * functions may only be called on objects for which writing has been
     * signaled.  Note that currently some const functions write to the
     * underlying memory representation of this bucket using optimized methods
     * to signal write operations.
     *
     * DiskLoc parameters that may shadow references within the btree should
     * be passed by value rather than by reference to non const member
     * functions or to const member functions which may perform writes.  This way
     * a callee need not worry that write operations will change or invalidate
     * its arguments.
     *
     * The current policy for dealing with bson arguments is the opposite of
     * what is described above for DiskLoc arguments.  We do not want to copy
     * bson into memory as an intermediate step for btree changes, and if bson
     * is to be moved it must be copied to the new location before the old
     * location is invalidated.  Care should be taken in cases where that invalid
     * memory may be implicitly referenced by function arguments.
     *
     * A number of functions below require a thisLoc argument, which must be the
     * disk location of the bucket mapped to 'this'.
     */
    template< class Version >
    class BucketBasics : public Version {
    public:
        template <class U> friend class BtreeBuilder;
        typedef typename Version::Key Key;
        typedef typename Version::_KeyNode _KeyNode;
        typedef typename Version::Loc Loc;

        int getN() const { return this->n; }

        /**
         * This is an in memory wrapper for a _KeyNode, and not itself part of btree
         * storage.  This object and its BSONObj 'key' will become invalid if the
         * _KeyNode data that generated it is moved within the btree.  In general,
         * a KeyNode should not be expected to be valid after a write.
         */
        class KeyNode {
        public:
            KeyNode(const BucketBasics<Version>& bb, const _KeyNode &k);
            const Loc& prevChildBucket;
            const Loc& recordLoc;
            /* Points to the bson key storage for a _KeyNode */
            Key key;
        };
        friend class KeyNode;

        /** Assert write intent declared for this bucket already. */
        void assertWritable();

        void assertValid(const Ordering &order, bool force = false) const;
        void assertValid(const BSONObj &orderObj, bool force = false) const { return assertValid(Ordering::make(orderObj),force); }

        /**
         * @return KeyNode for key at index i.  The KeyNode will become invalid
         * if the key is moved or reassigned, or if the node is packed.  In general
         * a KeyNode should not be expected to be valid after a write.
         */
        const KeyNode keyNode(int i) const {
            if ( i >= this->n ) {
                massert( 13000 , (string)"invalid keyNode: " +  BSON( "i" << i << "n" << this->n ).jsonString() , i < this->n );
            }
            return KeyNode(*this, k(i));
        }

        static int headerSize() {
            const BucketBasics *d = 0;
            return (char*)&(d->data) - (char*)&(d->parent);
        }
        static int bodySize() { return Version::BucketSize - headerSize(); }
        static int lowWaterMark() { return bodySize() / 2 - Version::KeyMax - sizeof( _KeyNode ) + 1; } // see comment in btree.cpp

        // for testing
        int nKeys() const { return this->n; }
        const DiskLoc getNextChild() const { return this->nextChild; }

    protected:
        char * dataAt(short ofs) { return this->data + ofs; }

        /** Initialize the header for a new node. */
        void init();

        /**
         * Preconditions:
         *  - 0 <= keypos <= n
         *  - If key is inserted at position keypos, the bucket's keys will still be
         *    in order.
         * Postconditions:
         *  - If key can fit in the bucket, the bucket may be packed and keypos
         *    may be decreased to reflect deletion of earlier indexed keys during
         *    packing, the key will be inserted at the updated keypos index with
         *    a null prevChildBucket, the subsequent keys shifted to the right,
         *    and the function will return true.
         *  - If key cannot fit in the bucket, the bucket will be packed and
         *    the function will return false.
         * Although this function is marked const, it modifies the underlying
         * btree representation through an optimized write intent mechanism.
         */
        bool basicInsert(const DiskLoc thisLoc, int &keypos, const DiskLoc recordLoc, const Key& key, const Ordering &order) const;

        /**
         * Preconditions:
         *  - key / recordLoc are > all existing keys
         *  - The keys in prevChild and their descendents are between all existing
         *    keys and 'key'.
         * Postconditions:
         *  - If there is space for key without packing, it is inserted as the
         *    last key with specified prevChild and true is returned.
         *    Importantly, nextChild is not updated!
         *  - Otherwise false is returned and there is no change.
         */
        bool _pushBack(const DiskLoc recordLoc, const Key& key, const Ordering &order, const DiskLoc prevChild);
        void pushBack(const DiskLoc recordLoc, const Key& key, const Ordering &order, const DiskLoc prevChild) {
            bool ok = _pushBack( recordLoc , key , order , prevChild );
            verify(ok);
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
         *
         * Preconditions:
         *  - bucket is not empty
         *  - last key of bucket is used (not unused)
         *  - nextChild isNull()
         *  - _unalloc will work correctly as used - see code
         * Postconditions:
         *  - The last key of the bucket is removed, and its key and recLoc are
         *    returned.  As mentioned above, the key points to unallocated memory.
         */
        void popBack(DiskLoc& recLoc, Key &key);

        /**
         * Preconditions:
         *  - 0 <= keypos < n
         *  - there is no child bucket at keypos
         *  - n > 1
         *  - if mayEmpty == false or nextChild.isNull(), n > 0
         * Postconditions:
         *  - The key at keypos is removed, and remaining keys are shifted over.
         *  - The bucket becomes unpacked.
         *  - if mayEmpty is true and nextChild.isNull(), the bucket may have no keys.
         */
        void _delKeyAtPos(int keypos, bool mayEmpty = false);

        /* !Packed means there is deleted fragment space within the bucket.
           We "repack" when we run out of space before considering the node
           to be full.
           */
        enum Flags { Packed=1 };

        /** n == 0 is ok */
        const Loc& childForPos(int p) const { return p == this->n ? this->nextChild : k(p).prevChildBucket; }
        Loc& childForPos(int p) { return p == this->n ? this->nextChild : k(p).prevChildBucket; }

        /** Same as bodySize(). */
        int totalDataSize() const;
        /**
         * @return true when a key may be dropped by pack()
         * @param index index of the key that may be dropped
         * @param refPos index of a particular key of interest, which must not
         *  be dropped; = 0 to safely ignore
         */
        bool mayDropKey( int index, int refPos ) const;

        /**
         * Pack the bucket to reclaim space from invalidated memory.
         * @refPos is an index in the bucket which may be updated if we
         *  delete keys from the bucket
         * This function may cast away const and perform a write.
         * Preconditions: none
         * Postconditions:
         *  - Bucket will be packed
         *  - Some unused nodes may be dropped, but not ones at index 0 or refPos
         *  - Some used nodes may be moved
         *  - If refPos is the index of an existing key, it will be updated to that
         *    key's new index if the key is moved.
         */
        void _pack(const DiskLoc thisLoc, const Ordering &order, int &refPos) const;
        /** Pack when already writable */
        void _packReadyForMod(const Ordering &order, int &refPos);

        /** @return the size the bucket's body would have if we were to call pack() */
        int packedDataSize( int refPos ) const;
        void setNotPacked() { this->flags &= ~Packed; }
        void setPacked() { this->flags |= Packed; }
        /**
         * Preconditions: 'bytes' is <= emptySize
         * Postconditions: A buffer of size 'bytes' is allocated on the top side,
         *  and its offset is returned.
         */
        int _alloc(int bytes);
        /**
         * This function can be used to deallocate the lowest byte index bson
         * buffer in the top region, which in some but not all cases is for the
         * n - 1 index key.  This function only works correctly in certain
         * special cases, please be careful.
         * Preconditions: 'bytes' <= topSize
         * Postconditions: The top region is decreased
         */
        void _unalloc(int bytes);
        /**
         * Preconditions: 'N' <= n
         * Postconditions:
         *  - All keys after the N index key are dropped.
         *  - Then bucket is packed, without dropping refPos if < refPos N.
         */
        void truncateTo(int N, const Ordering &order, int &refPos);
        /**
         * Preconditions:
         *  - 'nDrop' < n
         *  - for now, refPos should be zero.
         * Postconditions:
         *  - All keys before the nDrop index key are dropped.
         *  - The bucket is packed.
         */
        void dropFront(int nDrop, const Ordering &order, int &refPos);
        /**
         * Preconditions: 0 <= keypos < n
         * Postconditions: keypos indexed key is marked unused.
         */
        void markUnused(int keypos);

        /**
         * BtreeBuilder uses the parent var as a temp place to maintain a linked list chain.
         *   we use tempNext() when we do that to be less confusing. (one might have written a union in C)
         */
        DiskLoc tempNext() const { return this->parent; }
        void setTempNext(DiskLoc l) { this->parent = l; }

        void _shape(int level, stringstream&) const;
        int Size() const;
        
        /** @return i-indexed _KeyNode, without bounds checking */
    public:
        const _KeyNode& k(int i) const { return ((const _KeyNode*)this->data)[i]; }
        _KeyNode& _k(int i) { return ((_KeyNode*)this->data)[i]; }
    protected:        
        _KeyNode& k(int i) { return ((_KeyNode*)this->data)[i]; }

        /**
         * Preconditions: 'this' is packed
         * @return the key index to be promoted on split
         * @param keypos The requested index of a key to insert, which may affect
         *  the choice of split position.
         */
        int splitPos( int keypos ) const;

        /**
         * Preconditions: nAdd * sizeof( _KeyNode ) <= emptySize
         * Postconditions:
         *  - Increases indexes of existing _KeyNode objects by nAdd, reserving
         *    space for additional _KeyNode objects at front.
         *  - Does not initialize ofs values for the bson data of these
         *    _KeyNode objects.
         */
        void reserveKeysFront( int nAdd );

        /**
         * Preconditions:
         *  - 0 <= i < n
         *  - The bson 'key' must fit in the bucket without packing.
         *  - If 'key' and 'prevChildBucket' are set at index i, the btree
         *    ordering properties will be maintained.
         * Postconditions:
         *  - The specified key is set at index i, replacing the existing
         *    _KeyNode data and without shifting any other _KeyNode objects.
         */
        void setKey( int i, const DiskLoc recordLoc, const Key& key, const DiskLoc prevChildBucket );
    };

    class IndexInsertionContinuation;

    template< class V>
    struct IndexInsertionContinuationImpl;

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
     * TODO There are several cases in which the 'this' pointer is invalidated
     * as a result of deallocation.  A seperate class representing a btree would
     * alleviate some fragile cases where the implementation must currently
     * behave correctly if the 'this' pointer is suddenly invalidated by a
     * callee.
     */
    template< class V >
    class BtreeBucket : public BucketBasics<V> {
        friend class BtreeCursor;
        friend struct IndexInsertionContinuationImpl<V>;
    public:
	// make compiler happy:
        typedef typename V::Key Key;
        typedef typename V::KeyOwned KeyOwned;
	typedef typename BucketBasics<V>::KeyNode KeyNode;
	typedef typename BucketBasics<V>::_KeyNode _KeyNode;
	typedef typename BucketBasics<V>::Loc Loc;
        const _KeyNode& k(int i) const     { return static_cast< const BucketBasics<V> * >(this)->k(i); }
    protected:
        _KeyNode& k(int i)                 { return static_cast< BucketBasics<V> * >(this)->_k(i); }
    public:
        const KeyNode keyNode(int i) const { return static_cast< const BucketBasics<V> * >(this)->keyNode(i); }

        bool isHead() const { return this->parent.isNull(); }
        void dumpTree(const DiskLoc &thisLoc, const BSONObj &order) const;
        long long fullValidate(const DiskLoc& thisLoc, const BSONObj &order, long long *unusedCount = 0, bool strict = false, unsigned depth=0) const; /* traverses everything */

        bool isUsed( int i ) const { return this->k(i).isUsed(); }
        string bucketSummary() const;
        void dump(unsigned depth=0) const;

        /**
         * @return true if key exists in index
         *
         * @order - indicates order of keys in the index.  this is basically the index's key pattern, e.g.:
         *    BSONObj order = ((IndexDetails&)idx).keyPattern();
         * likewise below in bt_insert() etc.
         */
    private:
        bool exists(const IndexDetails& idx, const DiskLoc &thisLoc, const Key& key, const Ordering& order) const;
    public:

        /**
         * @param self - Don't complain about ourself already being in the index case.
         * @return true = There is a duplicate used key.
         */
        bool wouldCreateDup(
            const IndexDetails& idx, const DiskLoc &thisLoc,
            const Key& key, const Ordering& order,
            const DiskLoc &self) const;

        /**
         * Preconditions: none
         * Postconditions: @return a new bucket allocated from pdfile storage
         *  and init()-ed.  This bucket is suitable to for use as a new root
         *  or any other new node in the tree.
         */
        static DiskLoc addBucket(const IndexDetails&);

        /**
         * Preconditions: none
         * Postconditions:
         *  - Some header values in this bucket are cleared, and the bucket is
         *    deallocated from pdfile storage.
         *  - The memory at thisLoc is invalidated, and 'this' is invalidated.
         */
        void deallocBucket(const DiskLoc thisLoc, const IndexDetails &id);

        /**
         * Preconditions:
         *  - 'key' has a valid schema for this index.
         *  - All other paramenters are valid and consistent with this index if applicable.
         * Postconditions:
         *  - If key is bigger than KeyMax, @return 2 or 3 and no change.
         *  - If key / recordLoc exist in the btree as an unused key, set them
         *    as used and @return 0
         *  - If key / recordLoc exist in the btree as a used key, @throw
         *    exception 10287 and no change.
         *  - If key / recordLoc do not exist in the btree, they are inserted
         *    and @return 0.  The root of the btree may be changed, so
         *    'this'/thisLoc may no longer be the root upon return.
         */
        int bt_insert(const DiskLoc thisLoc, const DiskLoc recordLoc,
                      const BSONObj& key, const Ordering &order, bool dupsAllowed,
                      IndexDetails& idx, bool toplevel = true) const;

        /** does the insert in two steps - can then use an upgradable lock for step 1, which 
            is the part which may have page faults.  also that step is most of the computational work.
        */
        void twoStepInsert(DiskLoc thisLoc, IndexInsertionContinuationImpl<V> &c, bool dupsAllowed) const;

        /**
         * Preconditions:
         *  - 'key' has a valid schema for this index, and may have objsize() > KeyMax.
         * Postconditions:
         *  - If key / recordLoc are in the btree, they are removed (possibly
         *    by being marked as an unused key), @return true, and potentially
         *    invalidate 'this' / thisLoc and change the head.
         *  - If key / recordLoc are not in the btree, @return false and do nothing.
         */
        bool unindex(const DiskLoc thisLoc, IndexDetails& id, const BSONObj& key, const DiskLoc recordLoc) const;

        /**
         * locate may return an "unused" key that is just a marker.  so be careful.
         *   looks for a key:recordloc pair.
         *
         * @found - returns true if exact match found.  note you can get back a position
         *          result even if found is false.
         */
        DiskLoc locate(const IndexDetails &idx , const DiskLoc& thisLoc, const BSONObj& key, const Ordering &order,
                       int& pos, bool& found, const DiskLoc &recordLoc, int direction=1) const;
        DiskLoc locate(const IndexDetails &idx , const DiskLoc& thisLoc, const Key& key, const Ordering &order,
                       int& pos, bool& found, const DiskLoc &recordLoc, int direction=1) const;

        /**
         * find the first instance of the key
         * does not handle dups
         * WARNING: findSingle may not be compound index safe.  this may need to change.  see notes in 
         *          findSingle code.
         * @return the record location of the first match
         */
        DiskLoc findSingle( const IndexDetails &indexdetails , const DiskLoc& thisLoc, const BSONObj& key ) const;

        /**
         * Advance to next or previous key in the index.
         * @param direction to advance.
         */
        DiskLoc advance(const DiskLoc& thisLoc, int& keyOfs, int direction, const char *caller) const;

        /** Advance in specified direction to the specified key */
        void advanceTo(DiskLoc &thisLoc, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction ) const;

        /** Locate a key with fields comprised of a combination of keyBegin fields and keyEnd fields. */
        static void customLocate(DiskLoc &locInOut, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction, pair< DiskLoc, int > &bestParent ) ;

        /** @return head of the btree by traversing from current bucket. */
        const DiskLoc getHead(const DiskLoc& thisLoc) const;

        /** get tree shape */
        void shape(stringstream&) const;

        static void a_test(IndexDetails&);

        static int getKeyMax();

    protected:
        /**
         * Preconditions:
         *  - 0 <= firstIndex <= n
         *  - -1 <= lastIndex <= n ( -1 is equivalent to n )
         * Postconditions:
         *  - Any children at indexes firstIndex through lastIndex (inclusive)
         *    will have their parent pointers set to thisLoc.
         */
        void fixParentPtrs(const DiskLoc thisLoc, int firstIndex = 0, int lastIndex = -1) const;

        /**
         * Preconditions:
         *  - thisLoc is not the btree head.
         *  - n == 0 is ok
         * Postconditions:
         *  - All cursors pointing to this bucket will be updated.
         *  - This bucket's parent's child pointer is set to null.
         *  - This bucket is deallocated from pdfile storage.
         *  - 'this' and thisLoc are invalidated.
         */
        void delBucket(const DiskLoc thisLoc, const IndexDetails&);

        /**
         * Preconditions: 0 <= p < n
         * Postconditions:
         *  - The key at index p is removed from the btree.
         *  - 'this' and thisLoc may be invalidated.
         *  - The tree head may change.
         */
        void delKeyAtPos(const DiskLoc thisLoc, IndexDetails& id, int p, const Ordering &order);

        /**
         * Preconditions:
         *  - n == 0 is ok
         * Postconditions:
         *  - If thisLoc is head, or if its body has at least lowWaterMark bytes,
         *    return false and do nothing.
         *  - Otherwise, if thisLoc has left or right neighbors, either balance
         *    or merge with them and return true.  Also, 'this' and thisLoc may
         *    be invalidated and the tree head may change.
         */
        bool mayBalanceWithNeighbors(const DiskLoc thisLoc, IndexDetails &id, const Ordering &order) const;

        /**
         * Preconditions:
         *  - 0 <= leftIndex < n
         *  - The child at leftIndex or the child at leftIndex + 1 contains
         *    fewer than lowWaterMark bytes.
         * Postconditions:
         *  - If the child bucket at leftIndex can merge with the child index
         *    at leftIndex + 1, do nothing and return false.
         *  - Otherwise, balance keys between the leftIndex child and the
         *    leftIndex + 1 child, return true, and possibly change the tree head.
         */
        bool tryBalanceChildren( const DiskLoc thisLoc, int leftIndex, IndexDetails &id, const Ordering &order ) const;

        /**
         * Preconditions:
         *  - All preconditions of tryBalanceChildren.
         *  - The leftIndex child and leftIndex + 1 child cannot be merged.
         * Postconditions:
         *  - Keys are moved between the leftIndex child and the leftIndex + 1
         *    child such that neither child has fewer than lowWaterMark bytes.
         *    The tree head may change.
         */
        void doBalanceChildren( const DiskLoc thisLoc, int leftIndex, IndexDetails &id, const Ordering &order );
        
        /**
         * Preconditions:
         *  - All preconditions of doBalanceChildren
         *  - The leftIndex and leftIndex + 1 children are packed.
         *  - The leftIndex + 1 child has fewer than lowWaterMark bytes.
         *  - split returned by rebalancedSeparatorPos()
         * Postconditions:
         *  - The key in lchild at index split is set as thisLoc's key at index
         *    leftIndex, which may trigger a split and change the tree head.
         *    The previous key in thisLoc at index leftIndex and all keys with
         *    indexes greater than split in lchild are moved to rchild.
         */
        void doBalanceLeftToRight( const DiskLoc thisLoc, int leftIndex, int split,
                                   BtreeBucket<V> *l, const DiskLoc lchild,
                                   BtreeBucket<V> *r, const DiskLoc rchild,
                                   IndexDetails &id, const Ordering &order );
        /**
         * Preconditions:
         *  - All preconditions of doBalanceChildren
         *  - The leftIndex and leftIndex + 1 children are packed.
         *  - The leftIndex child has fewer than lowWaterMark bytes.
         *  - split returned by rebalancedSeparatorPos()
         * Postconditions:
         *  - The key in rchild at index split - l->n - 1 is set as thisLoc's key
         *    at index leftIndex, which may trigger a split and change the tree
         *    head.  The previous key in thisLoc at index leftIndex and all keys
         *    with indexes less than split - l->n - 1 in rchild are moved to
         *    lchild.
         */        
        void doBalanceRightToLeft( const DiskLoc thisLoc, int leftIndex, int split,
                                   BtreeBucket<V> *l, const DiskLoc lchild,
                                   BtreeBucket<V> *r, const DiskLoc rchild,
                                   IndexDetails &id, const Ordering &order );

        /**
         * Preconditions:
         *  - 0 <= leftIndex < n
         *  - this->canMergeChildren( thisLoc, leftIndex ) == true
         * Postconditions:
         *  - All of the above mentioned keys will be placed in the left child.
         *  - The tree may be updated recursively, resulting in 'this' and
         *    thisLoc being invalidated and the tree head being changed.
         */
        void doMergeChildren( const DiskLoc thisLoc, int leftIndex, IndexDetails &id, const Ordering &order);

        /**
         * Preconditions:
         *  - n == 0
         *  - !nextChild.isNull()
         * Postconditions:
         *  - 'this' and thisLoc are deallocated (and invalidated), any cursors
         *    to them are updated, and the tree head may change.
         *  - nextChild replaces thisLoc in the btree structure.
         */
        void replaceWithNextChild( const DiskLoc thisLoc, IndexDetails &id );

        /**
         * @return true iff the leftIndex and leftIndex + 1 children both exist,
         *  and if their body sizes when packed and the thisLoc key at leftIndex
         *  would fit in a single bucket body.
         */
        bool canMergeChildren( const DiskLoc &thisLoc, int leftIndex ) const;

        /**
         * Preconditions:
         *  - leftIndex and leftIndex + 1 children are packed
         *  - leftIndex or leftIndex + 1 child is below lowWaterMark
         * @return index of the rebalanced separator; the index value is
         *  determined as if we had a bucket with body
         *  <left bucket keys array>.push( <old separator> ).concat( <right bucket keys array> )
         *  and called splitPos( 0 ) on it.
         */
        int rebalancedSeparatorPos( const DiskLoc &thisLoc, int leftIndex ) const;

        /**
         * Preconditions: thisLoc has a parent
         * @return parent's index of thisLoc.
         */
        int indexInParent( const DiskLoc &thisLoc ) const;        

    public:
        Key keyAt(int i) const {
            if( i >= this->n ) 
                return Key();
            return Key(this->data + k(i).keyDataOfs());
        }
    protected:

        /**
         * Preconditions:
         *  - This bucket is packed.
         *  - Cannot add a key of size KeyMax to this bucket.
         *  - 0 <= keypos <= n is the position of a new key that will be inserted
         *  - lchild is equal to the existing child at index keypos.
         * Postconditions:
         *  - The thisLoc bucket is split into two packed buckets, possibly
         *    invalidating the initial position of keypos, with a split key
         *    promoted to the parent.  The new key key/recordLoc will be inserted
         *    into one of the split buckets, and lchild/rchild set appropriately.
         *    Splitting may occur recursively, possibly changing the tree head.
         */
        void split(const DiskLoc thisLoc, int keypos,
                   const DiskLoc recordLoc, const Key& key,
                   const Ordering& order, const DiskLoc lchild, const DiskLoc rchild, IndexDetails& idx);

        /**
         * Preconditions:
         *  - 0 <= keypos <= n
         *  - If key / recordLoc are inserted at position keypos, with provided
         *    lchild and rchild, the btree ordering requirements will be
         *    maintained.
         *  - lchild is equal to the existing child at index keypos.
         *  - n == 0 is ok.
         * Postconditions:
         *  - The key / recordLoc are inserted at position keypos, and the
         *    bucket is split if necessary, which may change the tree head.
         *  - The bucket may be packed or split, invalidating the specified value
         *    of keypos.
         * This function will always modify thisLoc, but it's marked const because
         * it commonly relies on the specialized writ]e intent mechanism of basicInsert().
         */
        void insertHere(const DiskLoc thisLoc, int keypos,
                        const DiskLoc recordLoc, const Key& key, const Ordering &order,
                        const DiskLoc lchild, const DiskLoc rchild, IndexDetails &idx) const;

        /** bt_insert() is basically just a wrapper around this. */
        int _insert(const DiskLoc thisLoc, const DiskLoc recordLoc,
                    const Key& key, const Ordering &order, bool dupsAllowed,
                    const DiskLoc lChild, const DiskLoc rChild, IndexDetails &idx) const;

        void insertStepOne(
                DiskLoc thisLoc, IndexInsertionContinuationImpl<V>& c, bool dupsAllowed) const;

        bool find(const IndexDetails& idx, const Key& key, const DiskLoc &recordLoc, const Ordering &order, int& pos, bool assertIfDup) const;        
        static bool customFind( int l, int h, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction, DiskLoc &thisLoc, int &keyOfs, pair< DiskLoc, int > &bestParent ) ;
        static void findLargestKey(const DiskLoc& thisLoc, DiskLoc& largestLoc, int& largestKey);
        static int customBSONCmp( const BSONObj &l, const BSONObj &rBegin, int rBeginLen, bool rSup, const vector< const BSONElement * > &rEnd, const vector< bool > &rEndInclusive, const Ordering &o, int direction );
        
        /** If child is non null, set its parent to thisLoc */
        static void fix(const DiskLoc thisLoc, const DiskLoc child);

        /**
         * Preconditions:
         *  - 0 <= keypos < n
         *  - If the specified key and recordLoc are placed in keypos of thisLoc,
         *    and lchild and rchild are set, the btree ordering properties will
         *    be maintained.
         *  - rchild == childForPos( keypos + 1 )
         *  - childForPos( keypos ) is referenced elsewhere if nonnull.
         * Postconditions:
         *  - The key at keypos will be replaced with the specified key and
         *    lchild, potentially splitting this bucket and changing the tree
         *    head.
         *  - childForPos( keypos ) will be orphaned.
         */
        void setInternalKey( const DiskLoc thisLoc, int keypos,
                             const DiskLoc recordLoc, const Key &key, const Ordering &order,
                             const DiskLoc lchild, const DiskLoc rchild, IndexDetails &idx);

        /**
         * Preconditions:
         *  - 0 <= keypos < n
         *  - The keypos or keypos+1 indexed child is non null.
         * Postconditions:
         *  - The specified key is deleted by replacing it with another key if
         *    possible.  This replacement may cause a split and change the tree
         *    head.  The replacement key will be deleted from its original
         *    location, potentially causing merges and splits that may invalidate
         *    'this' and thisLoc and change the tree head.
         *  - If the key cannot be replaced, it will be marked as unused.  This
         *    is only expected in legacy btrees.
         */
        void deleteInternalKey( const DiskLoc thisLoc, int keypos, IndexDetails &id, const Ordering &order );
    public:
        /** simply builds and returns a dup key error message string */
        static string dupKeyError( const IndexDetails& idx , const Key& key );
    };
#pragma pack()

    class FieldRangeVector;
    class FieldRangeVectorIterator;
    
    /**
     * A Cursor class for Btree iteration.
     *
     * A BtreeCursor can record its current btree position (noteLoc()) and then relocate this
     * position after a write (checkLoc()).  A recorded btree position consists of a btree bucket,
     * bucket key offset, and unique btree key.  To relocate a unique btree key, a BtreeCursor first
     * checks the btree key at its recorded btree bucket and bucket key offset.  If the key at that
     * location does not match the recorded btree key, and an adjacent key also fails to match,
     * the recorded key (or the next existing key following it) is located in the btree using binary
     * search.  If the recorded btree bucket is invalidated, the initial recorded bucket check is
     * not attempted (see SERVER-4575).
     */
    class BtreeCursor : public Cursor {
    protected:
        BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails&, const BSONObj &startKey, const BSONObj &endKey, bool endKeyInclusive, int direction );
        BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails& _id,
                    const shared_ptr< FieldRangeVector > &_bounds, int singleIntervalLimit,
                    int _direction );
    public:
        virtual ~BtreeCursor();
        /** makes an appropriate subclass depending on the index version */
        static BtreeCursor* make( NamespaceDetails *_d, const IndexDetails&, const BSONObj &startKey, const BSONObj &endKey, bool endKeyInclusive, int direction );
        static BtreeCursor* make( NamespaceDetails *_d, const IndexDetails& _id, const shared_ptr< FieldRangeVector > &_bounds, int _direction );
        static BtreeCursor* make( NamespaceDetails *_d, int _idxNo, const IndexDetails&, const BSONObj &startKey, const BSONObj &endKey, bool endKeyInclusive, int direction );
        static BtreeCursor* make( NamespaceDetails *_d, int _idxNo, const IndexDetails& _id,
                                 const shared_ptr< FieldRangeVector > &_bounds,
                                 int singleIntervalLimit, int _direction );

        virtual bool ok() { return !bucket.isNull(); }
        virtual bool advance();
        virtual void noteLocation(); // updates keyAtKeyOfs...
        virtual void checkLocation() = 0;
        virtual bool supportGetMore() { return true; }
        virtual bool supportYields() { return true; }

        /**
         * used for multikey index traversal to avoid sending back dups. see Matcher::matches().
         * if a multikey index traversal:
         *   if loc has already been sent, returns true.
         *   otherwise, marks loc as sent.
         * @return false if the loc has not been seen
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

        /*const _KeyNode& _currKeyNode() const {
            verify( !bucket.isNull() );
            const _KeyNode& kn = keyNode(keyOfs);
            verify( kn.isUsed() );
            return kn;
        }*/

        /** returns BSONObj() if ofs is out of range */
        virtual BSONObj keyAt(int ofs) const = 0;

        virtual BSONObj currKey() const = 0;
        virtual BSONObj indexKeyPattern() { return indexDetails.keyPattern(); }

        virtual void aboutToDeleteBucket(const DiskLoc& b) {
            if ( bucket == b )
                keyOfs = -1;
        }

        virtual DiskLoc currLoc() = 0; //  { return !bucket.isNull() ? _currKeyNode().recordLoc : DiskLoc();  }
        virtual DiskLoc refLoc()   { return currLoc(); }
        virtual Record* _current() { return currLoc().rec(); }
        virtual BSONObj current()  { return BSONObj(_current()); }
        virtual string toString();

        BSONObj prettyKey( const BSONObj &key ) const {
            return key.replaceFieldNames( indexDetails.keyPattern() ).clientReadable();
        }

        virtual BSONObj prettyIndexBounds() const;

        virtual CoveredIndexMatcher *matcher() const { return _matcher.get(); }
        virtual shared_ptr< CoveredIndexMatcher > matcherPtr() const { return _matcher; }

        virtual void setMatcher( shared_ptr< CoveredIndexMatcher > matcher ) { _matcher = matcher;  }

        virtual const Projection::KeyOnly *keyFieldsOnly() const { return _keyFieldsOnly.get(); }
        
        virtual void setKeyFieldsOnly( const shared_ptr<Projection::KeyOnly> &keyFieldsOnly ) {
            _keyFieldsOnly = keyFieldsOnly;
        }
        
        virtual long long nscanned() { return _nscanned; }

        /** for debugging only */
        const DiskLoc getBucket() const { return bucket; }
        int getKeyOfs() const { return keyOfs; }

        // just for unit tests
        virtual bool curKeyHasChild() = 0;

    protected:
        /**
         * Our btrees may (rarely) have "unused" keys when items are deleted.
         * Skip past them.
         */
        virtual bool skipUnusedKeys() = 0;

        bool skipOutOfRangeKeysAndCheckEnd();
        void skipAndCheck();
        void checkEnd();

        /** selective audits on construction */
        void audit();

        virtual void _audit() = 0;
        virtual DiskLoc _locate(const BSONObj& key, const DiskLoc& loc) = 0;
        virtual DiskLoc _advance(const DiskLoc& thisLoc, int& keyOfs, int direction, const char *caller) = 0;
        virtual void _advanceTo(DiskLoc &thisLoc, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction ) = 0;

        /** set initial bucket */
        void initWithoutIndependentFieldRanges();

        /** if afterKey is true, we want the first key with values of the keyBegin fields greater than keyBegin */
        void advanceTo( const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive );

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
        auto_ptr< FieldRangeVectorIterator > _boundsIterator;
        shared_ptr< CoveredIndexMatcher > _matcher;
        shared_ptr<Projection::KeyOnly> _keyFieldsOnly;
        bool _independentFieldRanges;
        long long _nscanned;
    };

    /** Renames the index namespace for this btree's index. */
    void renameIndexNamespace(const char *oldNs, const char *newNs);

    /**
     * give us a writable version of the btree bucket (declares write intent).
     * note it is likely more efficient to declare write intent on something smaller when you can.
     */
    template< class V >
    BtreeBucket<V> * DiskLoc::btreemod() const {
        verify( _a != -1 );
        BtreeBucket<V> *b = const_cast< BtreeBucket<V> * >( btree<V>() );
        return static_cast< BtreeBucket<V>* >( getDur().writingPtr( b, V::BucketSize ) );
    }

    template< class V >
    BucketBasics<V>::KeyNode::KeyNode(const BucketBasics<V>& bb, const _KeyNode &k) :
        prevChildBucket(k.prevChildBucket),
        recordLoc(k.recordLoc), key(bb.data+k.keyDataOfs())
    { }

} // namespace mongo;
