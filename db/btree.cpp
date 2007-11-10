// btree.cpp

#include "stdafx.h"
#include "btree.h"
#include "pdfile.h"

/* it is easy to do custom sizes for a namespace - all the same for now */
const int BucketSize = 8192;
const int KeyMax = BucketSize / 8;

inline KeyNode::KeyNode(BucketBasics& bb, _KeyNode &k) : 
  prevChildBucket(k.prevChildBucket), recordLoc(k.recordLoc), key(bb.data+k.keyDataOfs) { }

/* - BucketBasics --------------------------------------------------- */

inline void BucketBasics::setNotPacked() { flags &= ~Packed; }
inline void BucketBasics::setPacked() { flags |= Packed; }

int BucketBasics::totalDataSize() const {
	return Size - (data-(char*)this);
}

void BucketBasics::init(){
	parent.Null(); nextChild.Null();
	Size = BucketSize;
	flags = Packed;
	n = 0;
	emptySize = totalDataSize();
	reserved = 0;
}

/* we allocate space from the end of the buffer for data.
   the keynodes grow from the front.
*/
inline int BucketBasics::_alloc(int bytes) {
	int ofs = emptySize - bytes;
	assert( ofs >= 0 );
	emptySize -= bytes;
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

/* add a key.  must be < all existing */
void BucketBasics::pushFront(const DiskLoc& recordLoc, JSObj& key, DiskLoc prevChild) { 
	int bytesNeeded = key.objsize() + sizeof(_KeyNode);
	assert( bytesNeeded <= emptySize );
	for( int j = n; j > 0; j-- ) // make room
		k(j) = k(j-1);
	n++;
	emptySize -= sizeof(_KeyNode);
	_KeyNode& kn = k(0);
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

	int keysz = n * sizeof(_KeyNode);
	int left = totalDataSize() - keysz;
	for( int j = n-1; j >= 0; j++ ) {
		short ofsold = k(j).keyDataOfs;
		int sz = keyNode(j).key.objsize();
		short ofsnew = keysz + left - sz;
		if( ofsold != ofsnew ) {
			memmove(dataAt(ofsnew), dataAt(ofsold), sz);
			k(j).keyDataOfs = ofsnew;
		}
		left -= sz;
	}
	assert(left>=0);
	emptySize = left;

	setPacked();
}

inline void BucketBasics::truncateTo(int N) {
	n = N;
	int sz = 0;
	for( int i = 0; i < n; i++ )
		sz += sizeof(_KeyNode) + keyNode(i).key.objsize();
	emptySize = totalDataSize() - sz;
	assert( emptySize >= 0 );
}

/* - BtreeBucket --------------------------------------------------- */

/* pos: for existing keys k0...kn-1.
   returns # it goes BEFORE.  so pos=0 -> newkey<k0.
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
	return false;
}

BtreeBucket* BtreeBucket::allocTemp() { 
	BtreeBucket *b = (BtreeBucket*) malloc(BucketSize);
	b->init();
	return b;
}

void BtreeBucket::insertHere(const DiskLoc& thisLoc, const char *ns, int keypos, 
							 const DiskLoc& recordLoc, JSObj& key,
							 DiskLoc lchild, DiskLoc rchild) {
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

	// split!
	BtreeBucket *r = allocTemp();
	DiskLoc rLoc;
	int mid = n / 2;
	for( int i = mid+1; i < n; i++ ) {
		KeyNode kn = keyNode(i);
		r->pushFront(kn.recordLoc, kn.key, kn.prevChildBucket);
	}
	rLoc = theDataFileMgr.insert(ns, r, r->Size, true);
	free(r); r = 0;
	KeyNode middle = keyNode(mid);
	truncateTo(mid); // mark on left that we no longer have anything from midpoint on.
	nextChild = middle.prevChildBucket;

	// add our new key, there is room now
	{
		if( keypos < mid ) {
			insertHere(thisLoc, ns, keypos, recordLoc, key, lchild, rchild);
		} else {
			int kp = keypos-mid-1; assert(kp>=0);
			insertHere(rLoc, ns, kp, recordLoc, key, lchild, rchild);
		}
	}

	// promote middle to a parent node
	{
		if( parent.isNull() ) { 
			// make a new parent if we were the root
			BtreeBucket *p = allocTemp();
			p->pushFront(middle.recordLoc, middle.key, thisLoc);
			p->nextChild = rLoc;
			theDataFileMgr.insert(ns, p, p->Size, true);
			free(p);
			// set location of new head! xxx
		} 
		else {
			parent.btree()->_insert(parent, ns, middle.recordLoc, middle.key, false, thisLoc, rLoc);
		}
	}
}

DiskLoc BtreeBucket::addHead(const char *ns) {
	BtreeBucket *p = allocTemp();
	DiskLoc loc = theDataFileMgr.insert(ns, p, p->Size, true);
	return loc;
}

DiskLoc BtreeBucket::advance(const DiskLoc& thisLoc, int& keyOfs) {
	assert( keyOfs >= 0 && keyOfs < n );

	int ko = keyOfs + 1;
	DiskLoc nextDown = ko==n ? nextChild : k(ko).prevChildBucket;
	if( !nextDown.isNull() ) { 
		keyOfs = 0;
		return nextDown;
	}

	if( ko < n ) {
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
			if( an->k(i).prevChildBucket == childLoc ) { 
				keyOfs = i;
				return ancestor;
			}
		}
		assert( an->nextChild == childLoc );
		// parent exhausted also, keep going up
		childLoc = ancestor;
		ancestor = an->parent;
	}

	return DiskLoc();
}

DiskLoc BtreeBucket::locate(const DiskLoc& thisLoc, JSObj& key, int& pos, bool& found) { 
	int p;
	found = find(key, p);
	if( found ) {
		pos = p;
		return thisLoc;
	}

	DiskLoc child = p == n ? nextChild : k(p).prevChildBucket;

	if( !child.isNull() ) { 
		DiskLoc l = child.btree()->locate(child, key, pos, found);
		if( !l.isNull() )
			return l;
	}

	pos = p;
	return pos == n ? DiskLoc() /*theend*/ : thisLoc;
}

/* thisloc is the location of this bucket object.  you must pass that in. */
int BtreeBucket::_insert(const DiskLoc& thisLoc, const char *ns, const DiskLoc& recordLoc, 
						JSObj& key, bool dupsAllowed,
						DiskLoc lChild, DiskLoc rChild) { 
	if( key.objsize() > KeyMax ) { 
		cout << "ERROR: key too large len:" << key.objsize() << " max:" << KeyMax << endl;
		return 2;
	}

	int pos;
	bool found = find(key, pos);
	if( found ) {
		// todo: support dup keys
		cout << "dup key failing" << endl;
		return 1;
	}

	DiskLoc& child = getChild(pos);
	if( child.isNull() || !rChild.isNull() ) { 
		insertHere(thisLoc, ns, pos, recordLoc, key, lChild, rChild);
		return 0;
	}

	return child.btree()->insert(child, ns, recordLoc, key, dupsAllowed);
}

int BtreeBucket::insert(const DiskLoc& thisLoc, const char *ns, const DiskLoc& recordLoc, 
						JSObj& key, bool dupsAllowed) 
{
	return _insert(thisLoc, ns, recordLoc, key, dupsAllowed, DiskLoc(), DiskLoc());
}

/* - BtreeCursor --------------------------------------------------- */

BtreeCursor::BtreeCursor(DiskLoc head, JSObj k, bool sm) : stopmiss(sm) {
	bool found;
	bucket = head.btree()->locate(head, k, keyOfs, found);
}


DiskLoc BtreeCursor::currLoc() {
	assert( !bucket.isNull() );
	return bucket.btree()->k(keyOfs).recordLoc;
}

bool BtreeCursor::advance() { 
	if( bucket.isNull() )
		return false;
	bucket = bucket.btree()->advance(bucket, keyOfs);
	return !bucket.isNull();
}
