// pdfile.cpp

/* 
todo: 
_ manage deleted records.  bucket?
_ use deleted on inserts!
_ quantize allocations
*/

#include "stdafx.h"
#include "pdfile.h"
#include "db.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"

#include <map>
#include <string>

DataFileMgr theDataFileMgr;

/* just temporary */
const int ExtentSize = 1 * 1024 * 1024;

/*---------------------------------------------------------------------*/ 

int bucketSizes[] = { 
	32, 64, 128, 256, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000,
	0x8000, 0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000,
	0x400000, 0x800000
};
const int Buckets = 19;

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

	void addDeletedRec(Record *d, DiskLoc dloc) { 
		int b = bucket(d->lengthWithHeaders);
		DiskLoc& list = deletedList[b];
		DiskLoc oldHead = list;
		list = dloc;
		d->nextDeleted() = oldHead;
	}
};

class NamespaceIndex {
public:
	NamespaceIndex() { }

	void init() { 
		const int LEN = 16 * 1024 * 1024;
		void *p = f.map("/data/namespace.idx", LEN);
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

/*---------------------------------------------------------------------*/ 

void PhysicalDataFile::open(const char *filename, int length) {
	header = (PDFHeader *) mmf.map(filename, length);
	assert(header);
	header->init(length);
}

/* prev - previous extent for this namespace.  null=this is the first one. */
Extent* PhysicalDataFile::newExtent(const char *ns, DiskLoc& loc, Extent *prev) {
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
	e->init(ns, ExtentSize, offset);
	if( prev ) {
		assert( prev->xnext.isNull() );
		prev->xnext = e->myLoc;
		e->xprev = prev->myLoc;
	} else {
		e->xprev.Null();
	}
	e->xnext.Null();
	return e;
}

/*---------------------------------------------------------------------*/ 

/* assumes already zeroed -- insufficient for block 'reuse' perhaps */
void Extent::init(const char *nsname, int _length, int _offset) { 
	magic = 0x41424344;
	myLoc.setOfs(_offset);
	xnext.Null(); xprev.Null();
	ns = nsname;
	length = _length;
	firstRecord.Null(); lastRecord.Null();

	firstEmptyRegion = myLoc;
	firstEmptyRegion.inc( (extentData-(char*)this) );

	Record *empty1 = (Record *) extentData;
	Record *empty = getRecord(firstEmptyRegion);
	assert( empty == empty1 );
	empty->lengthWithHeaders = _length - (extentData - (char *) this);
	empty->next.Null();
}

Record* Extent::newRecord(int len) {
	if( firstEmptyRegion.isNull() )
		return 0;

	assert(len > 0);
	int newRecSize = len + Record::HeaderSize;
	DiskLoc newRecordLoc = firstEmptyRegion;
	Record *r = getRecord(newRecordLoc);
	int left = r->netLength() - len;
	if( left < 0 ) {
		/* this might be wasteful if huge variance in record sizes in a namespace */
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

/*---------------------------------------------------------------------*/ 

Cursor DataFileMgr::findAll(const char *ns) {
	DiskLoc loc;
	bool found = namespaceIndex.find(ns, loc);
	if( !found ) {
		cout << "info: findAll() namespace does not exist: " << ns << endl;
		return Cursor(DiskLoc());
	}
	Extent *e = temp.getExtent(loc);
	return Cursor( e->firstRecord );
}

void DataFileMgr::deleteRecord(const char *ns, Record *todelete, const DiskLoc& dl) {
	/* remove ourself from the record next/prev chain */
	DiskLoc prev = todelete->prev.getPrev(dl);
	if( !prev.isNull() )
		getRecord(prev)->next.set( todelete->next.getNext(dl) );
	DiskLoc next = todelete->next.getNext(dl);
	if( !next.isNull() )
		getRecord(next)->prev.set( todelete->prev.getPrev(dl) );

	/* remove ourself from extent pointers */
	DiskLoc ext = todelete->prev.myExtent(dl);
	if( !ext.isNull() ) {
		// we are first.
		Extent *e = DataFileMgr::getExtent(ext);
		assert( e->firstRecord == dl );
		e->firstRecord = next;
	}
	ext = todelete->next.myExtent(dl);
	if( !ext.isNull() ) {
		Extent *e = DataFileMgr::getExtent(ext);
		assert( e->lastRecord == dl );
		e->lastRecord = next;
	}

	NamespaceDetails* d = namespaceIndex.details(ns);
	d->addDeletedRec(todelete, dl);
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

void DataFileMgr::insert(const char *ns, const void *buf, int len) {
	DiskLoc loc;
	bool found = namespaceIndex.find(ns, loc);
	if( !found ) {
		cout << "New namespace: " << ns << endl;
		temp.newExtent(ns, loc, 0);
		namespaceIndex.add(ns, loc);
	}
	Extent *e = temp.getExtent(loc);
	Record *r = e->newRecord(len); /*todo: if zero returned, need new extent */
	memcpy(r->data, buf, len);
}

void DataFileMgr::init() {
	temp.open("/data/temp.dat", 64 * 1024 * 1024);
}

void pdfileInit() {
	namespaceIndex.init();
	theDataFileMgr.init();
}
