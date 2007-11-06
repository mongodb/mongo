// pdfile.cpp

/* 
todo: 
_ manage deleted records.  bucket?
_ use deleted on inserts!
_ quantize allocations
_ table scans must be sequential, not next/prev pointers
_ regex support
*/

#include "stdafx.h"
#include "pdfile.h"
#include "db.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"

DataFileMgr theDataFileMgr;

/* just temporary */
const int ExtentSize = 1 * 1024 * 1024;

JSObj::JSObj(Record *r) { 
	_objdata = r->data;
	_objsize = *((int*) _objdata);
	assert( _objsize <= r->netLength() );
	iFree = false;
}

/*---------------------------------------------------------------------*/ 

int bucketSizes[] = { 
	32, 64, 128, 256, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000,
	0x8000, 0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000,
	0x400000, 0x800000
};
const int Buckets = 19;
const int MaxBucket = 18;

class NamespaceDetails {
public:
	NamespaceDetails() { memset(reserved, 0, sizeof(reserved)); }
	DiskLoc firstExtent;
	DiskLoc deletedList[Buckets];
	char reserved[256];

	static int bucket(int n) { 
		for( int i = 0; i < Buckets; i++ )
			if( bucketSizes[i] > n )
				return i;
		return Buckets-1;
	}

	void addDeletedRec(DeletedRecord *d, DiskLoc dloc) { 
		int b = bucket(d->lengthWithHeaders);
		DiskLoc& list = deletedList[b];
		DiskLoc oldHead = list;
		list = dloc;
		d->nextDeleted = oldHead;
	}

	DiskLoc alloc(int lenToAlloc, DiskLoc& extentLoc);

private:
	DiskLoc _alloc(int len);
};

DiskLoc NamespaceDetails::alloc(int lenToAlloc, DiskLoc& extentLoc) {
	lenToAlloc = (lenToAlloc + 3) & 0xfffffffc;
	DiskLoc loc = _alloc(lenToAlloc);
	if( loc.isNull() )
		return loc;

	DeletedRecord *r = loc.drec();

	/* note we want to grab from the front so our next pointers on disk tend
	to go in a forward direction which is important for performance. */
	int regionlen = r->lengthWithHeaders;
	extentLoc.set(loc.a(), r->extentOfs);

	int left = regionlen - lenToAlloc;
	if( left < 24 ) {
		// you get the whole thing.
		return loc;
	}

	/* split off some for further use. */
	r->lengthWithHeaders = lenToAlloc;
	DiskLoc newDelLoc = loc;
	newDelLoc.inc(lenToAlloc);
	DeletedRecord *newDel = newDelLoc.drec();
	newDel->extentOfs = r->extentOfs;
	newDel->lengthWithHeaders = left;
	newDel->nextDeleted.Null();
	addDeletedRec(newDel, newDelLoc);

	return loc;
}

/* returned item is out of the deleted list upon return */
DiskLoc NamespaceDetails::_alloc(int len) {
	DiskLoc *prev;
	DiskLoc *bestprev = 0;
	DiskLoc bestmatch;
	int bestmatchlen = 0x7fffffff;
	int b = bucket(len);
	DiskLoc cur = deletedList[b]; prev = &deletedList[b];
	int extra = 5; // look for a better fit, a little.
	int chain = 0;
	while( 1 ) { 
		if( cur.isNull() ) { 
			// move to next bucket.  if we were doing "extra", just break
			if( bestmatchlen < 0x7fffffff )
				break;
			b++;
			if( b > MaxBucket ) {
				// out of space. alloc a new extent.
				return DiskLoc();
			}
			cur = deletedList[b]; prev = &deletedList[b];
			continue;
		}
		DeletedRecord *r = cur.drec();
		if( r->lengthWithHeaders >= len && 
			r->lengthWithHeaders < bestmatchlen ) {
				bestmatchlen = r->lengthWithHeaders;
				bestmatch = cur;
				bestprev = prev;
		}
		if( bestmatchlen < 0x7fffffff && --extra <= 0 )
			break;
		if( ++chain > 30 && b < MaxBucket ) {
			// too slow, force move to next bucket to grab a big chunk
			b++;
			chain = 0;
			cur.Null();
		}
		else {
			cur = r->nextDeleted; prev = &r->nextDeleted;
		}
	}

	/* unlink ourself from the deleted list */
	*bestprev = bestmatch.drec()->nextDeleted;

	return bestmatch;
}

class NamespaceIndex {
	friend class NamespaceCursor;
public:
	NamespaceIndex() { }

	void init() { 
		const int LEN = 16 * 1024 * 1024;
		void *p = f.map("/data/db/namespace.idx", LEN);
		if( p == 0 ) { 
			cout << "couldn't open /data/db/namespace.idx" << endl;
			exit(-3);
		}
		ht = new HashTable<Namespace,NamespaceDetails>(p, LEN, "namespace index");
	}

	void add(const char *ns, DiskLoc& loc) { 
		Namespace n(ns);
		NamespaceDetails details;
		details.firstExtent = loc;
		ht->put(n, details);
	}

	NamespaceDetails* details(const char *ns) { 
		Namespace n(ns);
		return ht->get(n); 
	}

	bool find(const char *ns, DiskLoc& loc) { 
		NamespaceDetails *l = details(ns);
		if( l ) {
			loc = l->firstExtent;
			return true;
		}
		return false;
	}

private:
	MemoryMappedFile f;
	HashTable<Namespace,NamespaceDetails> *ht;
} namespaceIndex;

class NamespaceCursor : public Cursor {
public:
	virtual bool ok() { return i >= 0; }
	virtual Record* _current() { assert(false); return 0; }
	virtual DiskLoc currLoc() { assert(false); return DiskLoc(); }

	virtual JSObj current() {
		NamespaceDetails &d = namespaceIndex.ht->nodes[i].value;
		JSObjBuilder b;
		b.append("name", namespaceIndex.ht->nodes[i].k.buf);
		return b.done();
	}

	virtual bool advance() {
		while( 1 ) {
			i++;
			if( i >= namespaceIndex.ht->n )
				break;
			if( namespaceIndex.ht->nodes[i].inUse() )
				return true;
		}
		i = -1000000;
		return false;
	}

	NamespaceCursor() { 
		i = -1;
		advance();
	}
private:
	int i;
};

auto_ptr<Cursor> makeNamespaceCursor() {
	return auto_ptr<Cursor>(new NamespaceCursor());
}

void newNamespace(const char *ns) {
	cout << "New namespace: " << ns << endl;
	if( strcmp(ns, "system.namespaces") != 0 ) {
		JSObjBuilder b;
		b.append("name", ns);
		JSObj j = b.done();
		theDataFileMgr.insert("system.namespaces", j.objdata(), j.objsize(), true);
	}
}

/*---------------------------------------------------------------------*/ 

void PhysicalDataFile::open(const char *filename, int length) {
	header = (PDFHeader *) mmf.map(filename, length);
	assert(header);
	header->init(length);
}

/* prev - previous extent for this namespace.  null=this is the first one. */
Extent* PhysicalDataFile::newExtent(const char *ns) {
	DiskLoc loc;
	int left = header->unusedLength - ExtentSize;
	if( left < 0 ) {
		cout << "ERROR: newExtent: no more room for extents. write more code" << endl;
		assert(false);
		exit(2);
	}
	int offset = header->unused.getOfs();
	header->unused.setOfs( offset + ExtentSize );
	header->unusedLength -= ExtentSize;
	loc.setOfs(offset);
	Extent *e = _getExtent(loc);
	DiskLoc emptyLoc = e->init(ns, ExtentSize, offset);

	DiskLoc oldExtentLoc;
	if( namespaceIndex.find(ns, oldExtentLoc) ) {
		Extent *old = oldExtentLoc.ext();
		assert( old->xprev.isNull() );
		old->xprev = loc;
		e->xnext = oldExtentLoc;
		namespaceIndex.details(ns)->firstExtent = loc;
	}
	else {
		namespaceIndex.add(ns, loc);
	}

	namespaceIndex.details(ns)->addDeletedRec(emptyLoc.drec(), emptyLoc);

	return e;
}

/*---------------------------------------------------------------------*/ 

/* assumes already zeroed -- insufficient for block 'reuse' perhaps */
DiskLoc Extent::init(const char *nsname, int _length, int _offset) { 
	magic = 0x41424344;
	myLoc.setOfs(_offset);
	xnext.Null(); xprev.Null();
	ns = nsname;
	length = _length;
	firstRecord.Null(); lastRecord.Null();

	DiskLoc emptyLoc = myLoc;
	emptyLoc.inc( (extentData-(char*)this) );

	DeletedRecord *empty1 = (DeletedRecord *) extentData;
	DeletedRecord *empty = (DeletedRecord *) getRecord(emptyLoc);
	assert( empty == empty1 );
	empty->lengthWithHeaders = _length - (extentData - (char *) this);
	empty->extentOfs = myLoc.getOfs();
	return emptyLoc;
}

/*
Record* Extent::newRecord(int len) {
	if( firstEmptyRegion.isNull() )
		return 0;

	assert(len > 0);
	int newRecSize = len + Record::HeaderSize;
	DiskLoc newRecordLoc = firstEmptyRegion;
	Record *r = getRecord(newRecordLoc);
	int left = r->netLength() - len;
	if( left < 0 ) {
	//
		firstEmptyRegion.Null();
		return 0;
	}

	DiskLoc nextEmpty = r->next.getNextEmpty(firstEmptyRegion);
	r->lengthWithHeaders = newRecSize;
	r->next.markAsFirstOrLastInExtent(this); // we're now last in the extent
	if( !lastRecord.isNull() ) {
		assert(getRecord(lastRecord)->next.lastInExtent()); // it was the last one
		getRecord(lastRecord)->next.set(newRecordLoc); // until now
		r->prev.set(lastRecord);
	}
	else {
		r->prev.markAsFirstOrLastInExtent(this); // we are the first in the extent
		assert( firstRecord.isNull() );
		firstRecord = newRecordLoc;
	}
	lastRecord = newRecordLoc;

	if( left < Record::HeaderSize + 32 ) { 
		firstEmptyRegion.Null();
	}
	else {
		firstEmptyRegion.inc(newRecSize);
		Record *empty = getRecord(firstEmptyRegion);
		empty->next.set(nextEmpty); // not for empty records, unless in-use records, next and prev can be null.
		empty->prev.Null();
		empty->lengthWithHeaders = left;
	}

	return r;
}
*/

/*---------------------------------------------------------------------*/ 

auto_ptr<Cursor> DataFileMgr::findAll(const char *ns) {
	DiskLoc loc;
	bool found = namespaceIndex.find(ns, loc);
	if( !found ) {
		cout << "info: findAll() namespace does not exist: " << ns << endl;
		return auto_ptr<Cursor>(new BasicCursor(DiskLoc()));
	}
	Extent *e = temp.getExtent(loc);
	return auto_ptr<Cursor>(new BasicCursor( e->firstRecord ));
}

void aboutToDelete(const DiskLoc& dl);

void DataFileMgr::deleteRecord(const char *ns, Record *todelete, const DiskLoc& dl) 
{
	/* check if any cursors point to us.  if so, advance them. */
	aboutToDelete(dl);

	/* remove ourself from the record next/prev chain */
	{
		if( todelete->prevOfs != DiskLoc::NullOfs )
			todelete->getPrev(dl).rec()->nextOfs = todelete->nextOfs;
		if( todelete->nextOfs != DiskLoc::NullOfs )
			todelete->getNext(dl).rec()->prevOfs = todelete->prevOfs;
	}

	/* remove ourself from extent pointers */
	{
		Extent *e = todelete->myExtent(dl);
		if( e->firstRecord == dl )
			e->firstRecord.setOfs(todelete->nextOfs);
		if( e->lastRecord == dl )
			e->lastRecord.setOfs(todelete->prevOfs);
	}

	/* add to the free list */
	{
		NamespaceDetails* d = namespaceIndex.details(ns);
		d->addDeletedRec((DeletedRecord*)todelete, dl);
	}
}

/** Note: as written so far, if the object shrinks a lot, we don't free up space. */
void DataFileMgr::update(
		const char *ns,
		Record *toupdate, const DiskLoc& dl,
		const char *buf, int len) 
{
	if( toupdate->netLength() < len ) { 
		cout << "temp: update: moving record to a larger location " << ns << endl;
		// doesn't fit.
		deleteRecord(ns, toupdate, dl);
		insert(ns, buf, len);
		return;
	}

	memcpy(toupdate->data, buf, len);
}

void DataFileMgr::insert(const char *ns, const void *buf, int len, bool god) {
	if( strncmp(ns, "system.", 7) == 0 && !god ) { 
		cout << "ERROR: attempt to insert in system namespace " << ns << endl;
		return;
	}

	NamespaceDetails *d = namespaceIndex.details(ns);
	if( d == 0 ) {
		newNamespace(ns);
		temp.newExtent(ns);
		d = namespaceIndex.details(ns);
	}

	DiskLoc extentLoc;
	int lenWHdr = len + Record::HeaderSize;
	DiskLoc loc = d->alloc(lenWHdr, extentLoc);
	if( loc.isNull() ) {
		// out of space
		cout << "allocating new extent for " << ns << endl;
		temp.newExtent(ns);
		loc = d->alloc(lenWHdr, extentLoc);
		if( loc.isNull() ) { 
			cout << "ERROR: out of space in datafile.  write more code." << endl;
			assert(false);
			return;
		}
	}

	Record *r = loc.rec();
	assert( r->lengthWithHeaders >= lenWHdr );
	memcpy(r->data, buf, len);
	Extent *e = r->myExtent(loc);
	if( e->lastRecord.isNull() ) { 
		e->firstRecord = e->lastRecord = loc;
		r->prevOfs = r->nextOfs = DiskLoc::NullOfs;
	}
	else {
		Record *oldlast = e->lastRecord.rec();
		r->prevOfs = e->lastRecord.getOfs();
		r->nextOfs = DiskLoc::NullOfs;
		oldlast->nextOfs = loc.getOfs();
		e->lastRecord = loc;
	}
}

void DataFileMgr::init() {
	temp.open("/data/db/temp.dat", 64 * 1024 * 1024);
}

void pdfileInit() {
	namespaceIndex.init();
	theDataFileMgr.init();
}
