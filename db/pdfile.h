/* pdfile.h

   Files:
     client.ns - namespace index
     client.1  - data files
     client.2 
     ...
*/

#pragma once

// see version, versionMinor, below.
const int VERSION = 4;

#include "../stdafx.h"
#include "../util/mmap.h"
#include "storage.h"
#include "jsobj.h"
#include "namespace.h"

class PDFHeader;
class Extent;
class Record;
class Cursor;

/*---------------------------------------------------------------------*/ 

class PDFHeader;
class PhysicalDataFile {
	friend class DataFileMgr;
	friend class BasicCursor;
public:
	PhysicalDataFile(int fn) : fileNo(fn) { }
	void open(int fileNo, const char *filename);


	Extent* newExtent(const char *ns, int approxSize, int loops = 0);
private:
	Extent* getExtent(DiskLoc loc);
	Extent* _getExtent(DiskLoc loc);
	Record* recordAt(DiskLoc dl);

	MemoryMappedFile mmf;
	PDFHeader *header;
int __unUsEd;
//	int length;
	int fileNo;
};

class DataFileMgr {
	friend class BasicCursor;
public:
	void init(const char *);

	void update(
		const char *ns,
		Record *toupdate, const DiskLoc& dl,
		const char *buf, int len);
	DiskLoc insert(const char *ns, const void *buf, int len, bool god = false);
	void deleteRecord(const char *ns, Record *todelete, const DiskLoc& dl, bool cappedOK = false);
	static auto_ptr<Cursor> findAll(const char *ns);

	static Extent* getExtent(const DiskLoc& dl);
	static Record* getRecord(const DiskLoc& dl);
private:
	vector<PhysicalDataFile *> files;
};

extern DataFileMgr theDataFileMgr;

#pragma pack(push)
#pragma pack(1)

class DeletedRecord {
public:
	int lengthWithHeaders;
	int extentOfs;
	DiskLoc nextDeleted;
};

class Record {
public:
	enum { HeaderSize = 16 };
	int lengthWithHeaders;
	int extentOfs;
	int nextOfs;
	int prevOfs;
	char data[4];
	int netLength() { return lengthWithHeaders - HeaderSize; }
	//void setNewLength(int netlen) { lengthWithHeaders = netlen + HeaderSize; }

	/* use this when a record is deleted. basically a union with next/prev fields */
	DeletedRecord& asDeleted() { return *((DeletedRecord*) this); }

	Extent* myExtent(const DiskLoc& myLoc) { 
		return DataFileMgr::getExtent(DiskLoc(myLoc.a(), extentOfs));
	}
	/* get the next record in the namespace, traversing extents as necessary */
	DiskLoc getNext(const DiskLoc& myLoc);
	DiskLoc getPrev(const DiskLoc& myLoc);
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

	/* assumes already zeroed -- insufficient for block 'reuse' perhaps 
	Returns a DeletedRecord location which is the data in the extent ready for us.
	Caller will need to add that to the freelist structure in namespacedetail.
	*/
	DiskLoc init(const char *nsname, int _length, int _fileNo, int _offset);

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
	char reserved[8192 - 4*4 - 8];

	char data[4];

	static int headerSize() { return sizeof(PDFHeader) - 4; }

	bool uninitialized() { if( version == 0 ) return true; assert(version == VERSION); return false; }

	Record* getRecord(DiskLoc dl) {
		int ofs = dl.getOfs();
		assert( ofs >= headerSize() );
		return (Record*) (((char *) this) + ofs);
	}

	void init(int fileno, int filelength) {
		if( uninitialized() ) {
			assert(filelength > 32768 );
			assert( headerSize() == 8192 );
			fileLength = filelength;
			version = VERSION;
			versionMinor = 0;
			unused.setOfs( fileno, headerSize() );
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
	virtual bool ok() = 0;
	bool eof() { return !ok(); }
	virtual Record* _current() = 0;
	virtual JSObj current() = 0;
	virtual DiskLoc currLoc() = 0;
	virtual bool advance() = 0; /*true=ok*/

	/* optional to implement.  if implemented, means 'this' is a prototype */
	virtual Cursor* clone() { return 0; }

	virtual bool tempStopOnMiss() { return false; }

	/* called after every query block is iterated -- i.e. between getMore() blocks
	   so you can note where we are, if necessary.
	   */
	virtual void noteLocation() { } 

	/* called before query getmore block is iterated */
	virtual void checkLocation() { } 

	virtual const char * toString() { return "abstract?"; }

	/* used for multikey index traversal to avoid sending back dups. see JSMatcher::matches() */
	set<DiskLoc> dups;
	bool dup(DiskLoc loc) {
		/* to save mem only call this when there is risk of dups (e.g. when 'deep'/multikey) */
		if( dups.count(loc) > 0 )
			return true;
		dups.insert(loc);
		return false;
	}
};

class BasicCursor : public Cursor {
public:
	bool ok() { return !curr.isNull(); }
	Record* _current() {
		assert( ok() );
		return curr.rec();
	}
	JSObj current() { return JSObj( _current() ); }
	virtual DiskLoc currLoc() { return curr; }

	bool advance() { 
		if( eof() )
			return false;
		Record *r = _current();
		curr = r->getNext(curr);
		return ok();
	}

	BasicCursor(DiskLoc dl) : curr(dl) { }
	BasicCursor() { }
	virtual const char * toString() { return "BasicCursor"; }

	DiskLoc curr;
};

class ReverseCursor : public BasicCursor {
public:
	bool advance() { 
		if( eof() )
			return false;
		Record *r = _current();
		curr = r->getPrev(curr);
		return ok();
	}

	ReverseCursor(DiskLoc dl) : BasicCursor(dl) { }
	ReverseCursor() { }
	virtual const char * toString() { return "ReverseCursor"; }
};

inline Record* PhysicalDataFile::recordAt(DiskLoc dl) { return header->getRecord(dl); }

void sayDbContext();

inline DiskLoc Record::getNext(const DiskLoc& myLoc) {
	if( nextOfs != DiskLoc::NullOfs ) {
		/* defensive */
		if( nextOfs >= 0 && nextOfs < 10 ) { 
			cout << "Assertion failure - Record::getNext() referencing a deleted record?" << endl;
			sayDbContext();
			return DiskLoc();
		}

		return DiskLoc(myLoc.a(), nextOfs);
	}
	Extent *e = myExtent(myLoc);
	while( 1 ) {
		if( e->xnext.isNull() )
			return DiskLoc(); // end of table.
		e = e->xnext.ext();
		if( !e->firstRecord.isNull() ) 
			break;
		// entire extent could be empty, keep looking
	}
	return e->firstRecord;
}
inline DiskLoc Record::getPrev(const DiskLoc& myLoc) {
	if( prevOfs != DiskLoc::NullOfs )
		return DiskLoc(myLoc.a(), prevOfs);
	Extent *e = myExtent(myLoc);
	if( e->xprev.isNull() )
		return DiskLoc();
	return e->xprev.ext()->lastRecord;
}

inline Record* DiskLoc::rec() const {
	return DataFileMgr::getRecord(*this);
}
inline JSObj DiskLoc::obj() const {
	return JSObj(rec());
}
inline DeletedRecord* DiskLoc::drec() const {
	return (DeletedRecord*) rec();
}
inline Extent* DiskLoc::ext() const {
	return DataFileMgr::getExtent(*this);
}

inline BtreeBucket* DiskLoc::btree() const { 
	return (BtreeBucket*) rec()->data;
}

inline Bucket* DiskLoc::bucket() const { 
	return (Bucket*) rec()->data;
}

/*---------------------------------------------------------------------*/ 

// customer, or rather a customer's database -- i guess down the line
// there might be more than one for a cust, we'll see.
class Client { 
public:
	Client(const char *nm) : name(nm) { 
		namespaceIndex = new NamespaceIndex();
		namespaceIndex->init(dbpath, nm);
		profile = 0;
	} 

	PhysicalDataFile* getFile(int n) { 
//			assert( name != "alleyinsider" );

		if( n < 0 || n >= 100000 ) {
			cout << "getFile(): n=" << n << endl;
			assert( n >= 0 && n < 100000 );
		}
#if defined(_DEBUG)
		if( n > 100 )
			cout << "getFile(): n=" << n << "?" << endl;
#endif
		while( n >= (int) files.size() )
			files.push_back(0);
		PhysicalDataFile* p = files[n];
		if( p == 0 ) {
			p = new PhysicalDataFile(n);
			files[n] = p;
			stringstream out;
			out << dbpath << name << '.' << n;
			p->open(n, out.str().c_str());
		}
		return p;
	}

	PhysicalDataFile* addAFile() {
		int n = (int) files.size();
		return getFile(n);
	}

	PhysicalDataFile* newestFile() { 
		int n = (int) files.size();
		if( n > 0 ) n--;
		return getFile(n);
	}

	vector<PhysicalDataFile*> files;
	string name; // "alleyinsider"
	NamespaceIndex *namespaceIndex;
	int profile; // 0=off.
};

// tempish...move to TLS or pass all the way down as a parm
extern map<string,Client*> clients;
extern Client *client;
extern const char *curNs;
inline void setClient(const char *ns) { 
	char cl[256];
	curNs = ns;
	nsToClient(ns, cl);
	map<string,Client*>::iterator it = clients.find(cl);
	if( it != clients.end() ) {
		client = it->second;
		return;
	}
	Client *c = new Client(cl);
	clients[cl] = c;
	client = c;
}

inline NamespaceIndex* nsindex(const char *ns) { 
#if defined(_DEBUG)
	char buf[256];
	nsToClient(ns, buf);
	assert( client->name == buf );
#endif
	return client->namespaceIndex;
}

inline NamespaceDetails* nsdetails(const char *ns) { 
	return nsindex(ns)->details(ns);
}

inline PhysicalDataFile& DiskLoc::pdf() const { 
	return *client->getFile(fileNo);
}

inline Extent* DataFileMgr::getExtent(const DiskLoc& dl) {
	return client->getFile(dl.a())->getExtent(dl);
}

inline Record* DataFileMgr::getRecord(const DiskLoc& dl) {
	return client->getFile(dl.a())->recordAt(dl);
}

