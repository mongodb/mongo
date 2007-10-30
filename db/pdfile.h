// pdfile.h

#pragma once

#include "../stdafx.h"
#include "../util/mmap.h"
#include "storage.h"

class PDFHeader;
class Extent;
class Record;

/*---------------------------------------------------------------------*/ 

class Namespace {
public:
	Namespace(const char *ns) { 
		*this = ns;
	}
	Namespace& operator=(const char *ns) { 
		memset(buf, 0, 128); /* this is just to keep stuff clean in the files for easy dumping and reading */
		strcpy_s(buf, 128, ns); return *this; 
	}

	bool operator==(const Namespace& r) { return strcmp(buf, r.buf) == 0; }
	int hash() const {
		unsigned x = 0;
		const char *p = buf;
		while( *p ) { 
			x = x * 131 + *p;
			p++;
		}
		return (x & 0x7fffffff) | 0x8000000; // must be > 0
	}

	char buf[128];
};

/*---------------------------------------------------------------------*/ 

class Cursor;

class PDFHeader;
class PhysicalDataFile {
	friend class DataFileMgr;
	friend class Cursor;
public:
	void open(const char *filename, int length = 64 * 1024 * 1024);

private:
	Extent* newExtent(const char *ns, DiskLoc& loc, Extent *prev);
	Extent* getExtent(DiskLoc loc);
	Extent* _getExtent(DiskLoc loc);
	Record* recordAt(DiskLoc dl);

	MemoryMappedFile mmf;
	PDFHeader *header;
	int length;
};

class DataFileMgr {
	friend class Cursor;
public:
	void init();

	void update(
		const char *ns,
		Record *toupdate, const DiskLoc& dl,
		const char *buf, int len);
	void insert(const char *ns, const void *buf, int len);
	void deleteRecord(const char *ns, Record *todelete, const DiskLoc& dl);
	Cursor findAll(const char *ns);

	static Extent* getExtent(const DiskLoc& dl);
	static Record* getRecord(const DiskLoc& dl);
private:
	PhysicalDataFile temp;
};

extern DataFileMgr theDataFileMgr;

#pragma pack(push)
#pragma pack(1)

/* lots of code to make our next/prev pointers 4 bytes instead of 8! */
class SmartLoc {
public:
	SmartLoc() { x = 0; }
	DiskLoc getNextEmpty(const DiskLoc& myLoc) {
		assert( x >= 0 );
		return DiskLoc(myLoc.a(), x);
	}
	DiskLoc getNext(const DiskLoc& myLoc);
	DiskLoc getPrev(const DiskLoc& myLoc);
	/* if a next pointer, marks as last.  if a prev pointer, marks as first */
	void markAsFirstOrLastInExtent(Extent *e);
	bool firstInExtent() { return x < 0; }
	bool lastInExtent() { return x < 0; }
	void set(const DiskLoc& nextprevRecordLoc) { x = nextprevRecordLoc.getOfs(); }
	void Null() { x = 0; } /* this is for empty records only. nonempties point to the extent. */
	DiskLoc myExtent(const DiskLoc& myLoc);
private:
	int x;
};

class DeletedRecord {
public:
	DiskLoc nextDeleted;
	int lengthWithHeaders;
	int myOfs;
	DiskLoc myExtent;

	void init(const DiskLoc& extent, const DiskLoc& myLoc) {
		myExtent = extent;
		myOfs = myLoc.getOfs();
	}
};
const int MinRecordSize = sizeof(DeletedRecord);

class Record {
public:
	enum { HeaderSize = 12 };
	SmartLoc next, prev;
	int lengthWithHeaders;
	char data[4];
	//	bool haveNext() { return !next.isNull(); }
	int netLength() { return lengthWithHeaders - HeaderSize; }
	void setNewLength(int netlen) { lengthWithHeaders = netlen + HeaderSize; }

	/* use this when a record is deleted. basically a union with next/prev fields */
	DiskLoc& asDeleted() { return *((DeletedRecord*) this); }
};

/* extents are regions where all the records within the region 
   belong to the same namespace.
*/
class Extent {
public:
	unsigned magic;
	DiskLoc myLoc;
	DiskLoc xnext, xprev; /* next/prev extent for this namespace */
	Namespace ns; /* which namespace this extent is for.  this is just for troubleshooting really */
	int length;   /* size of the extent, including these fields */
	//	DiskLoc firstEmptyRegion;
	DiskLoc firstRecord, lastRecord;
	char extentData[4];

	/* assumes already zeroed -- insufficient for block 'reuse' perhaps */
	void init(const char *nsname, int _length, int _offset);

	void assertOk() { assert(magic == 0x41424344); }

	Record* newRecord(int len);

	Record* getRecord(DiskLoc dl) {
		assert( !dl.isNull() );
		assert( dl.sameFile(myLoc) );
		int x = dl.getOfs() - myLoc.getOfs();
		assert( x > 0 );
		return (Record *) (((char *) this) + x);
	}

	Extent* getNextExtent() { return xnext.isNull() ? 0 : DataFileMgr::getExtent(xnext); }
	Extent* getPrevExtent() { return xprev.isNull() ? 0 : DataFileMgr::getExtent(xprev); }
};

inline DiskLoc SmartLoc::getNext(const DiskLoc& myLoc) {
	assert( x != 0 );
	if( x > 0 )
		return DiskLoc(myLoc.a(), x);
	// we are the last one in this extent.
	DiskLoc extLoc(myLoc.a(), -x);
	Extent *e = DataFileMgr::getExtent(extLoc);
	assert( e->lastRecord == myLoc );
	Extent *nxt = e->getNextExtent();
	return nxt ? nxt->firstRecord : DiskLoc();
}
inline DiskLoc SmartLoc::getPrev(const DiskLoc& myLoc) {
	assert( x != 0 );
	if( x > 0 )
		return DiskLoc(myLoc.a(), x);
	// we are the first one in this extent.
	DiskLoc extLoc(myLoc.a(), -x);
	Extent *e = DataFileMgr::getExtent(extLoc);
	assert( e->firstRecord == myLoc );
	Extent *prv = e->getPrevExtent();
	return prv ? prv->lastRecord : DiskLoc();
}
/* only works if first (or last for 'next') record in the extent. */
inline DiskLoc SmartLoc::myExtent(const DiskLoc& myLoc) {
	return x < 0 ? DiskLoc(myLoc.a(), -x) : DiskLoc();
}
/* if a next pointer, marks as last.  if a prev pointer, marks as first */
inline void SmartLoc::markAsFirstOrLastInExtent(Extent *e) { 
	x = -e->myLoc.getOfs(); 
}

/*
      ----------------------
      Header
      ----------------------
      Extent (for a particular namespace)
        Record
        ...
        Record (some chained for unused space)
      ----------------------
      more Extents...
      ----------------------
*/

/* data file header */
class PDFHeader {
public:
	int version;
	int versionMinor;
	int fileLength;
	DiskLoc unused; /* unused is the portion of the file that doesn't belong to any allocated extents. -1 = no more */
	int unusedLength;
	int reserved[8192 - 4*4];

	char data[4];

	static int headerSize() { return sizeof(PDFHeader) - 4; }

	bool uninitialized() { if( version == 0 ) return true; assert(version == 3); return false; }

	Record* getRecord(DiskLoc dl) {
		int ofs = dl.getOfs();
		assert( ofs >= headerSize() );
		return (Record*) (((char *) this) + ofs);
	}

	void init(int filelength) {
		if( uninitialized() ) {
			assert(filelength > 32768 );
			fileLength = filelength;
			version = 3;
			versionMinor = 0;
			unused.setOfs( headerSize() );
			assert( (data-(char*)this) == headerSize() );
			unusedLength = fileLength - headerSize() - 16;
			memcpy(data+unusedLength, "      \nthe end\n", 16); 
		}
	}
};

#pragma pack(pop)

inline Extent* PhysicalDataFile::_getExtent(DiskLoc loc) {
	loc.assertOk();
	Extent *e = (Extent *) (((char *)header) + loc.getOfs());
	return e;
}

inline Extent* PhysicalDataFile::getExtent(DiskLoc loc) {
	Extent *e = _getExtent(loc);
	e->assertOk();
	return e;
}

class Cursor {
public:
	bool ok() { return !curr.isNull(); }
	bool eof() { return !ok(); }
	Record* current() {
		assert( ok() );
		return theDataFileMgr.temp.recordAt(curr); 
	}
	bool advance() { 
		if( eof() )
			return false;
		Record *r = current();
		curr = r->next.getNext(curr);
		return ok();
	}

	Cursor(DiskLoc dl) : curr(dl) { }
	Cursor() { }

	DiskLoc curr;
};

inline Record* PhysicalDataFile::recordAt(DiskLoc dl) { return header->getRecord(dl); }

inline Extent* DataFileMgr::getExtent(const DiskLoc& dl) {
	return theDataFileMgr.temp.getExtent(dl);
}
inline Record* DataFileMgr::getRecord(const DiskLoc& dl) {
	return theDataFileMgr.temp.recordAt(dl);
}
