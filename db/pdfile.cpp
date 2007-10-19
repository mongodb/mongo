// pdfile.cpp

#include "stdafx.h"
#include "pdfile.h"
#include "db.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"

#include <map>
#include <string>

DataFileMgr theDataFileMgr;

/* just temporary */
const int ExtentSize = 1024 * 1024;

/*---------------------------------------------------------------------*/ 

class NamespaceIndex {
public:
	NamespaceIndex() { }

	void init() { 
		const int LEN = 16 * 1024 * 1024;
		void *p = f.map("/data/namespace.idx", LEN);
		ht = new HashTable<Namespace,DiskLoc>(p, LEN, "namespace index");
	}

	void add(const char *ns, DiskLoc& loc) { 
		Namespace n(ns);
		ht->put(n, loc);
	}

	bool find(const char *ns, DiskLoc& loc) { 
		Namespace n(ns);
		DiskLoc *l = ht->get(n);
		if( l ) {
			loc = *l;
			return true;
		}
		return false;
	}

private:
	MemoryMappedFile f;
	HashTable<Namespace,DiskLoc> *ht;
} namespaceIndex;

/*---------------------------------------------------------------------*/ 

void PhysicalDataFile::open(const char *filename, int length) {
	header = (PDFHeader *) mmf.map(filename, length);
	assert(header);
	header->init(length);
}

Extent* PhysicalDataFile::newExtent(const char *ns, DiskLoc& loc) {
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
	return e;
}

/*---------------------------------------------------------------------*/ 

/* assumes already zeroed -- insufficient for block 'reuse' perhaps */
void Extent::init(const char *nsname, int _length, int _offset) { 
	magic = 0x41424344;
	myLoc.setOfs(_offset);
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

	DiskLoc nextEmpty = r->next;
	r->lengthWithHeaders = newRecSize;
	r->next.Null();
	if( !lastRecord.isNull() ) {
		assert(getRecord(lastRecord)->next.isNull());
		getRecord(lastRecord)->next = newRecordLoc;
	}
	lastRecord = newRecordLoc;

	if( firstRecord.isNull() )
		firstRecord = newRecordLoc;

	if( left < Record::HeaderSize + 32 ) { 
		firstEmptyRegion.Null();
	}
	else {
		firstEmptyRegion.inc(newRecSize);
		Record *empty = getRecord(firstEmptyRegion);
		empty->next = nextEmpty;
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

void DataFileMgr::insert(const char *ns, void *buf, int len) {
	DiskLoc loc;
	bool found = namespaceIndex.find(ns, loc);
	if( !found ) {
		cout << "New namespace: " << ns << endl;
		temp.newExtent(ns, loc);
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
