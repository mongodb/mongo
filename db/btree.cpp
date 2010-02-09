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

#include "stdafx.h"
#include "db.h"
#include "btree.h"
#include "pdfile.h"
#include "json.h"
#include "clientcursor.h"
#include "client.h"
#include "dbhelpers.h"

namespace mongo {

#define VERIFYTHISLOC dassert( thisLoc.btree() == this );

    KeyNode::KeyNode(const BucketBasics& bb, const _KeyNode &k) :
            prevChildBucket(k.prevChildBucket),
            recordLoc(k.recordLoc), key(bb.data+k.keyDataOfs())
    { }

    const int KeyMax = BucketSize / 10;

    extern int otherTraceLevel;
    const int split_debug = 0;
    const int insert_debug = 0;

    /* BucketBasics --------------------------------------------------- */

    inline void BucketBasics::modified(const DiskLoc& thisLoc) {
        VERIFYTHISLOC
        btreeStore->modified(thisLoc);
    }

    int BucketBasics::Size() const {
        assert( _Size == BucketSize );
        return _Size;
    }
    inline void BucketBasics::setNotPacked() {
        flags &= ~Packed;
    }
    inline void BucketBasics::setPacked() {
        flags |= Packed;
    }

    void BucketBasics::_shape(int level, stringstream& ss) {
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

    void BucketBasics::dumpTree(DiskLoc thisLoc, const BSONObj &order) {
        bt_dmp=1;
        fullValidate(thisLoc, order);
        bt_dmp=0;
    }

    int BucketBasics::fullValidate(const DiskLoc& thisLoc, const BSONObj &order) {
        {
            bool f = false;
            assert( f = true );
            massert("assert is misdefined", f);
        }

        checkForInterrupt();
        assertValid(order, true);
//	if( bt_fv==0 )
//		return;

        if ( bt_dmp ) {
            out() << thisLoc.toString() << ' ';
            ((BtreeBucket *) this)->dump();
        }

        // keycount
        int kc = 0;

        for ( int i = 0; i < n; i++ ) {
            _KeyNode& kn = k(i);

            if ( kn.isUsed() ) kc++;
            if ( !kn.prevChildBucket.isNull() ) {
                DiskLoc left = kn.prevChildBucket;
                BtreeBucket *b = left.btree();
                wassert( b->parent == thisLoc );
                kc += b->fullValidate(kn.prevChildBucket, order);
            }
        }
        if ( !nextChild.isNull() ) {
            BtreeBucket *b = nextChild.btree();
            wassert( b->parent == thisLoc );
            kc += b->fullValidate(nextChild, order);
        }

        return kc;
    }

    int nDumped = 0;

    void BucketBasics::assertValid(const BSONObj &order, bool force) {
        if ( !debug && !force )
            return;
        wassert( n >= 0 && n < Size() );
        wassert( emptySize >= 0 && emptySize < BucketSize );
        wassert( topSize >= n && topSize <= BucketSize );
        DEV {
            // slow:
            for ( int i = 0; i < n-1; i++ ) {
                BSONObj k1 = keyNode(i).key;
                BSONObj k2 = keyNode(i+1).key;
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
                BSONObj k1 = keyNode(0).key;
                BSONObj k2 = keyNode(n-1).key;
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
        return Size() - (data-(char*)this);
    }

    void BucketBasics::init() {
        parent.Null();
        nextChild.Null();
        _Size = BucketSize;
        flags = Packed;
        n = 0;
        emptySize = totalDataSize();
        topSize = 0;
        reserved = 0;
    }

    /* see _alloc */
    inline void BucketBasics::_unalloc(int bytes) {
        topSize -= bytes;
        emptySize += bytes;
    }

    /* we allocate space from the end of the buffer for data.
       the keynodes grow from the front.
    */
    inline int BucketBasics::_alloc(int bytes) {
        topSize += bytes;
        emptySize -= bytes;
        int ofs = totalDataSize() - topSize;
        assert( ofs > 0 );
        return ofs;
    }

    void BucketBasics::_delKeyAtPos(int keypos) {
        assert( keypos >= 0 && keypos <= n );
        assert( childForPos(keypos).isNull() );
        n--;
        assert( n > 0 || nextChild.isNull() );
        for ( int j = keypos; j < n; j++ )
            k(j) = k(j+1);
        emptySize += sizeof(_KeyNode);
        setNotPacked();
    }

    /* pull rightmost key from the bucket.  this version requires its right child to be null so it 
	   does not bother returning that value.
    */
    void BucketBasics::popBack(DiskLoc& recLoc, BSONObj& key) { 
        massert( "n==0 in btree popBack()", n > 0 );
        assert( k(n-1).isUsed() ); // no unused skipping in this function at this point - btreebuilder doesn't require that
        KeyNode kn = keyNode(n-1);
        recLoc = kn.recordLoc;
        key = kn.key;
        int keysize = kn.key.objsize();

		massert("rchild not null in btree popBack()", nextChild.isNull());

		/* weirdly, we also put the rightmost down pointer in nextchild, even when bucket isn't full. */
		nextChild = kn.prevChildBucket;

        n--;
        emptySize += sizeof(_KeyNode);
        _unalloc(keysize);
    }

    /* add a key.  must be > all existing.  be careful to set next ptr right. */
    bool BucketBasics::_pushBack(const DiskLoc& recordLoc, BSONObj& key, const BSONObj &order, DiskLoc prevChild) {
        int bytesNeeded = key.objsize() + sizeof(_KeyNode);
        if ( bytesNeeded > emptySize )
            return false;
        assert( bytesNeeded <= emptySize );
        assert( n == 0 || keyNode(n-1).key.woCompare(key, order) <= 0 );
        emptySize -= sizeof(_KeyNode);
        _KeyNode& kn = k(n++);
        kn.prevChildBucket = prevChild;
        kn.recordLoc = recordLoc;
        kn.setKeyDataOfs( (short) _alloc(key.objsize()) );
        char *p = dataAt(kn.keyDataOfs());
        memcpy(p, key.objdata(), key.objsize());
        return true;
    }
    /*void BucketBasics::pushBack(const DiskLoc& recordLoc, BSONObj& key, const BSONObj &order, DiskLoc prevChild, DiskLoc nextChild) { 
        pushBack(recordLoc, key, order, prevChild);
        childForPos(n) = nextChild;
    }*/

    /* insert a key in a bucket with no complexity -- no splits required */
    bool BucketBasics::basicInsert(const DiskLoc& thisLoc, int keypos, const DiskLoc& recordLoc, const BSONObj& key, const BSONObj &order) {
        modified(thisLoc);
        assert( keypos >= 0 && keypos <= n );
        int bytesNeeded = key.objsize() + sizeof(_KeyNode);
        if ( bytesNeeded > emptySize ) {
            pack( order );
            if ( bytesNeeded > emptySize )
                return false;
        }
        for ( int j = n; j > keypos; j-- ) // make room
            k(j) = k(j-1);
        n++;
        emptySize -= sizeof(_KeyNode);
        _KeyNode& kn = k(keypos);
        kn.prevChildBucket.Null();
        kn.recordLoc = recordLoc;
        kn.setKeyDataOfs((short) _alloc(key.objsize()) );
        char *p = dataAt(kn.keyDataOfs());
        memcpy(p, key.objdata(), key.objsize());
        return true;
    }

    /* when we delete things we just leave empty space until the node is
       full and then we repack it.
    */
    void BucketBasics::pack( const BSONObj &order ) {
        if ( flags & Packed )
            return;

        int tdz = totalDataSize();
        char temp[BucketSize];
        int ofs = tdz;
        topSize = 0;
        for ( int j = 0; j < n; j++ ) {
            short ofsold = k(j).keyDataOfs();
            int sz = keyNode(j).key.objsize();
            ofs -= sz;
            topSize += sz;
            memcpy(temp+ofs, dataAt(ofsold), sz);
            k(j).setKeyDataOfsSavingUse( ofs );
        }
        int dataUsed = tdz - ofs;
        memcpy(data + ofs, temp + ofs, dataUsed);
        emptySize = tdz - dataUsed - n * sizeof(_KeyNode);
        assert( emptySize >= 0 );

        setPacked();
        assertValid( order );
    }

    inline void BucketBasics::truncateTo(int N, const BSONObj &order) {
        n = N;
        setNotPacked();
        pack( order );
    }

    /* - BtreeBucket --------------------------------------------------- */

    /* return largest key in the subtree. */
    void BtreeBucket::findLargestKey(const DiskLoc& thisLoc, DiskLoc& largestLoc, int& largestKey) {
        DiskLoc loc = thisLoc;
        while ( 1 ) {
            BtreeBucket *b = loc.btree();
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

    bool BtreeBucket::exists(const IndexDetails& idx, DiskLoc thisLoc, const BSONObj& key, BSONObj order) { 
        int pos;
        bool found;
        DiskLoc b = locate(idx, thisLoc, key, order, pos, found, minDiskLoc);

        // skip unused keys
        while ( 1 ) {
            if( b.isNull() )
                break;
            BtreeBucket *bucket = b.btree();
            _KeyNode& kn = bucket->k(pos);
            if ( kn.isUsed() )
                return bucket->keyAt(pos).woEqual(key);
            b = bucket->advance(b, pos, 1, "BtreeBucket::exists");
        }
        return false;
    }

    string BtreeBucket::dupKeyError( const IndexDetails& idx , const BSONObj& key ){
        stringstream ss;
        ss << "E11000 duplicate key error";
        ss << "index: " << idx.indexNamespace() << "  ";
        ss << "dup key: " << key;
        return ss.str();
    }

    /* Find a key withing this btree bucket.
 
       When duplicate keys are allowed, we use the DiskLoc of the record as if it were part of the 
       key.  That assures that even when there are many duplicates (e.g., 1 million) for a key,
       our performance is still good.

       assertIfDup: if the key exists (ignoring the recordLoc), uassert

       pos: for existing keys k0...kn-1.
       returns # it goes BEFORE.  so key[pos-1] < key < key[pos]
       returns n if it goes after the last existing key.
       note result might be an Unused location!
    */
	char foo;
    bool BtreeBucket::find(const IndexDetails& idx, const BSONObj& key, DiskLoc recordLoc, const BSONObj &order, int& pos, bool assertIfDup) {
#if defined(_EXPERIMENT1)
		{
			char *z = (char *) this;
			int i = 0;
			while( 1 ) {
				i += 4096;
				if( i >= BucketSize )
					break;
				foo += z[i];
			}
		}
#endif
        /* binary search for this key */
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
                            if( idx.head.btree()->exists(idx, idx.head, key, order) )
                                uasserted( dupKeyError( idx , key ) );
                        }
                    }
                    else
                        uasserted( dupKeyError( idx , key ) );
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
            BSONObj keyatpos = keyNode(pos).key;
            wassert( key.woCompare(keyatpos, order) <= 0 );
            if ( pos > 0 ) {
                wassert( keyNode(pos-1).key.woCompare(key, order) <= 0 );
            }
        }

        return false;
    }

    void BtreeBucket::delBucket(const DiskLoc& thisLoc, IndexDetails& id) {
        ClientCursor::informAboutToDeleteBucket(thisLoc);
        assert( !isHead() );

        BtreeBucket *p = parent.btreemod();
        if ( p->nextChild == thisLoc ) {
            p->nextChild.Null();
        }
        else {
            for ( int i = 0; i < p->n; i++ ) {
                if ( p->k(i).prevChildBucket == thisLoc ) {
                    p->k(i).prevChildBucket.Null();
                    goto found;
                }
            }
            out() << "ERROR: can't find ref to deleted bucket.\n";
            out() << "To delete:\n";
            dump();
            out() << "Parent:\n";
            p->dump();
            assert(false);
        }
found:
        deallocBucket( thisLoc );
    }
    
    void BtreeBucket::deallocBucket(const DiskLoc &thisLoc) {
#if 1
        /* as a temporary defensive measure, we zap the whole bucket, AND don't truly delete
           it (meaning it is ineligible for reuse).
           */
        memset(this, 0, Size());
        modified(thisLoc);
#else
        //defensive:
        n = -1;
        parent.Null();
        massert("todo: use RecStoreInterface instead", false);
        // TODO: this was broken anyway as deleteRecord does unindexRecord() call which assumes the data is a BSONObj, 
        // and it isn't.
        assert(false);
//        theDataFileMgr.deleteRecord(id.indexNamespace().c_str(), thisLoc.rec(), thisLoc);
#endif
    }

    /* note: may delete the entire bucket!  this invalid upon return sometimes. */
    void BtreeBucket::delKeyAtPos(const DiskLoc& thisLoc, IndexDetails& id, int p) {
        modified(thisLoc);
        assert(n>0);
        DiskLoc left = childForPos(p);

        if ( n == 1 ) {
            if ( left.isNull() && nextChild.isNull() ) {
                if ( isHead() )
                    _delKeyAtPos(p); // we don't delete the top bucket ever
                else
                    delBucket(thisLoc, id);
                return;
            }
            markUnused(p);
            return;
        }

        if ( left.isNull() )
            _delKeyAtPos(p);
        else
            markUnused(p);
    }

    int qqq = 0;

    /* remove a key from the index */
    bool BtreeBucket::unindex(const DiskLoc& thisLoc, IndexDetails& id, BSONObj& key, const DiskLoc& recordLoc ) {
        if ( key.objsize() > KeyMax ) {
            OCCASIONALLY problem() << "unindex: key too large to index, skipping " << id.indexNamespace() << /* ' ' << key.toString() << */ '\n';
            return false;
        }

        int pos;
        bool found;
        DiskLoc loc = locate(id, thisLoc, key, id.keyPattern(), pos, found, recordLoc, 1);
        if ( found ) {
            loc.btree()->delKeyAtPos(loc, id, pos);
            return true;
        }
        return false;
    }

    BtreeBucket* BtreeBucket::allocTemp() {
        BtreeBucket *b = (BtreeBucket*) malloc(BucketSize);
        b->init();
        return b;
    }

    inline void fix(const DiskLoc& thisLoc, const DiskLoc& child) {
        if ( !child.isNull() ) {
            if ( insert_debug )
                out() << "      " << child.toString() << ".parent=" << thisLoc.toString() << endl;
            child.btreemod()->parent = thisLoc;
        }
    }

    /* this sucks.  maybe get rid of parent ptrs. */
    void BtreeBucket::fixParentPtrs(const DiskLoc& thisLoc) {
        VERIFYTHISLOC
        fix(thisLoc, nextChild);
        for ( int i = 0; i < n; i++ )
            fix(thisLoc, k(i).prevChildBucket);
    }

    /* insert a key in this bucket, splitting if necessary.
       keypos - where to insert the key i3n range 0..n.  0=make leftmost, n=make rightmost.
    */
    void BtreeBucket::insertHere(DiskLoc thisLoc, int keypos,
                                 DiskLoc recordLoc, const BSONObj& key, const BSONObj& order,
                                 DiskLoc lchild, DiskLoc rchild, IndexDetails& idx)
    {
        modified(thisLoc);
        if ( insert_debug )
            out() << "   " << thisLoc.toString() << ".insertHere " << key.toString() << '/' << recordLoc.toString() << ' '
                 << lchild.toString() << ' ' << rchild.toString() << " keypos:" << keypos << endl;

        DiskLoc oldLoc = thisLoc;

        if ( basicInsert(thisLoc, keypos, recordLoc, key, order) ) {
            _KeyNode& kn = k(keypos);
            if ( keypos+1 == n ) { // last key
                if ( nextChild != lchild ) {
                    out() << "ERROR nextChild != lchild" << endl;
                    out() << "  thisLoc: " << thisLoc.toString() << ' ' << idx.indexNamespace() << endl;
                    out() << "  keyPos: " << keypos << " n:" << n << endl;
                    out() << "  nextChild: " << nextChild.toString() << " lchild: " << lchild.toString() << endl;
                    out() << "  recordLoc: " << recordLoc.toString() << " rchild: " << rchild.toString() << endl;
                    out() << "  key: " << key.toString() << endl;
                    dump();
#if 0
                    out() << "\n\nDUMPING FULL INDEX" << endl;
                    bt_dmp=1;
                    bt_fv=1;
                    idx.head.btree()->fullValidate(idx.head);
#endif
                    assert(false);
                }
                kn.prevChildBucket = nextChild;
                assert( kn.prevChildBucket == lchild );
                nextChild = rchild;
                if ( !rchild.isNull() )
                    rchild.btreemod()->parent = thisLoc;
            }
            else {
                k(keypos).prevChildBucket = lchild;
                if ( k(keypos+1).prevChildBucket != lchild ) {
                    out() << "ERROR k(keypos+1).prevChildBucket != lchild" << endl;
                    out() << "  thisLoc: " << thisLoc.toString() << ' ' << idx.indexNamespace() << endl;
                    out() << "  keyPos: " << keypos << " n:" << n << endl;
                    out() << "  k(keypos+1).pcb: " << k(keypos+1).prevChildBucket.toString() << " lchild: " << lchild.toString() << endl;
                    out() << "  recordLoc: " << recordLoc.toString() << " rchild: " << rchild.toString() << endl;
                    out() << "  key: " << key.toString() << endl;
                    dump();
#if 0
                    out() << "\n\nDUMPING FULL INDEX" << endl;
                    bt_dmp=1;
                    bt_fv=1;
                    idx.head.btree()->fullValidate(idx.head);
#endif
                    assert(false);
                }
                k(keypos+1).prevChildBucket = rchild;
                if ( !rchild.isNull() )
                    rchild.btreemod()->parent = thisLoc;
            }
            return;
        }

        /* ---------- split ---------------- */

        if ( split_debug )
            out() << "    " << thisLoc.toString() << ".split" << endl;

        int mid = n / 2;

        DiskLoc rLoc = addBucket(idx);
        BtreeBucket *r = rLoc.btreemod();
        if ( split_debug )
            out() << "     mid:" << mid << ' ' << keyNode(mid).key.toString() << " n:" << n << endl;
        for ( int i = mid+1; i < n; i++ ) {
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
            KeyNode middle = keyNode(mid);
            nextChild = middle.prevChildBucket; // middle key gets promoted, its children will be thisLoc (l) and rLoc (r)
            if ( split_debug ) {
                out() << "    middle key:" << middle.key.toString() << endl;
            }

            // promote middle to a parent node
            if ( parent.isNull() ) {
                // make a new parent if we were the root
                DiskLoc L = addBucket(idx);
                BtreeBucket *p = L.btreemod();
                p->pushBack(middle.recordLoc, middle.key, order, thisLoc);
                p->nextChild = rLoc;
                p->assertValid( order );
                parent = idx.head = L;
                if ( split_debug )
                    out() << "    we were root, making new root:" << hex << parent.getOfs() << dec << endl;
                rLoc.btreemod()->parent = parent;
            }
            else {
                /* set this before calling _insert - if it splits it will do fixParent() logic and change the value.
                */
                rLoc.btreemod()->parent = parent;
                if ( split_debug )
                    out() << "    promoting middle key " << middle.key.toString() << endl;
                parent.btree()->_insert(parent, middle.recordLoc, middle.key, order, /*dupsallowed*/true, thisLoc, rLoc, idx);
            }
        }

        truncateTo(mid, order);  // note this may trash middle.key.  thus we had to promote it before finishing up here.

        // add our new key, there is room now
        {

            if ( keypos <= mid ) {
                if ( split_debug )
                    out() << "  keypos<mid, insertHere() the new key" << endl;
                insertHere(thisLoc, keypos, recordLoc, key, order, lchild, rchild, idx);
            } else {
                int kp = keypos-mid-1;
                assert(kp>=0);
                rLoc.btree()->insertHere(rLoc, kp, recordLoc, key, order, lchild, rchild, idx);
            }
        }

        if ( split_debug )
            out() << "     split end " << hex << thisLoc.getOfs() << dec << endl;
    }

    /* start a new index off, empty */
    DiskLoc BtreeBucket::addBucket(IndexDetails& id) {
        DiskLoc loc = btreeStore->insert(id.indexNamespace().c_str(), 0, BucketSize, true);
        BtreeBucket *b = loc.btreemod();
        b->init();
        return loc;
    }

    void BtreeBucket::renameIndexNamespace(const char *oldNs, const char *newNs) {
        btreeStore->rename( oldNs, newNs );
    }

    DiskLoc BtreeBucket::getHead(const DiskLoc& thisLoc) {
        DiskLoc p = thisLoc;
        while ( !p.btree()->isHead() )
            p = p.btree()->parent;
        return p;
    }

    DiskLoc BtreeBucket::advance(const DiskLoc& thisLoc, int& keyOfs, int direction, const char *caller) {
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
            BtreeBucket *an = ancestor.btree();
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

    DiskLoc BtreeBucket::locate(const IndexDetails& idx, const DiskLoc& thisLoc, const BSONObj& key, const BSONObj &order, int& pos, bool& found, DiskLoc recordLoc, int direction) {
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

    /* @thisLoc disk location of *this
    */
    int BtreeBucket::_insert(DiskLoc thisLoc, DiskLoc recordLoc,
                             const BSONObj& key, const BSONObj &order, bool dupsAllowed,
                             DiskLoc lChild, DiskLoc rChild, IndexDetails& idx) {
        if ( key.objsize() > KeyMax ) {
            problem() << "ERROR: key too large len:" << key.objsize() << " max:" << KeyMax << ' ' << key.objsize() << ' ' << idx.indexNamespace() << endl;
            return 2;
        }
        assert( key.objsize() > 0 );

        int pos;
        bool found = find(idx, key, recordLoc, order, pos, !dupsAllowed);
        if ( insert_debug ) {
            out() << "  " << thisLoc.toString() << '.' << "_insert " <<
                 key.toString() << '/' << recordLoc.toString() <<
                 " l:" << lChild.toString() << " r:" << rChild.toString() << endl;
            out() << "    found:" << found << " pos:" << pos << " n:" << n << endl;
        }

        if ( found ) {
            _KeyNode& kn = k(pos);
            if ( kn.isUnused() ) {
                log(4) << "btree _insert: reusing unused key" << endl;
                massert("_insert: reuse key but lchild is not null", lChild.isNull());
                massert("_insert: reuse key but rchild is not null", rChild.isNull());
                kn.setUsed();
                return 0;
            }

            out() << "_insert(): key already exists in index\n";
            out() << "  " << idx.indexNamespace().c_str() << " thisLoc:" << thisLoc.toString() << '\n';
            out() << "  " << key.toString() << '\n';
            out() << "  " << "recordLoc:" << recordLoc.toString() << " pos:" << pos << endl;
            out() << "  old l r: " << childForPos(pos).toString() << ' ' << childForPos(pos+1).toString() << endl;
            out() << "  new l r: " << lChild.toString() << ' ' << rChild.toString() << endl;
            massert("btree: key+recloc already in index", false);
        }

        DEBUGGING out() << "TEMP: key: " << key.toString() << endl;
        DiskLoc& child = childForPos(pos);
        if ( insert_debug )
            out() << "    getChild(" << pos << "): " << child.toString() << endl;
        if ( child.isNull() || !rChild.isNull() /* means an 'internal' insert */ ) {
            insertHere(thisLoc, pos, recordLoc, key, order, lChild, rChild, idx);
            return 0;
        }

        return child.btree()->bt_insert(child, recordLoc, key, order, dupsAllowed, idx, /*toplevel*/false);
    }

    void BtreeBucket::dump() {
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

    /* todo: meaning of return code unclear clean up */
    int BtreeBucket::bt_insert(DiskLoc thisLoc, DiskLoc recordLoc,
                            const BSONObj& key, const BSONObj &order, bool dupsAllowed,
                            IndexDetails& idx, bool toplevel)
    {
        if ( toplevel ) {
            if ( key.objsize() > KeyMax ) {
                problem() << "Btree::insert: key too large to index, skipping " << idx.indexNamespace().c_str() << ' ' << key.objsize() << ' ' << key.toString() << '\n';
                return 3;
            }
        }

        int x = _insert(thisLoc, recordLoc, key, order, dupsAllowed, DiskLoc(), DiskLoc(), idx);
        assertValid( order );

        return x;
    }

    void BtreeBucket::shape(stringstream& ss) {
        _shape(0, ss);
    }
    
    DiskLoc BtreeBucket::findSingle( const IndexDetails& indexdetails , const DiskLoc& thisLoc, const BSONObj& key ){
        int pos;
        bool found;
        DiskLoc bucket = locate( indexdetails , indexdetails.head , key , BSONObj() , pos , found , minDiskLoc );
        if ( bucket.isNull() )
            return bucket;

        BtreeBucket *b = bucket.btree();
        while ( 1 ){
            _KeyNode& knraw = b->k(pos);
            if ( knraw.isUsed() )
                break;
            bucket = b->advance( bucket , pos , 1 , "findSingle" );
            if ( bucket.isNull() )
                return bucket;
            b = bucket.btree();
        }
        KeyNode kn = b->keyNode( pos );
        if ( key.woCompare( kn.key ) != 0 )
            return DiskLoc();
        return kn.recordLoc;
    }

} // namespace mongo

#include "db.h"
#include "dbhelpers.h"

namespace mongo {

    void BtreeBucket::a_test(IndexDetails& id) {
        BtreeBucket *b = id.head.btree();

        // record locs for testing
        DiskLoc A(1, 20);
        DiskLoc B(1, 30);
        DiskLoc C(1, 40);

        DiskLoc rl;
        BSONObj key = fromjson("{x:9}");
        BSONObj order = fromjson("{}");

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

        b->dumpTree(id.head, order);

        /*        b->bt_insert(id.head, B, key, order, false, id);
        b->k(1).setUnused();

        b->dumpTree(id.head, order);
        cout << "---\n";

        b->bt_insert(id.head, A, key, order, false, id);

        b->dumpTree(id.head, order);
        cout << "---\n";*/

        // this should assert.  does it? (it might "accidentally" though, not asserting proves a problem, asserting proves nothing)
        b->bt_insert(id.head, C, key, order, false, id);

        b->dumpTree(id.head, order);
    }

    /* --- BtreeBuilder --- */

    BtreeBuilder::BtreeBuilder(bool _dupsAllowed, IndexDetails& _idx) : 
      dupsAllowed(_dupsAllowed), idx(_idx), n(0) 
    {
        first = cur = BtreeBucket::addBucket(idx);
        b = cur.btreemod();
        order = idx.keyPattern();
        committed = false;
    }

    void BtreeBuilder::newBucket() { 
        DiskLoc L = BtreeBucket::addBucket(idx);
        b->tempNext() = L;
        cur = L;
        b = cur.btreemod();
    }

    void BtreeBuilder::addKey(BSONObj& key, DiskLoc loc) { 
        if( !dupsAllowed ) {
            if( n > 0 ) {
                int cmp = keyLast.woCompare(key, order);
                massert( "bad key order in BtreeBuilder - server internal error", cmp <= 0 );
                if( cmp == 0 ) {
                    //if( !dupsAllowed )
                    uasserted( BtreeBucket::dupKeyError( idx , keyLast ) );
                }
            }
            keyLast = key;
        }

        if ( ! b->_pushBack(loc, key, order, DiskLoc()) ){
            // no room
            if ( key.objsize() > KeyMax ) {
                problem() << "Btree::insert: key too large to index, skipping " << idx.indexNamespace().c_str() << ' ' << key.objsize() << ' ' << key.toString() << '\n';
            }
            else { 
                // bucket was full
                newBucket();
                b->pushBack(loc, key, order, DiskLoc());
            }
        }
        n++;
    }

    void BtreeBuilder::buildNextLevel(DiskLoc loc) { 
        int levels = 1;
        while( 1 ) { 
            if( loc.btree()->tempNext().isNull() ) { 
                // only 1 bucket at this level. we are done.
                idx.head = loc;
                break;
            }
            levels++;

            DiskLoc upLoc = BtreeBucket::addBucket(idx);
            DiskLoc upStart = upLoc;
            BtreeBucket *up = upLoc.btreemod();

            DiskLoc xloc = loc;
            while( !xloc.isNull() ) { 
                BtreeBucket *x = xloc.btreemod();
                BSONObj k; 
                DiskLoc r;
                x->popBack(r,k);
                bool keepX = ( x->n != 0 );
                DiskLoc keepLoc = keepX ? xloc : x->nextChild;

                if ( ! up->_pushBack(r, k, order, keepLoc) ){
                    // current bucket full
                    DiskLoc n = BtreeBucket::addBucket(idx);
                    up->tempNext() = n;
                    upLoc = n; 
                    up = upLoc.btreemod();
                    up->pushBack(r, k, order, keepLoc);
                }

                DiskLoc nextLoc = x->tempNext(); /* get next in chain at current level */
                if ( keepX ) {
                    x->parent = upLoc;                
                } else {
                    if ( !x->nextChild.isNull() )
                        x->nextChild.btreemod()->parent = upLoc;
                    x->deallocBucket( xloc );
                }
                xloc = nextLoc;
            }
            
            loc = upStart;
        }

        if( levels > 1 )
            log(2) << "btree levels: " << levels << endl;
    }

    /* when all addKeys are done, we then build the higher levels of the tree */
    void BtreeBuilder::commit() { 
        buildNextLevel(first);
        committed = true;
    }

    BtreeBuilder::~BtreeBuilder() { 
        if( !committed ) { 
            log(2) << "Rolling back partially built index space" << endl;
            DiskLoc x = first;
            while( !x.isNull() ) { 
                DiskLoc next = x.btree()->tempNext();
                btreeStore->deleteRecord(idx.indexNamespace().c_str(), x);
                x = next;
            }
            assert( idx.head.isNull() );
            log(2) << "done rollback" << endl;
        }
    }

}
