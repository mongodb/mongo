// btree.cpp

#include "stdafx.h"
#include "btree.h"
#include "pdfile.h"

/* it is easy to do custom sizes for a namespace - all the same for now */
const int BucketSize = 8192;
const int KeyMax = BucketSize / 8;

int ninserts = 0;
extern int otherTraceLevel;

inline KeyNode::KeyNode(BucketBasics& bb, _KeyNode &k) : 
  prevChildBucket(k.prevChildBucket), 
  recordLoc(k.recordLoc), key(bb.data+k.keyDataOfs) 
{ }

/* BucketBasics --------------------------------------------------- */

inline void BucketBasics::setNotPacked() { flags &= ~Packed; }
inline void BucketBasics::setPacked() { flags |= Packed; }

void BucketBasics::_shape(int level, stringstream& ss) {
	for( int i = 0; i < level; i++ ) ss << ' ';
	ss << "*\n";
	for( int i = 0; i < n; i++ ) 
		if( !k(i).prevChildBucket.isNull() )
			k(i).prevChildBucket.btree()->_shape(level+1,ss);
	if( !nextChild.isNull() )
		nextChild.btree()->_shape(level+1,ss);
}

void BucketBasics::fullValidate(const DiskLoc& thisLoc) { 
	assertValid();
	for( int i = 0; i < n; i++ ) {
		_KeyNode& kn = k(i);
		if( !kn.prevChildBucket.isNull() ) { 
			DiskLoc left = kn.prevChildBucket;
			BtreeBucket *b = left.btree();
			assert( b->parent == thisLoc );
			b->fullValidate(kn.prevChildBucket);
		}
	}
	if( !nextChild.isNull() ) {
		BtreeBucket *b = nextChild.btree();
		assert( b->parent == thisLoc );
		b->fullValidate(nextChild);
	}
}

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
			if( z > 0 ) {
				cout << "ERROR: btree key order corrupt.  Keys:" << endl;
				for( int j = 0; j < n; j++ ) { 
					cout << "  " << keyNode(j).key.toString() << endl;
				}
				assert(false);
			}
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

inline void BucketBasics::markUnused(int keypos) { 
	assert( keypos >= 0 && keypos < n );
	k(keypos).setUnused();
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

void BucketBasics::_delKeyAtPos(int keypos) { 
	assert( keypos >= 0 && keypos <= n );
	assert( childForPos(keypos).isNull() );
	n--;
	assert( n > 0 || nextChild.isNull() );
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
	kn.setKeyDataOfs( (short) _alloc(key.objsize()) );
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
	kn.setKeyDataOfs((short) _alloc(key.objsize()) );
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
		k(j).setKeyDataOfs( ofs );
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
   note result might be Unused!
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
			// found it.  however, if dup keys are here, be careful we might have
			// found one in the middle.  we want find() to return the leftmost instance.
			while( m >= 1 && keyNode(m-1).key.woEqual(key) )
				m--;
			pos = m;
			return true;
		}
		x = key.woCompare(M.key);
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

void BtreeBucket::delBucket(const DiskLoc& thisLoc, const char *ns) { 
	assert( !isHead() );

	BtreeBucket *p = parent.btree();
	if( p->nextChild == thisLoc ) {
		p->nextChild.Null();
	}
	else {
		for( int i = 0; i < p->n; i++ ) {
			if( p->k(i).prevChildBucket == thisLoc ) { 
				p->k(i).prevChildBucket.Null();
				goto found;
			}
		}
		cout << "ERROR: can't find ref to deleted bucket.\n";
		cout << "To delete:\n";
		dump();
		cout << "Parent:\n";
		p->dump();
		assert(false);
	}
found:
	//defensive:
	n = -1;
	parent.Null();
	theDataFileMgr.deleteRecord(ns, thisLoc.rec(), thisLoc);
}

/* note: may delete the entire bucket!  this invalid upon return sometimes. */
void BtreeBucket::delKeyAtPos(const DiskLoc& thisLoc, const char *ns, int p) { 
	assert(n>0);
	DiskLoc left = childForPos(p);

	if( n == 1 ) {
		if( left.isNull() && nextChild.isNull() ) { 
			if( isHead() )
				_delKeyAtPos(p); // we don't delete the top bucket ever
			else
				delBucket(thisLoc, ns);
			return;
		}
		markUnused(p);
		return;
	}

	if( left.isNull() )
		_delKeyAtPos(p);
	else
		markUnused(p);
}

int verbose = 0;
int qqq = 0;

bool BtreeBucket::unindex(const DiskLoc& thisLoc, const char *ns, JSObj& key, const DiskLoc& recordLoc ) { 
	cout << "unindex " << key.toString() << endl;

	BtreeCursor c(thisLoc, key, 1, true);

//	dump();

	while( c.ok() ) { 
		KeyNode kn = c.currKeyNode();
		if( !kn.key.woEqual(key) )
			break;
		if( recordLoc == kn.recordLoc ) {
			if( verbose )
				c.bucket.btree()->dump();
			c.bucket.btree()->delKeyAtPos(c.bucket, ns, c.keyOfs);
			return true;
		}
		c.advance();
	}

	return false;
}

BtreeBucket* BtreeBucket::allocTemp() { 
	BtreeBucket *b = (BtreeBucket*) malloc(BucketSize);
	b->init();
	return b;
}

inline void fix(const DiskLoc& thisLoc, const DiskLoc& child) { 
	if( !child.isNull() )
		child.btree()->parent = thisLoc;
}

/* this sucks.  maybe get rid of parent ptrs. */
void BtreeBucket::fixParentPtrs(const DiskLoc& thisLoc) { 
	fix(thisLoc, nextChild);
	for( int i = 0; i < n; i++ )
		fix(thisLoc, k(i).prevChildBucket);
}

/* keypos - where to insert the key i3n range 0..n.  0=make leftmost, n=make rightmost.
*/
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
	cout << "split begin " << hex << thisLoc.getOfs() << dec << endl;
	BtreeBucket *r = allocTemp();
	DiskLoc rLoc;
	int mid = n / 2;
	cout << "  mid:" << mid << " n:" << n << endl;
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
	cout << "  new rLoc:" << hex << rLoc.getOfs() << dec << endl;
	free(r); r = 0;
	rLoc.btree()->fixParentPtrs(rLoc);

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
			parent = idx.head = theDataFileMgr.insert(ns, p, p->Size, true);
			cout << "  we were root, making new root:" << hex << parent.getOfs() << dec << endl;
			free(p);
		} 
		else {
			cout << "  promoting middle key" << endl;
			parent.btree()->_insert(parent, ns, middle.recordLoc, middle.key, false, thisLoc, rLoc, idx);
		}
		rLoc.btree()->parent = parent;
	}

	// mark on left that we no longer have anything from midpoint on.
    bool highest = keypos == n;
	truncateTo(mid);  // note this may trash middle.key!  thus we had to promote it before finishing up here.

	// add our new key, there is room now
	{
		if( keypos < mid ) {
			cout << "  keypos<mid, insertHere() the new key" << endl;
			insertHere(thisLoc, ns, keypos, recordLoc, key, lchild, rchild, idx);
		} else if( highest ) {
			// else handled above already.
			int kp = keypos-mid-1; assert(kp>=0);
			rLoc.btree()->insertHere(rLoc, ns, kp, recordLoc, key, lchild, rchild, idx);
		}
	}

	cout << "split end " << hex << thisLoc.getOfs() << dec << endl;
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
		DiskLoc l = child.btree()->locate(child, key, pos, found, direction);
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
		// on a dup key always insert on the right or else you will be broken.
		pos++;
		// todo: support unique keys.
		/*
		cout << "bree: skipping insert of duplicate key ns:" << ns << "keysize:" << key.objsize() << endl;
		return 1;
		*/
	}

	DiskLoc& child = getChild(pos);
	if( child.isNull() || !rChild.isNull() ) { 
		insertHere(thisLoc, ns, pos, recordLoc, key, lChild, rChild, idx);
		return 0;
	}

	return child.btree()->insert(child, ns, recordLoc, key, dupsAllowed, idx);
}

void BtreeBucket::dump() { 
	cout << "DUMP btreebucket: ";
	cout << " parent:" << hex << parent.getOfs() << dec;
	for( int i = 0; i < n; i++ ) {
		cout << '\n';
		KeyNode k = keyNode(i);
		cout << '\t' << i << '\t' << k.key.toString() << "\tleft:" << hex << 
			k.prevChildBucket.getOfs() << "\trec:" << k.recordLoc.getOfs() << dec;
		if( this->k(i).isUnused() )
			cout << " UNUSED";
	}
	cout << " right:" << hex << nextChild.getOfs() << dec << endl;
}

int BtreeBucket::insert(const DiskLoc& thisLoc, const char *ns, const DiskLoc& recordLoc, 
						JSObj& key, bool dupsAllowed, IndexDetails& idx) 
{
	int x = _insert(thisLoc, ns, recordLoc, key, dupsAllowed, DiskLoc(), DiskLoc(), idx);
	assertValid(); 

#if defined(_WIN32)
	if( isHead() ) {
		ninserts++;
		fullValidate(thisLoc);
	}
#endif

	return x;
}

void BtreeBucket::shape(stringstream& ss) {
	_shape(0, ss);
}

/* - BtreeCursor --------------------------------------------------- */

BtreeCursor::BtreeCursor(DiskLoc head, JSObj& k, int _direction, bool sm) : 
    direction(_direction), stopmiss(sm) 
{
	bool found;
	if( otherTraceLevel >= 12 ) {
		cout << "BTreeCursor(). dumping head bucket" << endl;
		head.btree()->dump();
	}
	bucket = head.btree()->locate(head, k, keyOfs, found, direction);
	checkUnused();
}

/* skip unused keys. */
void BtreeCursor::checkUnused() {
	int u = 0;
	while( 1 ) {
		if( !ok() ) 
			break;
		BtreeBucket *b = bucket.btree();
		_KeyNode& kn = b->k(keyOfs);
		if( kn.isUsed() )
			break;
		bucket = b->advance(bucket, keyOfs, direction);
		u++;
	}
	if( u > 10 )
		cout << "btree unused skipped:" << u << endl;
}

/*DiskLoc BtreeCursor::currLoc() {
	assert( !bucket.isNull() );
	_KeyNode& kn = bucket.btree()->k(keyOfs);
	assert( kn.isUsed() );
	return kn.recordLoc;
}*/

bool BtreeCursor::advance() { 
	if( bucket.isNull() )
		return false;
	bucket = bucket.btree()->advance(bucket, keyOfs, direction);
	checkUnused();
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
