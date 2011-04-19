// btree.cpp

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

#include "pch.h"
#include "db.h"
#include "btree.h"
#include "pdfile.h"
#include "json.h"
#include "clientcursor.h"
#include "client.h"
#include "dbhelpers.h"
#include "curop-inl.h"
#include "stats/counters.h"
#include "dur_commitjob.h"
#include "btreebuilder.h"

namespace mongo {

#define VERIFYTHISLOC dassert( thisLoc.btree() == this );

    /**
     * give us a writable version of the btree bucket (declares write intent).
     * note it is likely more efficient to declare write intent on something smaller when you can.
     */
    BtreeBucket* DiskLoc::btreemod() const {
        assert( _a != -1 );
        BtreeBucket *b = const_cast< BtreeBucket * >( btree() );
        return static_cast< BtreeBucket* >( getDur().writingPtr( b, BucketSize ) );
    }

    _KeyNode& _KeyNode::writing() const {
        return *getDur().writing( const_cast< _KeyNode* >( this ) );
    }

    KeyNode::KeyNode(const BucketBasics& bb, const _KeyNode &k) :
        prevChildBucket(k.prevChildBucket),
        recordLoc(k.recordLoc), key(bb.data+k.keyDataOfs())
    { }

    // largest key size we allow.  note we very much need to support bigger keys (somehow) in the future.
    static const int KeyMax = BucketSize / 10;

    // We define this value as the maximum number of bytes such that, if we have
    // fewer than this many bytes, we must be able to either merge with or receive
    // keys from any neighboring node.  If our utilization goes below this value we
    // know we can bring up the utilization with a simple operation.  Ignoring the
    // 90/10 split policy which is sometimes employed and our 'unused' nodes, this
    // is a lower bound on bucket utilization for non root buckets.
    //
    // Note that the exact value here depends on the implementation of
    // rebalancedSeparatorPos().  The conditions for lowWaterMark - 1 are as
    // follows:  We know we cannot merge with the neighbor, so the total data size
    // for us, the neighbor, and the separator must be at least
    // BtreeBucket::bodySize() + 1.  We must be able to accept one key of any
    // allowed size, so our size plus storage for that additional key must be
    // <= BtreeBucket::bodySize() / 2.  This way, with the extra key we'll have a
    // new bucket data size < half the total data size and by the implementation
    // of rebalancedSeparatorPos() the key must be added.
    static const int lowWaterMark = BtreeBucket::bodySize() / 2 - KeyMax - sizeof( _KeyNode ) + 1;

    static const int split_debug = 0;
    static const int insert_debug = 0;

    extern int otherTraceLevel;

    /**
     * this error is ok/benign when doing a background indexing -- that logic in pdfile checks explicitly
     * for the 10287 error code.
     */
    static void alreadyInIndex() {
        // we don't use massert() here as that does logging and this is 'benign' - see catches in _indexRecord()
        throw MsgAssertionException(10287, "btree: key+recloc already in index");
    }

    /* BucketBasics --------------------------------------------------- */

    void BucketBasics::assertWritable() {
        if( cmdLine.dur )
            dur::assertAlreadyDeclared(this, sizeof(*this));
    }

    string BtreeBucket::bucketSummary() const {
        stringstream ss;
        ss << "  Bucket info:" << endl;
        ss << "    n: " << n << endl;
        ss << "    parent: " << parent.toString() << endl;
        ss << "    nextChild: " << parent.toString() << endl;
        ss << "    flags:" << flags << endl;
        ss << "    emptySize: " << emptySize << " topSize: " << topSize << endl;
        return ss.str();
    }

    int BucketBasics::Size() const {
        assert( _wasSize == BucketSize );
        return BucketSize;
    }

    void BucketBasics::_shape(int level, stringstream& ss) const {
        for ( int i = 0; i < level; i++ ) ss << ' ';
        ss << "*\n";
        for ( int i = 0; i < n; i++ )
            if ( !k(i).prevChildBucket.isNull() )
                k(i).prevChildBucket.btree()->_shape(level+1,ss);
        if ( !nextChild.isNull() )
            nextChild.btree()->_shape(level+1,ss);
    }

    int bt_fv=0;
    int bt_dmp=0;

    void BtreeBucket::dumpTree(const DiskLoc &thisLoc, const BSONObj &order) const {
        bt_dmp=1;
        fullValidate(thisLoc, order);
        bt_dmp=0;
    }

    long long BtreeBucket::fullValidate(const DiskLoc& thisLoc, const BSONObj &order, long long *unusedCount, bool strict) const {
        {
            bool f = false;
            assert( f = true );
            massert( 10281 , "assert is misdefined", f);
        }

        killCurrentOp.checkForInterrupt();
        assertValid(order, true);

        if ( bt_dmp ) {
            out() << thisLoc.toString() << ' ';
            ((BtreeBucket *) this)->dump();
        }

        // keycount
        long long kc = 0;

        for ( int i = 0; i < n; i++ ) {
            const _KeyNode& kn = k(i);

            if ( kn.isUsed() ) {
                kc++;
            }
            else {
                if ( unusedCount ) {
                    ++( *unusedCount );
                }
            }
            if ( !kn.prevChildBucket.isNull() ) {
                DiskLoc left = kn.prevChildBucket;
                const BtreeBucket *b = left.btree();
                if ( strict ) {
                    assert( b->parent == thisLoc );
                }
                else {
                    wassert( b->parent == thisLoc );
                }
                kc += b->fullValidate(kn.prevChildBucket, order, unusedCount, strict);
            }
        }
        if ( !nextChild.isNull() ) {
            const BtreeBucket *b = nextChild.btree();
            if ( strict ) {
                assert( b->parent == thisLoc );
            }
            else {
                wassert( b->parent == thisLoc );
            }
            kc += b->fullValidate(nextChild, order, unusedCount, strict);
        }

        return kc;
    }

    int nDumped = 0;

    void BucketBasics::assertValid(const Ordering &order, bool force) const {
        if ( !debug && !force )
            return;
        wassert( n >= 0 && n < Size() );
        wassert( emptySize >= 0 && emptySize < BucketSize );
        wassert( topSize >= n && topSize <= BucketSize );

        // this is very slow so don't do often
        {
            static int _k;
            if( ++_k % 128 )
                return;
        }

        DEV {
            // slow:
            for ( int i = 0; i < n-1; i++ ) {
                Key k1 = keyNode(i).key;
                Key k2 = keyNode(i+1).key;
                int z = k1.woCompare(k2, order); //OK
                if ( z > 0 ) {
                    out() << "ERROR: btree key order corrupt.  Keys:" << endl;
                    if ( ++nDumped < 5 ) {
                        for ( int j = 0; j < n; j++ ) {
                            out() << "  " << keyNode(j).key.toString() << endl;
                        }
                        ((BtreeBucket *) this)->dump();
                    }
                    wassert(false);
                    break;
                }
                else if ( z == 0 ) {
                    if ( !(k(i).recordLoc < k(i+1).recordLoc) ) {
                        out() << "ERROR: btree key order corrupt (recordloc's wrong).  Keys:" << endl;
                        out() << " k(" << i << "):" << keyNode(i).key.toString() << " RL:" << k(i).recordLoc.toString() << endl;
                        out() << " k(" << i+1 << "):" << keyNode(i+1).key.toString() << " RL:" << k(i+1).recordLoc.toString() << endl;
                        wassert( k(i).recordLoc < k(i+1).recordLoc );
                    }
                }
            }
        }
        else {
            //faster:
            if ( n > 1 ) {
                Key k1 = keyNode(0).key;
                Key k2 = keyNode(n-1).key;
                int z = k1.woCompare(k2, order);
                //wassert( z <= 0 );
                if ( z > 0 ) {
                    problem() << "btree keys out of order" << '\n';
                    ONCE {
                        ((BtreeBucket *) this)->dump();
                    }
                    assert(false);
                }
            }
        }
    }

    inline void BucketBasics::markUnused(int keypos) {
        assert( keypos >= 0 && keypos < n );
        k(keypos).setUnused();
    }

    inline int BucketBasics::totalDataSize() const {
        return (int) (Size() - (data-(char*)this));
    }

    void BucketBasics::init() {
        parent.Null();
        nextChild.Null();
        _wasSize = BucketSize;
        _reserved1 = 0;
        flags = Packed;
        n = 0;
        emptySize = totalDataSize();
        topSize = 0;
        reserved = 0;
    }

    /** see _alloc */
    inline void BucketBasics::_unalloc(int bytes) {
        topSize -= bytes;
        emptySize += bytes;
    }

    /**
     * we allocate space from the end of the buffer for data.
     * the keynodes grow from the front.
     */
    inline int BucketBasics::_alloc(int bytes) {
        topSize += bytes;
        emptySize -= bytes;
        int ofs = totalDataSize() - topSize;
        assert( ofs > 0 );
        return ofs;
    }

    void BucketBasics::_delKeyAtPos(int keypos, bool mayEmpty) {
        // TODO This should be keypos < n
        assert( keypos >= 0 && keypos <= n );
        assert( childForPos(keypos).isNull() );
        // TODO audit cases where nextChild is null
        assert( ( mayEmpty && n > 0 ) || n > 1 || nextChild.isNull() );
        emptySize += sizeof(_KeyNode);
        n--;
        for ( int j = keypos; j < n; j++ )
            k(j) = k(j+1);
        setNotPacked();
    }

    /**
     * pull rightmost key from the bucket.  this version requires its right child to be null so it
     *  does not bother returning that value.
     */
    void BucketBasics::popBack(DiskLoc& recLoc, Key& key) {
        massert( 10282 ,  "n==0 in btree popBack()", n > 0 );
        assert( k(n-1).isUsed() ); // no unused skipping in this function at this point - btreebuilder doesn't require that
        KeyNode kn = keyNode(n-1);
        recLoc = kn.recordLoc;
        key = kn.key;
        int keysize = kn.key.dataSize();

        massert( 10283 , "rchild not null in btree popBack()", nextChild.isNull());

        // weirdly, we also put the rightmost down pointer in nextchild, even when bucket isn't full.
        nextChild = kn.prevChildBucket;

        n--;
        // This is risky because the key we are returning points to this unalloc'ed memory,
        // and we are assuming that the last key points to the last allocated
        // bson region.
        emptySize += sizeof(_KeyNode);
        _unalloc(keysize);
    }

    /** add a key.  must be > all existing.  be careful to set next ptr right. */
    bool BucketBasics::_pushBack(const DiskLoc recordLoc, const Key& key, const Ordering &order, const DiskLoc prevChild) {
        int bytesNeeded = key.dataSize() + sizeof(_KeyNode);
        if ( bytesNeeded > emptySize )
            return false;
        assert( bytesNeeded <= emptySize );
        if( !( n == 0 || keyNode(n-1).key.woCompare(key, order) <= 0 ) ) { 
            cout << keyNode(n-1).key.toString() << endl;
            cout << key.toString() << endl;
            cout << keyNode(n-1).key.woCompare(key, order) << endl;
            assert(false);
        }
        emptySize -= sizeof(_KeyNode);
        _KeyNode& kn = k(n++);
        kn.prevChildBucket = prevChild;
        kn.recordLoc = recordLoc;
        kn.setKeyDataOfs( (short) _alloc(key.dataSize()) );
        char *p = dataAt(kn.keyDataOfs());
        memcpy(p, key.data(), key.dataSize());
        return true;
    }

    /* durability note
       we do separate intent declarations herein.  arguably one could just declare
       the whole bucket given we do group commits. this is something we could investigate
       later as to what is faster under what situations.
       */
    /** insert a key in a bucket with no complexity -- no splits required
        @return false if a split is required.
    */
    bool BucketBasics::basicInsert(const DiskLoc thisLoc, int &keypos, const DiskLoc recordLoc, const Key& key, const Ordering &order) const {
        assert( keypos >= 0 && keypos <= n );
        int bytesNeeded = key.dataSize() + sizeof(_KeyNode);
        if ( bytesNeeded > emptySize ) {
            _pack(thisLoc, order, keypos);
            if ( bytesNeeded > emptySize )
                return false;
        }

        BucketBasics *b;
        {
            const char *p = (const char *) &k(keypos);
            const char *q = (const char *) &k(n+1);
            // declare that we will write to [k(keypos),k(n)]
            // todo: this writes a medium amount to the journal.  we may want to add a verb "shift" to the redo log so
            //       we can log a very small amount.
            b = (BucketBasics*) getDur().writingAtOffset((void *) this, p-(char*)this, q-p);

            // e.g. n==3, keypos==2
            // 1 4 9
            // ->
            // 1 4 _ 9
            for ( int j = n; j > keypos; j-- ) // make room
                b->k(j) = b->k(j-1);
        }

        getDur().declareWriteIntent(&b->emptySize, 12); // [b->emptySize..b->n] is 12 bytes and we are going to write those
        b->emptySize -= sizeof(_KeyNode);
        b->n++;

        // This _KeyNode was marked for writing above.
        _KeyNode& kn = b->k(keypos);
        kn.prevChildBucket.Null();
        kn.recordLoc = recordLoc;
        kn.setKeyDataOfs((short) b->_alloc(key.dataSize()) );
        char *p = b->dataAt(kn.keyDataOfs());
        getDur().declareWriteIntent(p, key.dataSize());
        memcpy(p, key.data(), key.dataSize());
        return true;
    }

    /**
     * With this implementation, refPos == 0 disregards effect of refPos.
     * index > 0 prevents creation of an empty bucket.
     */
    bool BucketBasics::mayDropKey( int index, int refPos ) const {
        return index > 0 && ( index != refPos ) && k( index ).isUnused() && k( index ).prevChildBucket.isNull();
    }

    int BucketBasics::packedDataSize( int refPos ) const {
        if ( flags & Packed ) {
            return BucketSize - emptySize - headerSize();
        }
        int size = 0;
        for( int j = 0; j < n; ++j ) {
            if ( mayDropKey( j, refPos ) ) {
                continue;
            }
            size += keyNode( j ).key.dataSize() + sizeof( _KeyNode );
        }
        return size;
    }

    /**
     * when we delete things we just leave empty space until the node is
     * full and then we repack it.
     */
    void BucketBasics::_pack(const DiskLoc thisLoc, const Ordering &order, int &refPos) const {
        if ( flags & Packed )
            return;

        VERIFYTHISLOC

        /** TODO perhaps this can be optimized.  for example if packing does no write, we can skip intent decl.
                 an empirical approach is probably best than just adding new code : perhaps the bucket would need
                 declaration anyway within the group commit interval, in which case we would just be adding
                 code and complexity without benefit.
        */
        thisLoc.btreemod()->_packReadyForMod(order, refPos);
    }

    /** version when write intent already declared */
    void BucketBasics::_packReadyForMod( const Ordering &order, int &refPos ) {
        assertWritable();

        if ( flags & Packed )
            return;

        int tdz = totalDataSize();
        char temp[BucketSize];
        int ofs = tdz;
        topSize = 0;
        int i = 0;
        for ( int j = 0; j < n; j++ ) {
            if( mayDropKey( j, refPos ) ) {
                continue; // key is unused and has no children - drop it
            }
            if( i != j ) {
                if ( refPos == j ) {
                    refPos = i; // i < j so j will never be refPos again
                }
                k( i ) = k( j );
            }
            short ofsold = k(i).keyDataOfs();
            int sz = keyNode(i).key.dataSize();
            ofs -= sz;
            topSize += sz;
            memcpy(temp+ofs, dataAt(ofsold), sz);
            k(i).setKeyDataOfsSavingUse( ofs );
            ++i;
        }
        if ( refPos == n ) {
            refPos = i;
        }
        n = i;
        int dataUsed = tdz - ofs;
        memcpy(data + ofs, temp + ofs, dataUsed);

        // assertWritable();
        // TEMP TEST getDur().declareWriteIntent(this, sizeof(*this));

        emptySize = tdz - dataUsed - n * sizeof(_KeyNode);
        assert( emptySize >= 0 );

        setPacked();

        assertValid( order );
    }

    inline void BucketBasics::truncateTo(int N, const Ordering &order, int &refPos) {
        dbMutex.assertWriteLocked();
        assertWritable();

        n = N;
        setNotPacked();
        _packReadyForMod( order, refPos );
    }

    /**
     * In the standard btree algorithm, we would split based on the
     * existing keys _and_ the new key.  But that's more work to
     * implement, so we split the existing keys and then add the new key.
     *
     * There are several published heuristic algorithms for doing splits,
     * but basically what you want are (1) even balancing between the two
     * sides and (2) a small split key so the parent can have a larger
     * branching factor.
     *
     * We just have a simple algorithm right now: if a key includes the
     * halfway point (or 10% way point) in terms of bytes, split on that key;
     * otherwise split on the key immediately to the left of the halfway
     * point (or 10% point).
     *
     * This function is expected to be called on a packed bucket.
     */
    int BucketBasics::splitPos( int keypos ) const {
        assert( n > 2 );
        int split = 0;
        int rightSize = 0;
        // when splitting a btree node, if the new key is greater than all the other keys, we should not do an even split, but a 90/10 split.
        // see SERVER-983
        // TODO I think we only want to do the 90% split on the rhs node of the tree.
        int rightSizeLimit = ( topSize + sizeof( _KeyNode ) * n ) / ( keypos == n ? 10 : 2 );
        for( int i = n - 1; i > -1; --i ) {
            rightSize += keyNode( i ).key.dataSize() + sizeof( _KeyNode );
            if ( rightSize > rightSizeLimit ) {
                split = i;
                break;
            }
        }
        // safeguards - we must not create an empty bucket
        if ( split < 1 ) {
            split = 1;
        }
        else if ( split > n - 2 ) {
            split = n - 2;
        }

        return split;
    }

    void BucketBasics::reserveKeysFront( int nAdd ) {
        assert( emptySize >= int( sizeof( _KeyNode ) * nAdd ) );
        emptySize -= sizeof( _KeyNode ) * nAdd;
        for( int i = n - 1; i > -1; --i ) {
            k( i + nAdd ) = k( i );
        }
        n += nAdd;
    }

    void BucketBasics::setKey( int i, const DiskLoc recordLoc, const Key &key, const DiskLoc prevChildBucket ) {
        _KeyNode &kn = k( i );
        kn.recordLoc = recordLoc;
        kn.prevChildBucket = prevChildBucket;
        short ofs = (short) _alloc( key.dataSize() );
        kn.setKeyDataOfs( ofs );
        char *p = dataAt( ofs );
        memcpy( p, key.data(), key.dataSize() );
    }

    void BucketBasics::dropFront( int nDrop, const Ordering &order, int &refpos ) {
        for( int i = nDrop; i < n; ++i ) {
            k( i - nDrop ) = k( i );
        }
        n -= nDrop;
        setNotPacked();
        _packReadyForMod( order, refpos );
    }

    /* - BtreeBucket --------------------------------------------------- */

    /** @return largest key in the subtree. */
    void BtreeBucket::findLargestKey(const DiskLoc& thisLoc, DiskLoc& largestLoc, int& largestKey) {
        DiskLoc loc = thisLoc;
        while ( 1 ) {
            const BtreeBucket *b = loc.btree();
            if ( !b->nextChild.isNull() ) {
                loc = b->nextChild;
                continue;
            }

            assert(b->n>0);
            largestLoc = loc;
            largestKey = b->n-1;

            break;
        }
    }

    /**
     * NOTE Currently the Ordering implementation assumes a compound index will
     * not have more keys than an unsigned variable has bits.  The same
     * assumption is used in the implementation below with respect to the 'mask'
     * variable.
     *
     * @param l a regular bsonobj
     * @param rBegin composed partly of an existing bsonobj, and the remaining keys are taken from a vector of elements that frequently changes 
     *
     * see 
     *  jstests/index_check6.js
     *  https://jira.mongodb.org/browse/SERVER-371
     */
    int BtreeBucket::customBSONCmp( const BSONObj &l, const BSONObj &rBegin, int rBeginLen, bool rSup, const vector< const BSONElement * > &rEnd, const vector< bool > &rEndInclusive, const Ordering &o, int direction ) {
        BSONObjIterator ll( l );
        BSONObjIterator rr( rBegin );
        vector< const BSONElement * >::const_iterator rr2 = rEnd.begin();
        vector< bool >::const_iterator inc = rEndInclusive.begin();
        unsigned mask = 1;
        for( int i = 0; i < rBeginLen; ++i, mask <<= 1 ) {
            BSONElement lll = ll.next();
            BSONElement rrr = rr.next();
            ++rr2;
            ++inc;

            int x = lll.woCompare( rrr, false );
            if ( o.descending( mask ) )
                x = -x;
            if ( x != 0 )
                return x;
        }
        if ( rSup ) {
            return -direction;
        }
        for( ; ll.more(); mask <<= 1 ) {
            BSONElement lll = ll.next();
            BSONElement rrr = **rr2;
            ++rr2;
            int x = lll.woCompare( rrr, false );
            if ( o.descending( mask ) )
                x = -x;
            if ( x != 0 )
                return x;
            if ( !*inc ) {
                return -direction;
            }
            ++inc;
        }
        return 0;
    }

        bool BtreeBucket::exists(const IndexDetails& idx, const DiskLoc &thisLoc, const Key& key, const Ordering& order) const {
            int pos;
            bool found;
            DiskLoc b = locate(idx, thisLoc, key, order, pos, found, minDiskLoc);

            // skip unused keys
            while ( 1 ) {
                if( b.isNull() )
                    break;
                const BtreeBucket *bucket = b.btree();
                const _KeyNode& kn = bucket->k(pos);
                if ( kn.isUsed() )
                    return bucket->keyAt(pos).woEqual(key);
            b = bucket->advance(b, pos, 1, "BtreeBucket::exists");
        }
        return false;
    }

    bool BtreeBucket::wouldCreateDup(
        const IndexDetails& idx, const DiskLoc &thisLoc,
        const Key& key, const Ordering& order,
        const DiskLoc &self) const {
        int pos;
        bool found;
        DiskLoc b = locate(idx, thisLoc, key, order, pos, found, minDiskLoc);

        while ( !b.isNull() ) {
            // we skip unused keys
            const BtreeBucket *bucket = b.btree();
            const _KeyNode& kn = bucket->k(pos);
            if ( kn.isUsed() ) {
                if( bucket->keyAt(pos).woEqual(key) )
                    return kn.recordLoc != self;
                break;
            }
            b = bucket->advance(b, pos, 1, "BtreeBucket::dupCheck");
        }

        return false;
    }

    string BtreeBucket::dupKeyError( const IndexDetails& idx , const Key& key ) {
        stringstream ss;
        ss << "E11000 duplicate key error ";
        ss << "index: " << idx.indexNamespace() << "  ";
        ss << "dup key: " << key.toString();
        return ss.str();
    }

    /**
     * Find a key withing this btree bucket.
     *
     * When duplicate keys are allowed, we use the DiskLoc of the record as if it were part of the
     * key.  That assures that even when there are many duplicates (e.g., 1 million) for a key,
     * our performance is still good.
     *
     * assertIfDup: if the key exists (ignoring the recordLoc), uassert
     *
     * pos: for existing keys k0...kn-1.
     * returns # it goes BEFORE.  so key[pos-1] < key < key[pos]
     * returns n if it goes after the last existing key.
     * note result might be an Unused location!
     */
    bool BtreeBucket::find(const IndexDetails& idx, const Key& key, const DiskLoc &recordLoc, const Ordering &order, int& pos, bool assertIfDup) const {
        globalIndexCounters.btree( (char*)this );

        // binary search for this key
        bool dupsChecked = false;
        int l=0;
        int h=n-1;
        while ( l <= h ) {
            int m = (l+h)/2;
            KeyNode M = keyNode(m);
            int x = key.woCompare(M.key, order);
            if ( x == 0 ) {
                if( assertIfDup ) {
                    if( k(m).isUnused() ) {
                        // ok that key is there if unused.  but we need to check that there aren't other
                        // entries for the key then.  as it is very rare that we get here, we don't put any
                        // coding effort in here to make this particularly fast
                        if( !dupsChecked ) {
                            dupsChecked = true;
                            if( idx.head.btree()->exists(idx, idx.head, key, order) ) {
                                if( idx.head.btree()->wouldCreateDup(idx, idx.head, key, order, recordLoc) )
                                    uasserted( ASSERT_ID_DUPKEY , dupKeyError( idx , key ) );
                                else
                                    alreadyInIndex();
                            }
                        }
                    }
                    else {
                        if( M.recordLoc == recordLoc )
                            alreadyInIndex();
                        uasserted( ASSERT_ID_DUPKEY , dupKeyError( idx , key ) );
                    }
                }

                // dup keys allowed.  use recordLoc as if it is part of the key
                DiskLoc unusedRL = M.recordLoc;
                unusedRL.GETOFS() &= ~1; // so we can test equality without the used bit messing us up
                x = recordLoc.compare(unusedRL);
            }
            if ( x < 0 ) // key < M.key
                h = m-1;
            else if ( x > 0 )
                l = m+1;
            else {
                // found it.
                pos = m;
                return true;
            }
        }
        // not found
        pos = l;
        if ( pos != n ) {
            Key keyatpos = keyNode(pos).key;
            wassert( key.woCompare(keyatpos, order) <= 0 );
            if ( pos > 0 ) {
                wassert( keyNode(pos-1).key.woCompare(key, order) <= 0 );
            }
        }

        return false;
    }

    void BtreeBucket::delBucket(const DiskLoc thisLoc, const IndexDetails& id) {
        ClientCursor::informAboutToDeleteBucket(thisLoc); // slow...
        assert( !isHead() );

        const BtreeBucket *p = parent.btree();
        int parentIdx = indexInParent( thisLoc );
        p->childForPos( parentIdx ).writing().Null();
        deallocBucket( thisLoc, id );
    }

    void BtreeBucket::deallocBucket(const DiskLoc thisLoc, const IndexDetails &id) {
#if 0
        // as a temporary defensive measure, we zap the whole bucket, AND don't truly delete
        // it (meaning it is ineligible for reuse).
        memset(this, 0, Size());
#else
        // defensive:
        n = -1;
        parent.Null();
        string ns = id.indexNamespace();
        theDataFileMgr._deleteRecord(nsdetails(ns.c_str()), ns.c_str(), thisLoc.rec(), thisLoc);
#endif
    }

    /** note: may delete the entire bucket!  this invalid upon return sometimes. */
    void BtreeBucket::delKeyAtPos( const DiskLoc thisLoc, IndexDetails& id, int p, const Ordering &order) {
        assert(n>0);
        DiskLoc left = childForPos(p);

        if ( n == 1 ) {
            if ( left.isNull() && nextChild.isNull() ) {
                _delKeyAtPos(p);
                if ( isHead() ) {
                    // we don't delete the top bucket ever
                }
                else {
                    if ( !mayBalanceWithNeighbors( thisLoc, id, order ) ) {
                        // An empty bucket is only allowed as a transient state.  If
                        // there are no neighbors to balance with, we delete ourself.
                        // This condition is only expected in legacy btrees.
                        delBucket(thisLoc, id);
                    }
                }
                return;
            }
            deleteInternalKey( thisLoc, p, id, order );
            return;
        }

        if ( left.isNull() ) {
            _delKeyAtPos(p);
            mayBalanceWithNeighbors( thisLoc, id, order );
        }
        else {
            deleteInternalKey( thisLoc, p, id, order );
        }
    }

    /**
     * This function replaces the specified key (k) by either the prev or next
     * key in the btree (k').  We require that k have either a left or right
     * child.  If k has a left child, we set k' to the prev key of k, which must
     * be a leaf present in the left child.  If k does not have a left child, we
     * set k' to the next key of k, which must be a leaf present in the right
     * child.  When we replace k with k', we copy k' over k (which may cause a
     * split) and then remove k' from its original location.  Because k' is
     * stored in a descendent of k, replacing k by k' will not modify the
     * storage location of the original k', and we can easily remove k' from
     * its original location.
     *
     * This function is only needed in cases where k has a left or right child;
     * in other cases a simpler key removal implementation is possible.
     *
     * NOTE on legacy btree structures:
     * In legacy btrees, k' can be a nonleaf.  In such a case we 'delete' k by
     * marking it as an unused node rather than replacing it with k'.  Also, k'
     * may be a leaf but marked as an unused node.  In such a case we replace
     * k by k', preserving the key's unused marking.  This function is only
     * expected to mark a key as unused when handling a legacy btree.
     */
    void BtreeBucket::deleteInternalKey( const DiskLoc thisLoc, int keypos, IndexDetails &id, const Ordering &order ) {
        DiskLoc lchild = childForPos( keypos );
        DiskLoc rchild = childForPos( keypos + 1 );
        assert( !lchild.isNull() || !rchild.isNull() );
        int advanceDirection = lchild.isNull() ? 1 : -1;
        int advanceKeyOfs = keypos;
        DiskLoc advanceLoc = advance( thisLoc, advanceKeyOfs, advanceDirection, __FUNCTION__ );
        // advanceLoc must be a descentant of thisLoc, because thisLoc has a
        // child in the proper direction and all descendants of thisLoc must be
        // nonempty because they are not the root.
         
        if ( !advanceLoc.btree()->childForPos( advanceKeyOfs ).isNull() ||
                !advanceLoc.btree()->childForPos( advanceKeyOfs + 1 ).isNull() ) {
            // only expected with legacy btrees, see note above
            markUnused( keypos );
            return;
        }

        KeyNode kn = advanceLoc.btree()->keyNode( advanceKeyOfs );
        // Because advanceLoc is a descendant of thisLoc, updating thisLoc will
        // not affect packing or keys of advanceLoc and kn will be stable
        // during the following setInternalKey()
        setInternalKey( thisLoc, keypos, kn.recordLoc, kn.key, order, childForPos( keypos ), childForPos( keypos + 1 ), id );
        advanceLoc.btreemod()->delKeyAtPos( advanceLoc, id, advanceKeyOfs, order );
    }

    void BtreeBucket::replaceWithNextChild( const DiskLoc thisLoc, IndexDetails &id ) {
        assert( n == 0 && !nextChild.isNull() );
        if ( parent.isNull() ) {
            assert( id.head == thisLoc );
            id.head.writing() = nextChild;
        }
        else {
            parent.btree()->childForPos( indexInParent( thisLoc ) ).writing() = nextChild;
        }
        nextChild.btree()->parent.writing() = parent;
        ClientCursor::informAboutToDeleteBucket( thisLoc );
        deallocBucket( thisLoc, id );
    }

    bool BtreeBucket::canMergeChildren( const DiskLoc &thisLoc, int leftIndex ) const {
        assert( leftIndex >= 0 && leftIndex < n );
        DiskLoc leftNodeLoc = childForPos( leftIndex );
        DiskLoc rightNodeLoc = childForPos( leftIndex + 1 );
        if ( leftNodeLoc.isNull() || rightNodeLoc.isNull() ) {
            // TODO if this situation is possible in long term implementation, maybe we should compact somehow anyway
            return false;
        }
        int pos = 0;
        {
            const BtreeBucket *l = leftNodeLoc.btree();
            const BtreeBucket *r = rightNodeLoc.btree();
            if ( ( headerSize() + l->packedDataSize( pos ) + r->packedDataSize( pos ) + keyNode( leftIndex ).key.dataSize() + sizeof(_KeyNode) > unsigned( BucketSize ) ) ) {
                return false;
            }
        }
        return true;
    }

    /**
     * This implementation must respect the meaning and value of lowWaterMark.
     * Also see comments in splitPos().
     */
    int BtreeBucket::rebalancedSeparatorPos( const DiskLoc &thisLoc, int leftIndex ) const {
        int split = -1;
        int rightSize = 0;
        const BtreeBucket *l = childForPos( leftIndex ).btree();
        const BtreeBucket *r = childForPos( leftIndex + 1 ).btree();

        int KNS = sizeof( _KeyNode );
        int rightSizeLimit = ( l->topSize + l->n * KNS + keyNode( leftIndex ).key.dataSize() + KNS + r->topSize + r->n * KNS ) / 2;
        // This constraint should be ensured by only calling this function
        // if we go below the low water mark.
        assert( rightSizeLimit < BtreeBucket::bodySize() );
        for( int i = r->n - 1; i > -1; --i ) {
            rightSize += r->keyNode( i ).key.dataSize() + KNS;
            if ( rightSize > rightSizeLimit ) {
                split = l->n + 1 + i;
                break;
            }
        }
        if ( split == -1 ) {
            rightSize += keyNode( leftIndex ).key.dataSize() + KNS;
            if ( rightSize > rightSizeLimit ) {
                split = l->n;
            }
        }
        if ( split == -1 ) {
            for( int i = l->n - 1; i > -1; --i ) {
                rightSize += l->keyNode( i ).key.dataSize() + KNS;
                if ( rightSize > rightSizeLimit ) {
                    split = i;
                    break;
                }
            }
        }
        // safeguards - we must not create an empty bucket
        if ( split < 1 ) {
            split = 1;
        }
        else if ( split > l->n + 1 + r->n - 2 ) {
            split = l->n + 1 + r->n - 2;
        }

        return split;
    }

    void BtreeBucket::doMergeChildren( const DiskLoc thisLoc, int leftIndex, IndexDetails &id, const Ordering &order ) {
        DiskLoc leftNodeLoc = childForPos( leftIndex );
        DiskLoc rightNodeLoc = childForPos( leftIndex + 1 );
        BtreeBucket *l = leftNodeLoc.btreemod();
        BtreeBucket *r = rightNodeLoc.btreemod();
        int pos = 0;
        l->_packReadyForMod( order, pos );
        r->_packReadyForMod( order, pos ); // pack r in case there are droppable keys

        // We know the additional keys below will fit in l because canMergeChildren()
        // must be true.
        int oldLNum = l->n;
        {
            KeyNode kn = keyNode( leftIndex );
            l->pushBack( kn.recordLoc, kn.key, order, l->nextChild ); // left child's right child becomes old parent key's left child
        }
        for( int i = 0; i < r->n; ++i ) {
            KeyNode kn = r->keyNode( i );
            l->pushBack( kn.recordLoc, kn.key, order, kn.prevChildBucket );
        }
        l->nextChild = r->nextChild;
        l->fixParentPtrs( leftNodeLoc, oldLNum );
        r->delBucket( rightNodeLoc, id );
        childForPos( leftIndex + 1 ) = leftNodeLoc;
        childForPos( leftIndex ) = DiskLoc();
        _delKeyAtPos( leftIndex, true );
        if ( n == 0 ) {
            // will trash this and thisLoc
            // TODO To ensure all leaves are of equal height, we should ensure
            // this is only called on the root.
            replaceWithNextChild( thisLoc, id );
        }
        else {
            // balance recursively - maybe we should do this even when n == 0?
            mayBalanceWithNeighbors( thisLoc, id, order );
        }
    }

    int BtreeBucket::indexInParent( const DiskLoc &thisLoc ) const {
        assert( !parent.isNull() );
        const BtreeBucket *p = parent.btree();
        if ( p->nextChild == thisLoc ) {
            return p->n;
        }
        else {
            for( int i = 0; i < p->n; ++i ) {
                if ( p->k( i ).prevChildBucket == thisLoc ) {
                    return i;
                }
            }
        }
        out() << "ERROR: can't find ref to child bucket.\n";
        out() << "child: " << thisLoc << "\n";
        dump();
        out() << "Parent: " << parent << "\n";
        p->dump();
        assert(false);
        return -1; // just to compile
    }

    bool BtreeBucket::tryBalanceChildren( const DiskLoc thisLoc, int leftIndex, IndexDetails &id, const Ordering &order ) const {
        // If we can merge, then we must merge rather than balance to preserve
        // bucket utilization constraints.
        if ( canMergeChildren( thisLoc, leftIndex ) ) {
            return false;
        }
        thisLoc.btreemod()->doBalanceChildren( thisLoc, leftIndex, id, order );
        return true;
    }

    void BtreeBucket::doBalanceLeftToRight( const DiskLoc thisLoc, int leftIndex, int split,
                                            BtreeBucket *l, const DiskLoc lchild,
                                            BtreeBucket *r, const DiskLoc rchild,
                                            IndexDetails &id, const Ordering &order ) {
        // TODO maybe do some audits the same way pushBack() does?
        // As a precondition, rchild + the old separator are <= half a body size,
        // and lchild is at most completely full.  Based on the value of split,
        // rchild will get <= half of the total bytes which is at most 75%
        // of a full body.  So rchild will have room for the following keys:
        int rAdd = l->n - split;
        r->reserveKeysFront( rAdd );
        for( int i = split + 1, j = 0; i < l->n; ++i, ++j ) {
            KeyNode kn = l->keyNode( i );
            r->setKey( j, kn.recordLoc, kn.key, kn.prevChildBucket );
        }
        {
            KeyNode kn = keyNode( leftIndex );
            r->setKey( rAdd - 1, kn.recordLoc, kn.key, l->nextChild ); // left child's right child becomes old parent key's left child
        }
        r->fixParentPtrs( rchild, 0, rAdd - 1 );
        {
            KeyNode kn = l->keyNode( split );
            l->nextChild = kn.prevChildBucket;
            // Because lchild is a descendant of thisLoc, updating thisLoc will
            // not not affect packing or keys of lchild and kn will be stable
            // during the following setInternalKey()            
            setInternalKey( thisLoc, leftIndex, kn.recordLoc, kn.key, order, lchild, rchild, id );
        }
        int zeropos = 0;
        // lchild and rchild cannot be merged, so there must be >0 (actually more)
        // keys to the left of split.
        l->truncateTo( split, order, zeropos );
    }

    void BtreeBucket::doBalanceRightToLeft( const DiskLoc thisLoc, int leftIndex, int split,
                                            BtreeBucket *l, const DiskLoc lchild,
                                            BtreeBucket *r, const DiskLoc rchild,
                                            IndexDetails &id, const Ordering &order ) {
        // As a precondition, lchild + the old separator are <= half a body size,
        // and rchild is at most completely full.  Based on the value of split,
        // lchild will get less than half of the total bytes which is at most 75%
        // of a full body.  So lchild will have room for the following keys:
        int lN = l->n;
        {
            KeyNode kn = keyNode( leftIndex );
            l->pushBack( kn.recordLoc, kn.key, order, l->nextChild ); // left child's right child becomes old parent key's left child
        }
        for( int i = 0; i < split - lN - 1; ++i ) {
            KeyNode kn = r->keyNode( i );
            l->pushBack( kn.recordLoc, kn.key, order, kn.prevChildBucket );
        }
        {
            KeyNode kn = r->keyNode( split - lN - 1 );
            l->nextChild = kn.prevChildBucket;
            // Child lN was lchild's old nextChild, and don't need to fix that one.
            l->fixParentPtrs( lchild, lN + 1, l->n );
            // Because rchild is a descendant of thisLoc, updating thisLoc will
            // not affect packing or keys of rchild and kn will be stable
            // during the following setInternalKey()            
            setInternalKey( thisLoc, leftIndex, kn.recordLoc, kn.key, order, lchild, rchild, id );
        }
        int zeropos = 0;
        // lchild and rchild cannot be merged, so there must be >0 (actually more)
        // keys to the right of split.
        r->dropFront( split - lN, order, zeropos );
    }

    void BtreeBucket::doBalanceChildren( const DiskLoc thisLoc, int leftIndex, IndexDetails &id, const Ordering &order ) {
        DiskLoc lchild = childForPos( leftIndex );
        DiskLoc rchild = childForPos( leftIndex + 1 );
        int zeropos = 0;
        BtreeBucket *l = lchild.btreemod();
        l->_packReadyForMod( order, zeropos );
        BtreeBucket *r = rchild.btreemod();
        r->_packReadyForMod( order, zeropos );
        int split = rebalancedSeparatorPos( thisLoc, leftIndex );

        // By definition, if we are below the low water mark and cannot merge
        // then we must actively balance.
        assert( split != l->n );
        if ( split < l->n ) {
            doBalanceLeftToRight( thisLoc, leftIndex, split, l, lchild, r, rchild, id, order );
        }
        else {
            doBalanceRightToLeft( thisLoc, leftIndex, split, l, lchild, r, rchild, id, order );
        }
    }

    bool BtreeBucket::mayBalanceWithNeighbors( const DiskLoc thisLoc, IndexDetails &id, const Ordering &order ) const {
        if ( parent.isNull() ) { // we are root, there are no neighbors
            return false;
        }

        if ( packedDataSize( 0 ) >= lowWaterMark ) {
            return false;
        }

        const BtreeBucket *p = parent.btree();
        int parentIdx = indexInParent( thisLoc );

        // TODO will missing neighbor case be possible long term?  Should we try to merge/balance somehow in that case if so?
        bool mayBalanceRight = ( ( parentIdx < p->n ) && !p->childForPos( parentIdx + 1 ).isNull() );
        bool mayBalanceLeft = ( ( parentIdx > 0 ) && !p->childForPos( parentIdx - 1 ).isNull() );

        // Balance if possible on one side - we merge only if absolutely necessary
        // to preserve btree bucket utilization constraints since that's a more
        // heavy duty operation (especially if we must re-split later).
        if ( mayBalanceRight &&
                p->tryBalanceChildren( parent, parentIdx, id, order ) ) {
            return true;
        }
        if ( mayBalanceLeft &&
                p->tryBalanceChildren( parent, parentIdx - 1, id, order ) ) {
            return true;
        }

        BtreeBucket *pm = parent.btreemod();
        if ( mayBalanceRight ) {
            pm->doMergeChildren( parent, parentIdx, id, order );
            return true;
        }
        else if ( mayBalanceLeft ) {
            pm->doMergeChildren( parent, parentIdx - 1, id, order );
            return true;
        }

        return false;
    }

    /** remove a key from the index */
    bool BtreeBucket::unindex(const DiskLoc thisLoc, IndexDetails& id, const BSONObj& key, const DiskLoc recordLoc ) const {
        int pos;
        bool found;
        DiskLoc loc = locate(id, thisLoc, key, Ordering::make(id.keyPattern()), pos, found, recordLoc, 1);
        if ( found ) {

            if ( key.objsize() > KeyMax ) {
                OCCASIONALLY problem() << "unindex: key too large to index but was found for " << id.indexNamespace() << " reIndex suggested" << endl;
            }
            
            loc.btreemod()->delKeyAtPos(loc, id, pos, Ordering::make(id.keyPattern()));
            
            return true;
        }
        return false;
    }

    BtreeBucket* BtreeBucket::allocTemp() {
        BtreeBucket *b = (BtreeBucket*) malloc(BucketSize);
        b->init();
        return b;
    }

    inline void BtreeBucket::fix(const DiskLoc thisLoc, const DiskLoc child) {
        if ( !child.isNull() ) {
            if ( insert_debug )
                out() << "      " << child.toString() << ".parent=" << thisLoc.toString() << endl;
            child.btree()->parent.writing() = thisLoc;
        }
    }

    /**
     * This can cause a lot of additional page writes when we assign buckets to
     * different parents.  Maybe get rid of parent ptrs?
     */
    void BtreeBucket::fixParentPtrs(const DiskLoc thisLoc, int firstIndex, int lastIndex) const {
        VERIFYTHISLOC
        if ( lastIndex == -1 ) {
            lastIndex = n;
        }
        for ( int i = firstIndex; i <= lastIndex; i++ ) {
            fix(thisLoc, childForPos(i));
        }
    }

    void BtreeBucket::setInternalKey( const DiskLoc thisLoc, int keypos,
                                      const DiskLoc recordLoc, const Key &key, const Ordering &order,
                                      const DiskLoc lchild, const DiskLoc rchild, IndexDetails &idx ) {
        childForPos( keypos ).Null();

        // This may leave the bucket empty (n == 0) which is ok only as a
        // transient state.  In the instant case, the implementation of
        // insertHere behaves correctly when n == 0 and as a side effect
        // increments n.
        _delKeyAtPos( keypos, true );

        // Ensure we do not orphan neighbor's old child.
        assert( childForPos( keypos ) == rchild );

        // Just set temporarily - required to pass validation in insertHere()
        childForPos( keypos ) = lchild;

        insertHere( thisLoc, keypos, recordLoc, key, order, lchild, rchild, idx );
    }

    /**
     * insert a key in this bucket, splitting if necessary.
     * @keypos - where to insert the key in range 0..n.  0=make leftmost, n=make rightmost.
     * NOTE this function may free some data, and as a result the value passed for keypos may
     * be invalid after calling insertHere()
     *
     * Some of the write intent signaling below relies on the implementation of
     * the optimized write intent code in basicInsert().
     */
    void BtreeBucket::insertHere( const DiskLoc thisLoc, int keypos,
                                  const DiskLoc recordLoc, const Key& key, const Ordering& order,
                                  const DiskLoc lchild, const DiskLoc rchild, IndexDetails& idx) const {
        if ( insert_debug )
            out() << "   " << thisLoc.toString() << ".insertHere " << key.toString() << '/' << recordLoc.toString() << ' '
                  << lchild.toString() << ' ' << rchild.toString() << " keypos:" << keypos << endl;

        if ( !basicInsert(thisLoc, keypos, recordLoc, key, order) ) {
            // If basicInsert() fails, the bucket will be packed as required by split().
            thisLoc.btreemod()->split(thisLoc, keypos, recordLoc, key, order, lchild, rchild, idx);
            return;
        }

        {
            const _KeyNode *_kn = &k(keypos);
            _KeyNode *kn = (_KeyNode *) getDur().alreadyDeclared((_KeyNode*) _kn); // already declared intent in basicInsert()
            if ( keypos+1 == n ) { // last key
                if ( nextChild != lchild ) {
                    out() << "ERROR nextChild != lchild" << endl;
                    out() << "  thisLoc: " << thisLoc.toString() << ' ' << idx.indexNamespace() << endl;
                    out() << "  keyPos: " << keypos << " n:" << n << endl;
                    out() << "  nextChild: " << nextChild.toString() << " lchild: " << lchild.toString() << endl;
                    out() << "  recordLoc: " << recordLoc.toString() << " rchild: " << rchild.toString() << endl;
                    out() << "  key: " << key.toString() << endl;
                    dump();
                    assert(false);
                }
                kn->prevChildBucket = nextChild;
                assert( kn->prevChildBucket == lchild );
                nextChild.writing() = rchild;
                if ( !rchild.isNull() )
                    rchild.btree()->parent.writing() = thisLoc;
            }
            else {
                kn->prevChildBucket = lchild;
                if ( k(keypos+1).prevChildBucket != lchild ) {
                    out() << "ERROR k(keypos+1).prevChildBucket != lchild" << endl;
                    out() << "  thisLoc: " << thisLoc.toString() << ' ' << idx.indexNamespace() << endl;
                    out() << "  keyPos: " << keypos << " n:" << n << endl;
                    out() << "  k(keypos+1).pcb: " << k(keypos+1).prevChildBucket.toString() << " lchild: " << lchild.toString() << endl;
                    out() << "  recordLoc: " << recordLoc.toString() << " rchild: " << rchild.toString() << endl;
                    out() << "  key: " << key.toString() << endl;
                    dump();
                    assert(false);
                }
                const DiskLoc *pc = &k(keypos+1).prevChildBucket;
                *getDur().alreadyDeclared((DiskLoc*) pc) = rchild; // declared in basicInsert()
                if ( !rchild.isNull() )
                    rchild.btree()->parent.writing() = thisLoc;
            }
            return;
        }
    }

    void BtreeBucket::split(const DiskLoc thisLoc, int keypos, const DiskLoc recordLoc, const Key& key, const Ordering& order, const DiskLoc lchild, const DiskLoc rchild, IndexDetails& idx) {
        assertWritable();

        if ( split_debug )
            out() << "    " << thisLoc.toString() << ".split" << endl;

        int split = splitPos( keypos );
        DiskLoc rLoc = addBucket(idx);
        BtreeBucket *r = rLoc.btreemod();
        if ( split_debug )
            out() << "     split:" << split << ' ' << keyNode(split).key.toString() << " n:" << n << endl;
        for ( int i = split+1; i < n; i++ ) {
            KeyNode kn = keyNode(i);
            r->pushBack(kn.recordLoc, kn.key, order, kn.prevChildBucket);
        }
        r->nextChild = nextChild;
        r->assertValid( order );

        if ( split_debug )
            out() << "     new rLoc:" << rLoc.toString() << endl;
        r = 0;
        rLoc.btree()->fixParentPtrs(rLoc);

        {
            KeyNode splitkey = keyNode(split);
            nextChild = splitkey.prevChildBucket; // splitkey key gets promoted, its children will be thisLoc (l) and rLoc (r)
            if ( split_debug ) {
                out() << "    splitkey key:" << splitkey.key.toString() << endl;
            }

            // Because thisLoc is a descendant of parent, updating parent will
            // not not affect packing or keys of thisLoc and splitkey will be stable
            // during the following:
            
            // promote splitkey to a parent node
            if ( parent.isNull() ) {
                // make a new parent if we were the root
                DiskLoc L = addBucket(idx);
                BtreeBucket *p = L.btreemod();
                p->pushBack(splitkey.recordLoc, splitkey.key, order, thisLoc);
                p->nextChild = rLoc;
                p->assertValid( order );
                parent = idx.head.writing() = L;
                if ( split_debug )
                    out() << "    we were root, making new root:" << hex << parent.getOfs() << dec << endl;
                rLoc.btree()->parent.writing() = parent;
            }
            else {
                // set this before calling _insert - if it splits it will do fixParent() logic and change the value.
                rLoc.btree()->parent.writing() = parent;
                if ( split_debug )
                    out() << "    promoting splitkey key " << splitkey.key.toString() << endl;
                parent.btree()->_insert(parent, splitkey.recordLoc, splitkey.key, order, /*dupsallowed*/true, thisLoc, rLoc, idx);
            }
        }

        int newpos = keypos;
        // note this may trash splitkey.key.  thus we had to promote it before finishing up here.
        truncateTo(split, order, newpos);

        // add our new key, there is room now
        {
            if ( keypos <= split ) {
                if ( split_debug )
                    out() << "  keypos<split, insertHere() the new key" << endl;
                insertHere(thisLoc, newpos, recordLoc, key, order, lchild, rchild, idx);
            }
            else {
                int kp = keypos-split-1;
                assert(kp>=0);
                rLoc.btree()->insertHere(rLoc, kp, recordLoc, key, order, lchild, rchild, idx);
            }
        }

        if ( split_debug )
            out() << "     split end " << hex << thisLoc.getOfs() << dec << endl;
    }

    /** start a new index off, empty */
    DiskLoc BtreeBucket::addBucket(const IndexDetails& id) {
        string ns = id.indexNamespace();
        DiskLoc loc = theDataFileMgr.insert(ns.c_str(), 0, BucketSize, true);
        BtreeBucket *b = loc.btreemod();
        b->init();
        return loc;
    }

    void BtreeBucket::renameIndexNamespace(const char *oldNs, const char *newNs) {
        renameNamespace( oldNs, newNs );
    }

    const DiskLoc BtreeBucket::getHead(const DiskLoc& thisLoc) const {
        DiskLoc p = thisLoc;
        while ( !p.btree()->isHead() )
            p = p.btree()->parent;
        return p;
    }

    DiskLoc BtreeBucket::advance(const DiskLoc& thisLoc, int& keyOfs, int direction, const char *caller) const {
        if ( keyOfs < 0 || keyOfs >= n ) {
            out() << "ASSERT failure BtreeBucket::advance, caller: " << caller << endl;
            out() << "  thisLoc: " << thisLoc.toString() << endl;
            out() << "  keyOfs: " << keyOfs << " n:" << n << " direction: " << direction << endl;
            out() << bucketSummary() << endl;
            assert(false);
        }
        int adj = direction < 0 ? 1 : 0;
        int ko = keyOfs + direction;
        DiskLoc nextDown = childForPos(ko+adj);
        if ( !nextDown.isNull() ) {
            while ( 1 ) {
                keyOfs = direction>0 ? 0 : nextDown.btree()->n - 1;
                DiskLoc loc = nextDown.btree()->childForPos(keyOfs + adj);
                if ( loc.isNull() )
                    break;
                nextDown = loc;
            }
            return nextDown;
        }

        if ( ko < n && ko >= 0 ) {
            keyOfs = ko;
            return thisLoc;
        }

        // end of bucket.  traverse back up.
        DiskLoc childLoc = thisLoc;
        DiskLoc ancestor = parent;
        while ( 1 ) {
            if ( ancestor.isNull() )
                break;
            const BtreeBucket *an = ancestor.btree();
            for ( int i = 0; i < an->n; i++ ) {
                if ( an->childForPos(i+adj) == childLoc ) {
                    keyOfs = i;
                    return ancestor;
                }
            }
            assert( direction<0 || an->nextChild == childLoc );
            // parent exhausted also, keep going up
            childLoc = ancestor;
            ancestor = an->parent;
        }

        return DiskLoc();
    }

    DiskLoc BtreeBucket::locate(const IndexDetails& idx, const DiskLoc& thisLoc, const BSONObj& key, const Ordering &order, int& pos, bool& found, const DiskLoc &recordLoc, int direction) const {
        return locate(idx, thisLoc, KeyOwned(key), order, pos, found, recordLoc, direction);
    }

    DiskLoc BtreeBucket::locate(const IndexDetails& idx, const DiskLoc& thisLoc, const Key& key, const Ordering &order, int& pos, bool& found, const DiskLoc &recordLoc, int direction) const {
        int p;
        found = find(idx, key, recordLoc, order, p, /*assertIfDup*/ false);
        if ( found ) {
            pos = p;
            return thisLoc;
        }

        DiskLoc child = childForPos(p);

        if ( !child.isNull() ) {
            DiskLoc l = child.btree()->locate(idx, child, key, order, pos, found, recordLoc, direction);
            if ( !l.isNull() )
                return l;
        }

        pos = p;
        if ( direction < 0 )
            return --pos == -1 ? DiskLoc() /*theend*/ : thisLoc;
        else
            return pos == n ? DiskLoc() /*theend*/ : thisLoc;
    }

    bool BtreeBucket::customFind( int l, int h, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction, DiskLoc &thisLoc, int &keyOfs, pair< DiskLoc, int > &bestParent ) const {
        while( 1 ) {
            if ( l + 1 == h ) {
                keyOfs = ( direction > 0 ) ? h : l;
                DiskLoc next = thisLoc.btree()->k( h ).prevChildBucket;
                if ( !next.isNull() ) {
                    bestParent = make_pair( thisLoc, keyOfs );
                    thisLoc = next;
                    return true;
                }
                else {
                    return false;
                }
            }
            int m = l + ( h - l ) / 2;
            int cmp = customBSONCmp( thisLoc.btree()->keyNode( m ).key.toBson(), keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction );
            if ( cmp < 0 ) {
                l = m;
            }
            else if ( cmp > 0 ) {
                h = m;
            }
            else {
                if ( direction < 0 ) {
                    l = m;
                }
                else {
                    h = m;
                }
            }
        }
    }

    /**
     * find smallest/biggest value greater-equal/less-equal than specified
     * starting thisLoc + keyOfs will be strictly less than/strictly greater than keyBegin/keyBeginLen/keyEnd
     * All the direction checks below allowed me to refactor the code, but possibly separate forward and reverse implementations would be more efficient
     */
    void BtreeBucket::advanceTo(DiskLoc &thisLoc, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction ) const {
        int l,h;
        bool dontGoUp;
        if ( direction > 0 ) {
            l = keyOfs;
            h = n - 1;
            dontGoUp = ( customBSONCmp( keyNode( h ).key.toBson(), keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) >= 0 );
        }
        else {
            l = 0;
            h = keyOfs;
            dontGoUp = ( customBSONCmp( keyNode( l ).key.toBson(), keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) <= 0 );
        }
        pair< DiskLoc, int > bestParent;
        if ( dontGoUp ) {
            // this comparison result assures h > l
            if ( !customFind( l, h, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction, thisLoc, keyOfs, bestParent ) ) {
                return;
            }
        }
        else {
            // go up parents until rightmost/leftmost node is >=/<= target or at top
            while( !thisLoc.btree()->parent.isNull() ) {
                thisLoc = thisLoc.btree()->parent;
                if ( direction > 0 ) {
                    if ( customBSONCmp( thisLoc.btree()->keyNode( thisLoc.btree()->n - 1 ).key.toBson(), keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) >= 0 ) {
                        break;
                    }
                }
                else {
                    if ( customBSONCmp( thisLoc.btree()->keyNode( 0 ).key.toBson(), keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) <= 0 ) {
                        break;
                    }
                }
            }
        }
        customLocate( thisLoc, keyOfs, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction, bestParent );
    }

    void BtreeBucket::customLocate(DiskLoc &thisLoc, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction, pair< DiskLoc, int > &bestParent ) const {
        if ( thisLoc.btree()->n == 0 ) {
            thisLoc = DiskLoc();
            return;
        }
        // go down until find smallest/biggest >=/<= target
        while( 1 ) {
            int l = 0;
            int h = thisLoc.btree()->n - 1;
            // leftmost/rightmost key may possibly be >=/<= search key
            bool firstCheck;
            if ( direction > 0 ) {
                firstCheck = ( customBSONCmp( thisLoc.btree()->keyNode( 0 ).key.toBson(), keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) >= 0 );
            }
            else {
                firstCheck = ( customBSONCmp( thisLoc.btree()->keyNode( h ).key.toBson(), keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) <= 0 );
            }
            if ( firstCheck ) {
                DiskLoc next;
                if ( direction > 0 ) {
                    next = thisLoc.btree()->k( 0 ).prevChildBucket;
                    keyOfs = 0;
                }
                else {
                    next = thisLoc.btree()->nextChild;
                    keyOfs = h;
                }
                if ( !next.isNull() ) {
                    bestParent = pair< DiskLoc, int >( thisLoc, keyOfs );
                    thisLoc = next;
                    continue;
                }
                else {
                    return;
                }
            }
            bool secondCheck;
            if ( direction > 0 ) {
                secondCheck = ( customBSONCmp( thisLoc.btree()->keyNode( h ).key.toBson(), keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) < 0 );
            }
            else {
                secondCheck = ( customBSONCmp( thisLoc.btree()->keyNode( 0 ).key.toBson(), keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) > 0 );
            }
            if ( secondCheck ) {
                DiskLoc next;
                if ( direction > 0 ) {
                    next = thisLoc.btree()->nextChild;
                }
                else {
                    next = thisLoc.btree()->k( 0 ).prevChildBucket;
                }
                if ( next.isNull() ) {
                    // if bestParent is null, we've hit the end and thisLoc gets set to DiskLoc()
                    thisLoc = bestParent.first;
                    keyOfs = bestParent.second;
                    return;
                }
                else {
                    thisLoc = next;
                    continue;
                }
            }
            if ( !customFind( l, h, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction, thisLoc, keyOfs, bestParent ) ) {
                return;
            }
        }
    }


    /** @thisLoc disk location of *this */
    int BtreeBucket::_insert(const DiskLoc thisLoc, const DiskLoc recordLoc,
                             const Key& key, const Ordering &order, bool dupsAllowed,
                             const DiskLoc lChild, const DiskLoc rChild, IndexDetails& idx) const {
        if ( key.dataSize() > KeyMax ) {
            problem() << "ERROR: key too large len:" << key.dataSize() << " max:" << KeyMax << ' ' << key.dataSize() << ' ' << idx.indexNamespace() << endl;
            return 2;
        }
        assert( key.dataSize() > 0 );

        int pos;
        bool found = find(idx, key, recordLoc, order, pos, !dupsAllowed);
        if ( insert_debug ) {
            out() << "  " << thisLoc.toString() << '.' << "_insert " <<
                  key.toString() << '/' << recordLoc.toString() <<
                  " l:" << lChild.toString() << " r:" << rChild.toString() << endl;
            out() << "    found:" << found << " pos:" << pos << " n:" << n << endl;
        }

        if ( found ) {
            const _KeyNode& kn = k(pos);
            if ( kn.isUnused() ) {
                log(4) << "btree _insert: reusing unused key" << endl;
                massert( 10285 , "_insert: reuse key but lchild is not null", lChild.isNull());
                massert( 10286 , "_insert: reuse key but rchild is not null", rChild.isNull());
                kn.writing().setUsed();
                return 0;
            }

            DEV {
                log() << "_insert(): key already exists in index (ok for background:true)\n";
                log() << "  " << idx.indexNamespace() << " thisLoc:" << thisLoc.toString() << '\n';
                log() << "  " << key.toString() << '\n';
                log() << "  " << "recordLoc:" << recordLoc.toString() << " pos:" << pos << endl;
                log() << "  old l r: " << childForPos(pos).toString() << ' ' << childForPos(pos+1).toString() << endl;
                log() << "  new l r: " << lChild.toString() << ' ' << rChild.toString() << endl;
            }
            alreadyInIndex();
        }

        DEBUGGING out() << "TEMP: key: " << key.toString() << endl;
        DiskLoc child = childForPos(pos);
        if ( insert_debug )
            out() << "    getChild(" << pos << "): " << child.toString() << endl;
        // In current usage, rChild isNull() for a new key and false when we are
        // promoting a split key.  These are the only two cases where _insert()
        // is called currently.
        if ( child.isNull() || !rChild.isNull() ) {
            // A new key will be inserted at the same tree height as an adjacent existing key.
            insertHere(thisLoc, pos, recordLoc, key, order, lChild, rChild, idx);
            return 0;
        }

        return child.btree()->_insert(child, recordLoc, key, order, dupsAllowed, /*lchild*/DiskLoc(), /*rchild*/DiskLoc(), idx);
    }

    void BtreeBucket::dump() const {
        out() << "DUMP btreebucket n:" << n;
        out() << " parent:" << hex << parent.getOfs() << dec;
        for ( int i = 0; i < n; i++ ) {
            out() << '\n';
            KeyNode k = keyNode(i);
            out() << '\t' << i << '\t' << k.key.toString() << "\tleft:" << hex <<
                  k.prevChildBucket.getOfs() << "\tRecLoc:" << k.recordLoc.toString() << dec;
            if ( this->k(i).isUnused() )
                out() << " UNUSED";
        }
        out() << " right:" << hex << nextChild.getOfs() << dec << endl;
    }

    /** todo: meaning of return code unclear clean up */
    int BtreeBucket::bt_insert(const DiskLoc thisLoc, const DiskLoc recordLoc,
                               const BSONObj& _key, const Ordering &order, bool dupsAllowed,
                               IndexDetails& idx, bool toplevel) const 
    {
        KeyOwned key(_key);

        if ( toplevel ) {
            if ( key.dataSize() > KeyMax ) {
                problem() << "Btree::insert: key too large to index, skipping " << idx.indexNamespace() << ' ' << key.dataSize() << ' ' << key.toString() << endl;
                return 3;
            }
        }

        int x = _insert(thisLoc, recordLoc, Key(key), order, dupsAllowed, DiskLoc(), DiskLoc(), idx);
        assertValid( order );

        return x;
    }

    void BtreeBucket::shape(stringstream& ss) const {
        _shape(0, ss);
    }

    int BtreeBucket::getLowWaterMark() {
        return lowWaterMark;
    }

    int BtreeBucket::getKeyMax() {
        return KeyMax;
    }

    DiskLoc BtreeBucket::findSingle( const IndexDetails& indexdetails , const DiskLoc& thisLoc, const BSONObj& key ) const {
        int pos;
        bool found;
        // TODO: is it really ok here that the order is a default?  
        // for findById() use, yes.  for checkNoIndexConflicts, no?
        Ordering o = Ordering::make(BSONObj());
        DiskLoc bucket = locate( indexdetails , indexdetails.head , key , o , pos , found , minDiskLoc );
        if ( bucket.isNull() )
            return bucket;

        const BtreeBucket *b = bucket.btree();
        while ( 1 ) {
            const _KeyNode& knraw = b->k(pos);
            if ( knraw.isUsed() )
                break;
            bucket = b->advance( bucket , pos , 1 , "findSingle" );
            if ( bucket.isNull() )
                return bucket;
            b = bucket.btree();
        }
        KeyNode kn = b->keyNode( pos );
        if ( KeyOwned(key).woCompare( kn.key, o ) != 0 )
            return DiskLoc();
        return kn.recordLoc;
    }

} // namespace mongo

#include "db.h"
#include "dbhelpers.h"

namespace mongo {

    void BtreeBucket::a_test(IndexDetails& id) {
        BtreeBucket *b = id.head.btreemod();

        // record locs for testing
        DiskLoc A(1, 20);
        DiskLoc B(1, 30);
        DiskLoc C(1, 40);

        DiskLoc rl;
        BSONObj key = fromjson("{x:9}");
        BSONObj orderObj = fromjson("{}");
        Ordering order = Ordering::make(orderObj);

        b->bt_insert(id.head, A, key, order, true, id);
        A.GETOFS() += 2;
        b->bt_insert(id.head, A, key, order, true, id);
        A.GETOFS() += 2;
        b->bt_insert(id.head, A, key, order, true, id);
        A.GETOFS() += 2;
        b->bt_insert(id.head, A, key, order, true, id);
        A.GETOFS() += 2;
        assert( b->k(0).isUsed() );
//        b->k(0).setUnused();
        b->k(1).setUnused();
        b->k(2).setUnused();
        b->k(3).setUnused();

        b->dumpTree(id.head, orderObj);

        /*        b->bt_insert(id.head, B, key, order, false, id);
        b->k(1).setUnused();

        b->dumpTree(id.head, order);

        b->bt_insert(id.head, A, key, order, false, id);

        b->dumpTree(id.head, order);
        */

        // this should assert.  does it? (it might "accidentally" though, not asserting proves a problem, asserting proves nothing)
        b->bt_insert(id.head, C, key, order, false, id);

//        b->dumpTree(id.head, order);
    }

    /* --- BtreeBuilder --- */

    BtreeBuilder::BtreeBuilder(bool _dupsAllowed, IndexDetails& _idx) :
        dupsAllowed(_dupsAllowed),
        idx(_idx),
        n(0),
        order( idx.keyPattern() ),
        ordering( Ordering::make(idx.keyPattern()) ) {
        first = cur = BtreeBucket::addBucket(idx);
        b = cur.btreemod();
        committed = false;
    }

    void BtreeBuilder::newBucket() {
        DiskLoc L = BtreeBucket::addBucket(idx);
        b->tempNext() = L;
        cur = L;
        b = cur.btreemod();
    }

    void BtreeBuilder::mayCommitProgressDurably() {
        if ( getDur().commitIfNeeded() ) {
            b = cur.btreemod();
        }
    }

    void BtreeBuilder::addKey(BSONObj& _key, DiskLoc loc) {
        KeyOwned key(_key);

        if ( key.dataSize() > KeyMax ) {
            problem() << "Btree::insert: key too large to index, skipping " << idx.indexNamespace() 
                      << ' ' << key.dataSize() << ' ' << key.toString() << endl;
            return;
        }

        if( !dupsAllowed ) {
            if( n > 0 ) {
                int cmp = keyLast.woCompare(key, ordering);
                massert( 10288 ,  "bad key order in BtreeBuilder - server internal error", cmp <= 0 );
                if( cmp == 0 ) {
                    //if( !dupsAllowed )
                    uasserted( ASSERT_ID_DUPKEY , BtreeBucket::dupKeyError( idx , keyLast ) );
                }
            }
            keyLast = key;
        }

        if ( ! b->_pushBack(loc, key, ordering, DiskLoc()) ) {
            // bucket was full
            newBucket();
            b->pushBack(loc, key, ordering, DiskLoc());
        }
        n++;
        mayCommitProgressDurably();
    }

    void BtreeBuilder::buildNextLevel(DiskLoc loc) {
        int levels = 1;
        while( 1 ) {
            if( loc.btree()->tempNext().isNull() ) {
                // only 1 bucket at this level. we are done.
                getDur().writingDiskLoc(idx.head) = loc;
                break;
            }
            levels++;

            DiskLoc upLoc = BtreeBucket::addBucket(idx);
            DiskLoc upStart = upLoc;
            BtreeBucket *up = upLoc.btreemod();

            DiskLoc xloc = loc;
            while( !xloc.isNull() ) {
                if ( getDur().commitIfNeeded() ) {
                    b = cur.btreemod();
                    up = upLoc.btreemod();
                }

                BtreeBucket *x = xloc.btreemod();
                Key k;
                DiskLoc r;
                x->popBack(r,k);
                bool keepX = ( x->n != 0 );
                DiskLoc keepLoc = keepX ? xloc : x->nextChild;

                if ( ! up->_pushBack(r, k, ordering, keepLoc) ) {
                    // current bucket full
                    DiskLoc n = BtreeBucket::addBucket(idx);
                    up->tempNext() = n;
                    upLoc = n;
                    up = upLoc.btreemod();
                    up->pushBack(r, k, ordering, keepLoc);
                }

                DiskLoc nextLoc = x->tempNext(); // get next in chain at current level
                if ( keepX ) {
                    x->parent = upLoc;
                }
                else {
                    if ( !x->nextChild.isNull() )
                        x->nextChild.btreemod()->parent = upLoc;
                    x->deallocBucket( xloc, idx );
                }
                xloc = nextLoc;
            }

            loc = upStart;
            mayCommitProgressDurably();
        }

        if( levels > 1 )
            log(2) << "btree levels: " << levels << endl;
    }

    /** when all addKeys are done, we then build the higher levels of the tree */
    void BtreeBuilder::commit() {
        buildNextLevel(first);
        committed = true;
    }

    BtreeBuilder::~BtreeBuilder() {
        DESTRUCTOR_GUARD(
            if( !committed ) {
                log(2) << "Rolling back partially built index space" << endl;
                DiskLoc x = first;
                while( !x.isNull() ) {
                    DiskLoc next = x.btree()->tempNext();
                    string ns = idx.indexNamespace();
                    theDataFileMgr._deleteRecord(nsdetails(ns.c_str()), ns.c_str(), x.rec(), x);
                    x = next;
                    getDur().commitIfNeeded();
                }
                assert( idx.head.isNull() );
                log(2) << "done rollback" << endl;
            }
        )
    }

}
