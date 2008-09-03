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

void dropDatabase(const char *ns);
void dropNS(string& dropNs);

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
		const char *buf, int len, stringstream& profiling);
	DiskLoc insert(const char *ns, const void *buf, int len, bool god = false);
	void deleteRecord(const char *ns, Record *todelete, const DiskLoc& dl, bool cappedOK = false);
	static auto_ptr<Cursor> findAll(const char *ns);

	/* special version of insert for transaction logging -- streamlined a bit.
       assumes ns is capped and no indexes
    */
	Record* fast_oplog_insert(NamespaceDetails *d, const char *ns, int len);

	static Extent* getExtent(const DiskLoc& dl);
	static Record* getRecord(const DiskLoc& dl);
private:
	vector<PhysicalDataFile *> files;
};

extern DataFileMgr theDataFileMgr;

#pragma pack(push,1)

class DeletedRecord {
public:
	int lengthWithHeaders;
	int extentOfs;
	DiskLoc nextDeleted;
};

/* Record is a record in a datafile.  DeletedRecord is similar but for deleted space.

*11:03:20 AM) dm10gen: regarding extentOfs...
(11:03:42 AM) dm10gen: an extent is a continugous disk area, which contains many Records and DeleteRecords
(11:03:56 AM) dm10gen: a DiskLoc has two pieces, the fileno and ofs.  (64 bit total)
(11:04:16 AM) dm10gen: to keep the headesr small, instead of storing a 64 bit ptr to the full extent address, we keep just the offset
(11:04:29 AM) dm10gen: we can do this as we know the record's address, and it has the same fileNo
(11:04:33 AM) dm10gen: see class DiskLoc for more info
(11:04:43 AM) dm10gen: so that is how Record::myExtent() works
(11:04:53 AM) dm10gen: on an alloc(), when we build a new Record, we must popular its extentOfs then 
*/
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

/* extents are datafile regions where all the records within the region 
   belong to the same namespace.

(11:12:35 AM) dm10gen: when the extent is allocated, all its empty space is stuck into one big DeletedRecord
(11:12:55 AM) dm10gen: and that is placed on the free list
*/
class Extent {
public:
	unsigned magic;
	DiskLoc myLoc;
	DiskLoc xnext, xprev; /* next/prev extent for this namespace */
	Namespace ns; /* which namespace this extent is for.  this is just for troubleshooting really */
	int length;   /* size of the extent, including these fields */
	DiskLoc firstRecord, lastRecord;
	char extentData[4];

	bool validates() { 
		return !(firstRecord.isNull() ^ lastRecord.isNull()) &&
			length >= 0 && !myLoc.isNull();
	}

	void dump(iostream& s) { 
		s << "    loc:" << myLoc.toString() << " xnext:" << xnext.toString() << " xprev:" << xprev.toString() << '\n';
		s << "    ns:" << ns.buf << '\n';
		s << "    size:" << length << " firstRecord:" << firstRecord.toString() << " lastRecord:" << lastRecord.toString() << '\n';
	}

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

	bool uninitialized() { 
		if( version == 0 ) return true; 
		assert(version == VERSION); 
		return false; 
	}

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

#include "cursor.h"

inline Record* PhysicalDataFile::recordAt(DiskLoc dl) { return header->getRecord(dl); }

void sayDbContext(const char *msg = 0);

inline DiskLoc Record::getNext(const DiskLoc& myLoc) {
	if( nextOfs != DiskLoc::NullOfs ) {
		/* defensive */
		if( nextOfs >= 0 && nextOfs < 10 ) { 
			sayDbContext("Assertion failure - Record::getNext() referencing a deleted record?");
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
	assert( fileNo != -1 );
	return (DeletedRecord*) rec();
}
inline Extent* DiskLoc::ext() const {
	return DataFileMgr::getExtent(*this);
}

inline BtreeBucket* DiskLoc::btree() const { 
	assert( fileNo != -1 );
	return (BtreeBucket*) rec()->data;
}

/*---------------------------------------------------------------------*/ 

// customer, or rather a customer's database -- i guess down the line
// there might be more than one for a cust, we'll see.
class Client { 
public:
	Client(const char *nm) : name(nm) { 
		justCreated = namespaceIndex.init(dbpath, nm);
		profile = 0;
		profileName = name + ".system.profile";
	} 
	~Client() { 
		int n = files.size();
		for( int i = 0; i < n; i++ )
			delete files[i];
	}

	PhysicalDataFile* getFile(int n) { 
		assert(this);

		if( n < 0 || n >= DiskLoc::MaxFiles ) {
			cout << "getFile(): n=" << n << endl;
			assert( n >= 0 && n < DiskLoc::MaxFiles );
		}
		DEV { 
			if( n > 100 )
				cout << "getFile(): n=" << n << "?" << endl;
		}
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
	NamespaceIndex namespaceIndex;
	int profile; // 0=off.
	string profileName; // "alleyinsider.system.profile"
	bool justCreated;
};

// tempish...move to TLS or pass all the way down as a parm
extern map<string,Client*> clients;
extern Client *client;
extern const char *curNs;
extern int dbLocked;
inline void setClient(const char *ns) { 
    /* we must be in critical section at this point as these are global 
       variables. 
    */
    assert( dbLocked == 1 );

	char cl[256];
	curNs = ns;
	nsToClient(ns, cl);
	map<string,Client*>::iterator it = clients.find(cl);
	if( it != clients.end() ) {
		client = it->second;
		return;
	}
	problem() << "first request for database " << cl << endl;
	Client *c = new Client(cl);
	clients[cl] = c;
	client = c;
}
/* we normally keep around a curNs ptr -- if this ns is temporary, 
   use this instead so we don't have a bad ptr.  we could have made a copy,
   but trying to be fast as we call setClient this for every single operation.
*/
inline void setClientTempNs(const char *ns) { 
	setClient(ns); curNs = "";
}

inline void _deleteDataFiles(const char *client) { 
	string c = client;
	c += '.';
	boost::filesystem::path p(dbpath);
	boost::filesystem::path q;
	q = p / (c+"ns");
	bool ok = boost::filesystem::remove(q);
	if( ok ) 
		log() << "  removed file " << q.string() << '\n';
	int i = 0;
	int extra = 10; // should not be necessary, this is defensive in case there are missing files
	while( 1 ) { 
		assert( i <= DiskLoc::MaxFiles );
		stringstream ss;
		ss << c << i;
		q = p / ss.str();
		if( boost::filesystem::remove(q) ) {
			log() << "  removed file " << q.string() << '\n';
			if( extra != 10 ) 
				log() << "  _deleteDataFiles() warning: extra == " << extra << endl;
		}
		else if( --extra <= 0 ) 
			break;
		i++;
	}
}

inline NamespaceIndex* nsindex(const char *ns) { 
	DEV { 
		char buf[256];
		nsToClient(ns, buf);
		if( client->name != buf ) { 
			cout << "ERROR: attempt to write to wrong database client\n";
			cout << " ns:" << ns << '\n';
			cout << " client->name:" << client->name << endl;
			assert( client->name == buf );
		}
	}
	return &client->namespaceIndex;
}

inline NamespaceDetails* nsdetails(const char *ns) { 
	return nsindex(ns)->details(ns);
}

inline PhysicalDataFile& DiskLoc::pdf() const { 
	assert( fileNo != -1 );
	return *client->getFile(fileNo);
}

inline Extent* DataFileMgr::getExtent(const DiskLoc& dl) {
	assert( dl.a() != -1 );
	return client->getFile(dl.a())->getExtent(dl);
}

inline Record* DataFileMgr::getRecord(const DiskLoc& dl) {
	assert( dl.a() != -1 );
	return client->getFile(dl.a())->recordAt(dl);
}
