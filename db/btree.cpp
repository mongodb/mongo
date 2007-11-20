// btree.cpp

#include "stdafx.h"
#include "btree.h"
#include "pdfile.h"

/* it is easy to do custom sizes for a namespace - all the same for now */
const int BucketSize = 8192;
const int KeyMax = BucketSize / 8;

int ninserts = 0;

inline KeyNode::KeyNode(BucketBasics& bb, _KeyNode &k) : 
  prevChildBucket(k.prevChildBucket), recordLoc(k.recordLoc), key(bb.data+k.keyDataOfs) { }

/* - BucketBasics --------------------------------------------------- */

inline void BucketBasics::setNotPacked() { flags &= ~Packed; }
inline void BucketBasics::setPacked() { flags |= Packed; }

void BucketBasics::assertValid() { 
	if( !debug )
		return;
	assert( n >= 0 && n < BucketSize );
	assert( emptySize >= 0 && emptySize < BucketSize );
	assert( topSize >= n && topSize <= BucketSize );
	assert( Size == BucketSize );
	if( 1 ) {
		// slow:
		for( int i = 0; i < n-1; i++ ) {
			JSObj k1 = keyNode(i).key;
			JSObj k2 = keyNode(i+1).key;
			int z = k1.woCompare(k2);
			assert( z <= 0 );
		}
	}
	else {
		//faster:
		if( n > 1 ) {
			JSObj k1 = keyNode(0).key;
			JSObj k2 = keyNode(n-1).key;
			int z = k1.woCompare(k2);
			assert( z <= 0 );
		}
	}
}

inline int BucketBasics::totalDataSize() const {
	return Size - (data-(char*)this);
}

void BucketBasics::init(){
	parent.Null(); nextChild.Null();
	Size = BucketSize;
	flags = Packed;
	n = 0;
	emptySize = totalDataSize(); topSize = 0;
	reserved = 0;
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

void BucketBasics::del(int keypos) { 
	assert( keypos >= 0 && keypos <= n );
	n--;
	for( int j = keypos; j < n; j++ )
		k(j) = k(j+1);
	emptySize += sizeof(_KeyNode);
	setNotPacked();
}

/* add a key.  must be > all existing.  be careful to set next ptr right. */
void BucketBasics::pushBack(const DiskLoc& recordLoc, JSObj& key, DiskLoc prevChild) { 
	int bytesNeeded = key.objsize() + sizeof(_KeyNode);
	assert( bytesNeeded <= emptySize );
	assert( n == 0 || keyNode(n-1).key.woCompare(key) <= 0 );
	emptySize -= sizeof(_KeyNode);
	_KeyNode& kn = k(n++);
	kn.prevChildBucket = prevChild;
	kn.recordLoc = recordLoc;
	kn.keyDataOfs = (short) _alloc(key.objsize());
	char *p = dataAt(kn.keyDataOfs);
	memcpy(p, key.objdata(), key.objsize());
}

bool BucketBasics::basicInsert(int keypos, const DiskLoc& recordLoc, JSObj& key) {
	assert( keypos >= 0 && keypos <= n );
	int bytesNeeded = key.objsize() + sizeof(_KeyNode);
	if( bytesNeeded > emptySize ) { 
		pack();
		if( bytesNeeded > emptySize )
			return false;
	}
	for( int j = n; j > keypos; j-- ) // make room
		k(j) = k(j-1);
	n++;
	emptySize -= sizeof(_KeyNode);
	_KeyNode& kn = k(keypos);
	kn.prevChildBucket.Null();
	kn.recordLoc = recordLoc;
	kn.keyDataOfs = (short) _alloc(key.objsize());
	char *p = dataAt(kn.keyDataOfs);
	memcpy(p, key.objdata(), key.objsize());
	return true;
}

/* when we delete things we just leave empty space until the node is 
   full and then we repack it.
*/
void BucketBasics::pack() { 
	if( flags & Packed )
		return;

	int tdz = totalDataSize();
	char temp[BucketSize];
	int ofs = tdz;
	topSize = 0;
	for( int j = 0; j < n; j++ ) { 
		short ofsold = k(j).keyDataOfs;
		int sz = keyNode(j).key.objsize();
		ofs -= sz; 
		topSize += sz;
		memcpy(temp+ofs, dataAt(ofsold), sz);
		k(j).keyDataOfs = ofs;
	}
	int dataUsed = tdz - ofs;
	memcpy(data + ofs, temp + ofs, dataUsed);
	emptySize = tdz - dataUsed - n * sizeof(_KeyNode);
	assert( emptySize >= 0 );

	setPacked();
	assertValid();
}

inline void BucketBasics::truncateTo(int N) {
	n = N;
	setNotPacked();
	pack();
}

/* - BtreeBucket --------------------------------------------------- */

/* pos: for existing keys k0...kn-1.
   returns # it goes BEFORE.  so key[pos-1] < key < key[pos]
   returns n if it goes after the last existing key.
*/
bool BtreeBucket::find(JSObj& key, int& pos) { 
	/* binary search for this key */
	int l=0; int h=n-1;
	while( l <= h ) { 
		int m = (l+h)/2;
		KeyNode M = keyNode(m);
		int x = key.woCompare(M.key);
		if( x < 0 ) // key < M.key
			h = m-1;
		else if( x > 0 )
			l = m+1;
		else {
			pos = m;
			return true;
		}
	}
	// not found
	pos = l;
	if( pos != n ) {
		JSObj keyatpos = keyNode(pos).key;
		assert( key.woCompare(keyatpos) <= 0 );
		if( pos > 0 ) { 
			assert( keyNode(pos-1).key.woCompare(key) <= 0 );
		}
	}
	return false;
}

bool BtreeBucket::unindex(JSObj& key ) { 
	int pos;
	if( find(key, pos) ) {
		del(pos);
		assertValid();
		return true;
	}
	DiskLoc l = childForPos(pos);
	if( l.isNull() )
		return false;
	return l.btree()->unindex(key);
}


BtreeBucket* BtreeBucket::allocTemp() { 
	BtreeBucket *b = (BtreeBucket*) malloc(BucketSize);
	b->init();
	return b;
}

void BtreeBucket::insertHere(const DiskLoc& thisLoc, const char *ns, int keypos, 
							 const DiskLoc& recordLoc, JSObj& key,
							 DiskLoc lchild, DiskLoc rchild, IndexDetails& idx) {
	if( basicInsert(keypos, recordLoc, key) ) {
		_KeyNode& kn = k(keypos);
		if( keypos+1 == n ) { // last key
			kn.prevChildBucket = nextChild;
			nextChild = rchild;
			assert( kn.prevChildBucket == lchild );
		}
		else {
			k(keypos).prevChildBucket = lchild;
			assert( k(keypos+1).prevChildBucket == lchild );
			k(keypos+1).prevChildBucket = rchild;
		}
		return;
	}

	// split
	BtreeBucket *r = allocTemp();
	DiskLoc rLoc;
	int mid = n / 2;
	for( int i = mid+1; i < n; i++ ) {
		KeyNode kn = keyNode(i);
		if( i == keypos ) {
			// slip in the new one
			r->pushBack(recordLoc, key, kn.prevChildBucket);
			r->pushBack(kn.recordLoc, kn.key, rchild);
		}
		else
			r->pushBack(kn.recordLoc, kn.key, kn.prevChildBucket);
	}
	r->nextChild = nextChild;
	r->assertValid();
	rLoc = theDataFileMgr.insert(ns, r, r->Size, true);
	free(r); r = 0;

	{
		KeyNode middle = keyNode(mid);
		nextChild = middle.prevChildBucket;

		// promote middle to a parent node
		if( parent.isNull() ) { 
			// make a new parent if we were the root
			BtreeBucket *p = allocTemp();
			p->pushBack(middle.recordLoc, middle.key, thisLoc);
			p->nextChild = rLoc;
			p->assertValid();
			idx.head = theDataFileMgr.insert(ns, p, p->Size, true);
			free(p);
		} 
		else {
			parent.btree()->_insert(parent, ns, middle.recordLoc, middle.key, false, thisLoc, rLoc, idx);
		}
	}

	// mark on left that we no longer have anything from midpoint on.
	truncateTo(mid);  // note this may trash middle.key!  thus we had to promote it before finishing up here.

	// add our new key, there is room now
	{
		if( keypos < mid ) {
			insertHere(thisLoc, ns, keypos, recordLoc, key, lchild, rchild, idx);
		} else {
			// handled above already.
			// int kp = keypos-mid-1; assert(kp>=0);
			//rLoc.btree()->insertHere(rLoc, ns, kp, recordLoc, key, lchild, rchild, idx);
		}
	}
}

DiskLoc BtreeBucket::addHead(const char *ns) {
	BtreeBucket *p = allocTemp();
	DiskLoc loc = theDataFileMgr.insert(ns, p, p->Size, true);
	return loc;
}

DiskLoc BtreeBucket::getHead(const DiskLoc& thisLoc) {
	DiskLoc p = thisLoc;
	while( !p.btree()->isHead() )
		p = p.btree()->parent;
	return p;
}

DiskLoc BtreeBucket::advance(const DiskLoc& thisLoc, int& keyOfs, int direction) {
	assert( keyOfs >= 0 && keyOfs < n );
	int adj = direction < 0 ? 1 : 0;
	int ko = keyOfs + direction;
	DiskLoc nextDown = childForPos(ko+adj);
	if( !nextDown.isNull() ) { 
		while( 1 ) {
			keyOfs = direction>0 ? 0 : nextDown.btree()->n - 1;
			DiskLoc loc= nextDown.btree()->childForPos(keyOfs + adj);
			if( loc.isNull() )
				break;
			nextDown = loc;
		}
		return nextDown;
	}

	if( ko < n && ko >= 0 ) {
		keyOfs = ko;
		return thisLoc;
	}

	// end of bucket.  traverse back up.
	DiskLoc childLoc = thisLoc;
	DiskLoc ancestor = parent;
	while( 1 ) {
		if( ancestor.isNull() )
			break;
		BtreeBucket *an = ancestor.btree();
		for( int i = 0; i < an->n; i++ ) {
			if( an->childForPos(i+adj) == childLoc ) {
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

DiskLoc BtreeBucket::locate(const DiskLoc& thisLoc, JSObj& key, int& pos, bool& found, int direction) { 
	int p;
	found = find(key, p);
	if( found ) {
		pos = p;
		return thisLoc;
	}

	DiskLoc child = childForPos(p);

	if( !child.isNull() ) { 
		DiskLoc l = child.btree()->locate(child, key, pos, found);
		if( !l.isNull() )
			return l;
	}

	if( direction == -1 && p == n && n ) { 
		p--;
	}

	pos = p;
	return pos == n ? DiskLoc() /*theend*/ : thisLoc;
}

/* thisloc is the location of this bucket object.  you must pass that in. */
int BtreeBucket::_insert(const DiskLoc& thisLoc, const char *ns, const DiskLoc& recordLoc, 
						JSObj& key, bool dupsAllowed,
						DiskLoc lChild, DiskLoc rChild, IndexDetails& idx) { 
	if( key.objsize() > KeyMax ) { 
		cout << "ERROR: key too large len:" << key.objsize() << " max:" << KeyMax << endl;
		return 2;
	} assert( key.objsize() > 0 );

	int pos;
	bool found = find(key, pos);
	if( found ) {
		// todo: support dup keys
		cout << "bree: skipping insert of duplicate key ns:" << ns << "keysize:" << key.objsize() << endl;
		return 1;
	}

	DiskLoc& child = getChild(pos);
	if( child.isNull() || !rChild.isNull() ) { 
		insertHere(thisLoc, ns, pos, recordLoc, key, lChild, rChild, idx);
		return 0;
	}

	return child.btree()->insert(child, ns, recordLoc, key, dupsAllowed, idx);
}

void BtreeBucket::dump() { 
	cout << "DUMP btreebucket:\n";
	for( int i = 0; i < n; i++ ) {
		KeyNode k = keyNode(i);
		cout << '\t' << i << '\t' << k.key.toString() << endl;
	}
}

int BtreeBucket::insert(const DiskLoc& thisLoc, const char *ns, const DiskLoc& recordLoc, 
						JSObj& key, bool dupsAllowed, IndexDetails& idx) 
{
	ninserts++;
//	if( ninserts == 0x7cf ) { 
//		dump();
//	}
//	assertValid();
	int x = _insert(thisLoc, ns, recordLoc, key, dupsAllowed, DiskLoc(), DiskLoc(), idx);
	assertValid();
	return x;
}

/* - BtreeCursor --------------------------------------------------- */

BtreeCursor::BtreeCursor(DiskLoc head, JSObj k, int _direction, bool sm) : 
    direction(_direction), stopmiss(sm) 
{
	bool found;
	bucket = head.btree()->locate(head, k, keyOfs, found, direction);
}


DiskLoc BtreeCursor::currLoc() {
	assert( !bucket.isNull() );
	return bucket.btree()->k(keyOfs).recordLoc;
}

bool BtreeCursor::advance() { 
	if( bucket.isNull() )
		return false;
	bucket = bucket.btree()->advance(bucket, keyOfs, direction);
	return !bucket.isNull();
}

void BtreeCursor::noteLocation() {
	if( !eof() ) {
		JSObj o = bucket.btree()->keyAt(keyOfs).copy();
		keyAtKeyOfs = o;
	}
}

/* see if things moved around (deletes, splits, inserts) */
void BtreeCursor::checkLocation() { 
	if( eof() || bucket.btree()->keyAt(keyOfs).woEqual(keyAtKeyOfs) )
		return;
	cout << "  key seems to have moved in the index, refinding it" << endl;
	bool found;
	DiskLoc bold = bucket;
	/* probably just moved in our node, so to be fast start from here rather than the head */
	bucket = bucket.btree()->locate(bucket, keyAtKeyOfs, keyOfs, found, direction);
	if( found || bucket.btree()->isHead() )
		return;
	/* didn't find, check from the top */
	DiskLoc head = bold.btree()->getHead(bold);
	head.btree()->locate(head, keyAtKeyOfs, keyOfs, found);
}
