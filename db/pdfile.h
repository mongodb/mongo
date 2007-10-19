// pdfile.h

#pragma once

#include "../stdafx.h"
#include "../util/mmap.h"
#include "storage.h"

struct PDFHeader;
struct Extent;
struct Record;

/*---------------------------------------------------------------------*/ 

struct Namespace {
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

#pragma pack(push)
#pragma pack(1)

struct Record {
	enum { HeaderSize = 12 };
	DiskLoc next;
	int lengthWithHeaders;
	char data[4];
	bool haveNext() { return !next.isNull(); }
	int netLength() { return lengthWithHeaders - HeaderSize; }
	void setNewLength(int netlen) { lengthWithHeaders = netlen + HeaderSize; }
};

/* extents are regions where all the records within the region 
   belong to the same namespace.
*/
struct Extent {
	unsigned magic;
	DiskLoc myLoc;
	Namespace ns; /* which namespace this extent is for.  this is just for troubleshooting really */
	int length;   /* size of the extent, including these fields */
	DiskLoc firstEmptyRegion;
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
};

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
struct PDFHeader {
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

class PhysicalDataFile {
	friend class DataFileMgr;
	friend class Cursor;
public:
	void open(const char *filename, int length = 64 * 1024 * 1024);

private:
	Extent* newExtent(const char *ns, DiskLoc& loc);
	Extent* getExtent(DiskLoc loc);
	Extent* _getExtent(DiskLoc loc);
	Record* recordAt(DiskLoc dl) { return header->getRecord(dl); }

	MemoryMappedFile mmf;
	PDFHeader *header;
	int length;
};

inline Extent* PhysicalDataFile::_getExtent(DiskLoc loc) {
	loc.assertOk();
	Extent *e = (Extent *) (((char *)header) + loc.getOfs());
	return e;
}

inline Extent* PhysicalDataFile::getExtent(DiskLoc loc) {
	Extent *e = _getExtent(loc);
	return e;
}

class Cursor;

class DataFileMgr {
	friend class Cursor;
public:
	void init();

	void insert(const char *ns, void *buf, int len);
	Cursor findAll(const char *ns);

private:
	PhysicalDataFile temp;
};

extern DataFileMgr theDataFileMgr;

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
		curr = r->next;
		return ok();
	}

	Cursor(DiskLoc dl) : curr(dl) { }
	Cursor() { }

private:
	DiskLoc curr;
};

