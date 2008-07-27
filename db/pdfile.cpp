// pdfile.cpp

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

/* 
todo: 
_ table scans must be sequential, not next/prev pointers
_ coalesce deleted

_ disallow system* manipulations from the client.
*/

#include "stdafx.h"
#include "pdfile.h"
#include "db.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"
#include "objwrappers.h"
#include "btree.h"
#include <algorithm>
#include <list>
#include "query.h"

const char *dbpath = "/data/db/";

DataFileMgr theDataFileMgr;
map<string,Client*> clients;
Client *client;
const char *curNs = "";
int MAGIC = 0x1000;
int curOp = -2;
int callDepth = 0;

extern int otherTraceLevel;

void sayDbContext(const char *errmsg) { 
	if( errmsg ) { 
		cout << errmsg << '\n';
		problem() << errmsg << endl;
	}
	cout << " client: " << (client ? client->name.c_str() : "null");
	cout << " op:" << curOp << ' ' << callDepth << endl;
	if( client )
		cout << " ns: " << curNs << endl;
	printStackTrace();
}

JSObj::JSObj(Record *r) { 
	init(r->data, false);
/*
	_objdata = r->data;
	_objsize = *((int*) _objdata);
	if( _objsize > r->netLength() ) { 
		cout << "About to assert fail _objsize <= r->netLength()" << endl;
		cout << " _objsize: " << _objsize << endl;
		cout << " netLength(): " << r->netLength() << endl;
		cout << " extentOfs: " << r->extentOfs << endl;
		cout << " nextOfs: " << r->nextOfs << endl;
		cout << " prevOfs: " << r->prevOfs << endl;
		assert( _objsize <= r->netLength() );
	}
	iFree = false;
*/
}

/*---------------------------------------------------------------------*/ 

int bucketSizes[] = { 
	32, 64, 128, 256, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000,
	0x8000, 0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000,
	0x400000, 0x800000
};

//NamespaceIndexMgr namespaceIndexMgr;

void NamespaceDetails::addDeletedRec(DeletedRecord *d, DiskLoc dloc) { 
	{ 
		// defensive code: try to make us notice if we reference a deleted record 
		(unsigned&) (((Record *) d)->data) = 0xeeeeeeee;
	}

	dassert( dloc.drec() == d );
	DEBUGGING cout << "TEMP: add deleted rec " << dloc.toString() << ' ' << hex << d->extentOfs << endl;
	int b = bucket(d->lengthWithHeaders);
	DiskLoc& list = deletedList[b];
	DiskLoc oldHead = list;
	list = dloc;
	d->nextDeleted = oldHead;
}

/* 
   lenToAlloc is WITH header 
*/
DiskLoc NamespaceDetails::alloc(const char *ns, int lenToAlloc, DiskLoc& extentLoc) {
	lenToAlloc = (lenToAlloc + 3) & 0xfffffffc;
	DiskLoc loc = _alloc(ns, lenToAlloc);
	if( loc.isNull() )
		return loc;

	DeletedRecord *r = loc.drec();

	/* note we want to grab from the front so our next pointers on disk tend
	to go in a forward direction which is important for performance. */
	int regionlen = r->lengthWithHeaders;
	extentLoc.set(loc.a(), r->extentOfs);
	assert( r->extentOfs < loc.getOfs() );

	DEBUGGING cout << "TEMP: alloc() returns " << loc.toString() << ' ' << ns << " lentoalloc:" << lenToAlloc << " ext:" << extentLoc.toString() << endl;

	int left = regionlen - lenToAlloc;
	if( left < 24 || (left < (lenToAlloc >> 3) && capped == 0) ) {
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

/* for non-capped collections.
   returned item is out of the deleted list upon return 
*/
DiskLoc NamespaceDetails::__stdAlloc(int len) {
	DiskLoc *prev;
	DiskLoc *bestprev = 0;
	DiskLoc bestmatch;
	int bestmatchlen = 0x7fffffff;
	int b = bucket(len);
	DiskLoc cur = deletedList[b]; prev = &deletedList[b];
	int extra = 5; // look for a better fit, a little.
	int chain = 0;
	while( 1 ) { 
		{
			int a = cur.a();
			if( a < -1 || a >= 100000 ) {
				problem() << "~~ Assertion - cur out of range in _alloc() " << cur.toString() <<
					" b:" << b << " chain:" << chain << '\n';
				sayDbContext();
				if( cur == *prev )
					prev->Null();
				cur.Null();
			}
		}
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
			//b++;
			chain = 0;
			cur.Null();
		}
		else {
			if( r->nextDeleted.getOfs() == 0 ) {
				problem() << "~~ Assertion - bad nextDeleted " << r->nextDeleted.toString() << 
					" b:" << b << " chain:" << chain << ", fixing.\n";
				r->nextDeleted.Null();
			}
			cur = r->nextDeleted; prev = &r->nextDeleted;
		}
	}

	/* unlink ourself from the deleted list */
	{
		DeletedRecord *bmr = bestmatch.drec();
		*bestprev = bmr->nextDeleted;
		bmr->nextDeleted.setInvalid(); // defensive.
		assert(bmr->extentOfs < bestmatch.getOfs());
	}

	return bestmatch;
}

void NamespaceDetails::dumpDeleted(set<DiskLoc> *extents) { 
//	cout << "DUMP deleted chains" << endl;
	for( int i = 0; i < Buckets; i++ ) { 
//		cout << "  bucket " << i << endl;
		DiskLoc dl = deletedList[i];
		while( !dl.isNull() ) { 
			DeletedRecord *r = dl.drec();
			DiskLoc extLoc(dl.a(), r->extentOfs);
			if( extents == 0 || extents->count(extLoc) <= 0 ) {
				cout << "  bucket " << i << endl;
				cout << "   " << dl.toString() << " ext:" << extLoc.toString();
				if( extents && extents->count(extLoc) <= 0 )
					cout << '?';
				cout << " len:" << r->lengthWithHeaders << endl;
			}
			dl = r->nextDeleted;
		}
	}
//	cout << endl;
}

/* combine adjacent deleted records

   this is O(n^2) but we call it for capped tables where typically n==1 or 2! 
   (or 3...there will be a little unused sliver at the end of the extent.)
*/
void NamespaceDetails::compact() { 
	assert(capped);
	list<DiskLoc> drecs;

	for( int i = 0; i < Buckets; i++ ) { 
		DiskLoc dl = deletedList[i];
		deletedList[i].Null();
		while( !dl.isNull() ) { 
			DeletedRecord *r = dl.drec();
			drecs.push_back(dl);
			dl = r->nextDeleted;
		}
	}

	drecs.sort();

	list<DiskLoc>::iterator j = drecs.begin(); 
	assert( j != drecs.end() );
	DiskLoc a = *j;
	while( 1 ) {
		j++;
		if( j == drecs.end() ) {
			DEBUGGING cout << "TEMP: compact adddelrec\n";
			addDeletedRec(a.drec(), a);
			break;
		}
		DiskLoc b = *j;
		while( a.a() == b.a() && a.getOfs() + a.drec()->lengthWithHeaders == b.getOfs() ) { 
			// a & b are adjacent.  merge.
			a.drec()->lengthWithHeaders += b.drec()->lengthWithHeaders;
			j++;
			if( j == drecs.end() ) {
				DEBUGGING cout << "temp: compact adddelrec2\n";
				addDeletedRec(a.drec(), a);
				return;
			}
			b = *j;
		}
		DEBUGGING cout << "temp: compact adddelrec3\n";
		addDeletedRec(a.drec(), a);
		a = b;
	}
}

/* alloc with capped table handling. */
int n_complaints_cap = 0;
DiskLoc NamespaceDetails::_alloc(const char *ns, int len) {
	if( !capped )
		return __stdAlloc(len);

	// capped. 

	assert( len < 400000000 );
	int passes = 0;
	DiskLoc loc;

	// delete records until we have room and the max # objects limit achieved.
	Extent *theExtent = firstExtent.ext(); // only one extent if capped.
	dassert( theExtent->ns == ns ); 
	theExtent->assertOk();
	while( 1 ) {
		if( nrecords < max ) { 
			loc = __stdAlloc(len);
			if( !loc.isNull() )
				break;
		}

		DiskLoc fr = theExtent->firstRecord;
		if( fr.isNull() ) { 
			if( ++n_complaints_cap < 8 ) {
				cout << "couldn't make room for new record in capped ns " << ns << '\n'
					<< "  len: " << len << " extentsize:" << lastExtentSize << '\n';
				cout << "  magic: " << hex << theExtent->magic << " extent->ns: " << theExtent->ns.buf << '\n';
				cout << "  fr: " << theExtent->firstRecord.toString() << 
					" lr: " << theExtent->lastRecord.toString() << " extent->len: " << theExtent->length << '\n';
				assert( len * 5 > lastExtentSize ); // assume it is unusually large record; if not, something is broken
			}
			return DiskLoc();
		}

		theDataFileMgr.deleteRecord(ns, fr.rec(), fr, true);
		compact();
		assert( ++passes < 5000 );
	}

	return loc;
}

/*
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
}*/

/* add a new namespace to the system catalog (<dbname>.system.namespaces).
*/
void addNewNamespaceToCatalog(const char *ns) {
	cout << "New namespace: " << ns << endl;
	if( strstr(ns, "system.namespaces") ) { 
		// system.namespaces holds all the others, so it is not explicitly listed in the catalog.
		// TODO: fix above should not be strstr!
		return;
	}

	{
		JSObjBuilder b;
		b.append("name", ns);
		JSObj j = b.done();
		char client[256];
		nsToClient(ns, client);
		string s = client;
		s += ".system.namespaces";
		theDataFileMgr.insert(s.c_str(), j.objdata(), j.objsize(), true);
	}
}

int initialExtentSize(int len) { 
	long long sz = len * 16;
	if( len < 1000 ) sz = len * 64;
	if( sz > 1000000000 )
		sz = 1000000000;
	int z = ((int)sz) & 0xffffff00;
	assert( z > len );
	cout << "initialExtentSize(" << len << ") returns " << z << endl;
	return z;
}

// { ..., capped: true, size: ..., max: ... }
// returns true if successful
bool userCreateNS(const char *ns, JSObj& j, string& err) { 
	if( nsdetails(ns) ) {
		err = "collection already exists";
		return false;
	}

	cout << "create collection " << ns << ' ' << j.toString() << endl;

	/* todo: do this only when we have allocated space successfully? or we could insert with a { ok: 0 } field
             and then go back and set to ok : 1 after we are done.
	*/
	addNewNamespaceToCatalog(ns);

	int ies = initialExtentSize(128);
	Element e = j.findElement("size");
	if( e.type() == Number ) {
		ies = (int) e.number();
		ies += 256;
		ies &= 0xffffff00;
		if( ies > 1024 * 1024 * 1024 + 256 ) return false;
	}

	client->newestFile()->newExtent(ns, ies);
	NamespaceDetails *d = nsdetails(ns);
	assert(d);

	e = j.findElement("capped");
	if( e.type() == Bool && e.boolean() ) {
		d->capped = 1;
		e = j.findElement("max");
		if( e.type() == Number ) { 
			int mx = (int) e.number();
			if( mx > 0 )
				d->max = mx;
		}
	}

	return true;
}

/*---------------------------------------------------------------------*/ 

void PhysicalDataFile::open(int fn, const char *filename) {
	int length;
        
	if( fn <= 4 ) {
		length = (64*1024*1024) << fn;
		if( strstr(filename, "alleyinsider") && length < 1024 * 1024 * 1024 ) {
			DEV cout << "Warning: not making alleyinsider datafile bigger because DEV is true" << endl; 
   		    else
				length = 1024 * 1024 * 1024;
		}
	} else
		length = 0x7ff00000;
        
	assert( length >= 64*1024*1024 );

        if( strstr(filename, "_hudsonSmall") ) {
          int mult = 1;
          if ( fn > 1 && fn < 1000 )
            mult = fn;
          length = 1024 * 512 * mult;
          cout << "Warning : using small files for _hudsonSmall" << endl;
        }


	assert( length % 4096 == 0 );

	assert(fn == fileNo);
	header = (PDFHeader *) mmf.map(filename, length);
	assert(header);
	header->init(fileNo, length);
}

/* prev - previous extent for this namespace.  null=this is the first one. */
Extent* PhysicalDataFile::newExtent(const char *ns, int approxSize, int loops) {
	assert( approxSize >= 0 && approxSize <= 0x7ff00000 );

	assert( header ); // null if file open failed
	int ExtentSize = approxSize <= header->unusedLength ? approxSize : header->unusedLength;
	DiskLoc loc;
	if( ExtentSize <= 0 ) {
		/* not there could be a lot of looping here is db just started and 
		   no files are open yet.  we might want to do something about that. */
		if( loops > 8 ) {
			assert( loops < 10000 );
			cout << "warning: loops=" << loops << " fileno:" << fileNo << ' ' << ns << '\n';
		}
		cout << "info: newExtent(): file " << fileNo << " full, adding a new file " << ns << endl;
		return client->addAFile()->newExtent(ns, approxSize, loops+1);
	}
	int offset = header->unused.getOfs();
	header->unused.setOfs( fileNo, offset + ExtentSize );
	header->unusedLength -= ExtentSize;
	loc.setOfs(fileNo, offset);
	Extent *e = _getExtent(loc);
	DiskLoc emptyLoc = e->init(ns, ExtentSize, fileNo, offset);

	DiskLoc oldExtentLoc;
	NamespaceIndex *ni = nsindex(ns);
	NamespaceDetails *details = ni->details(ns);
	if( details ) { 
		assert( !details->firstExtent.isNull() );
		e->xprev = details->lastExtent;
		details->lastExtent.ext()->xnext = loc;
		details->lastExtent = loc;
	}
	else {
		ni->add(ns, loc);
		details = ni->details(ns);
	}

	details->lastExtentSize = approxSize;
	DEBUGGING cout << "temp: newextent adddelrec " << ns << endl;
	details->addDeletedRec(emptyLoc.drec(), emptyLoc);

	cout << "new extent size: 0x" << hex << ExtentSize << " loc: 0x" << hex << offset << dec;
	cout << " emptyLoc:" << hex << emptyLoc.getOfs() << dec;
	cout << ' ' << ns << endl;
	return e;
}

/*---------------------------------------------------------------------*/ 

/* assumes already zeroed -- insufficient for block 'reuse' perhaps */
DiskLoc Extent::init(const char *nsname, int _length, int _fileNo, int _offset) { 
	magic = 0x41424344;
	myLoc.setOfs(_fileNo, _offset);
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
	bool found = nsindex(ns)->find(ns, loc);
	if( !found ) {
		//		cout << "info: findAll() namespace does not exist: " << ns << endl;
		return auto_ptr<Cursor>(new BasicCursor(DiskLoc()));
	}

	Extent *e = getExtent(loc);

	DEBUGGING {
		cout << "listing extents for " << ns << endl;
		DiskLoc tmp = loc;
		set<DiskLoc> extents;

		while( 1 ) { 
			Extent *f = getExtent(tmp);
			cout << "extent: " << tmp.toString() << endl;
			extents.insert(tmp);
			tmp = f->xnext;
			if( tmp.isNull() )
				break;
			f = f->getNextExtent();
		}

		cout << endl;
		nsdetails(ns)->dumpDeleted(&extents);
	}

	while( e->firstRecord.isNull() && !e->xnext.isNull() ) {
		/* todo: if extent is empty, free it for reuse elsewhere. 
		         that is a bit complicated have to clean up the freelists.
		*/
	    RARELY cout << "info DFM::findAll(): extent " << loc.toString() << " was empty, skipping ahead " << ns << endl;
		// find a nonempty extent
		// it might be nice to free the whole extent here!  but have to clean up free recs then.
		e = e->getNextExtent();
	}
	return auto_ptr<Cursor>(new BasicCursor( e->firstRecord ));
}

/* get a table scan cursor, but can be forward or reverse direction.
   order.$natural - if set, > 0 means forward (asc), < 0 backward (desc).
*/
auto_ptr<Cursor> findTableScan(const char *ns, JSObj& order) {
	Element el = order.findElement("$natural");
	if( el.type() != Number || el.number() >= 0 )
		return DataFileMgr::findAll(ns);

	// "reverse natural order"
	NamespaceDetails *d = nsdetails(ns);
	if( !d )
		return auto_ptr<Cursor>(new BasicCursor(DiskLoc()));
	Extent *e = d->lastExtent.ext();
	while( e->lastRecord.isNull() && !e->xprev.isNull() ) {
		OCCASIONALLY cout << "  findTableScan: extent empty, skipping ahead" << endl;
		e = e->getPrevExtent();
	}
	return auto_ptr<Cursor>(new ReverseCursor( e->lastRecord ));
}

void aboutToDelete(const DiskLoc& dl);

/* drop a collection/namespace */
void dropNS(string& nsToDrop) {
	assert( strstr(nsToDrop.c_str(), ".system.") == 0 );
	{
		// remove from the system catalog
		JSObjBuilder b;
		b.append("name", nsToDrop.c_str());
		JSObj cond = b.done(); // { name: "colltodropname" }
		string system_namespaces = client->name + ".system.namespaces";
		int n = deleteObjects(system_namespaces.c_str(), cond, false, true);
		wassert( n == 1 );
	}
	// remove from the catalog hashtable
	client->namespaceIndex.kill(nsToDrop.c_str());
}

/* delete this index.  does NOT clean up the system catalog
   (system.indexes or system.namespaces) -- only NamespaceIndex.
*/
void IndexDetails::kill() { 
	string ns = indexNamespace(); // e.g. foo.coll.$ts_1

	{
		// clean up in system.indexes
		JSObjBuilder b;
		b.append("name", indexName().c_str());
		b.append("ns", parentNS().c_str());
		JSObj cond = b.done(); // e.g.: { name: "ts_1", ns: "foo.coll" }
		string system_indexes = client->name + ".system.indexes";
		int n = deleteObjects(system_indexes.c_str(), cond, false, true);
		wassert( n == 1 );
	}

	dropNS(ns);
	//	client->namespaceIndex.kill(ns.c_str());
	head.setInvalid();
	info.setInvalid();
}

/* Pull out the relevant key objects from obj, so we
   can index them.  Note that the set is multiple elements 
   only when it's a "multikey" array.
   Keys will be left empty if key not found in the object.
*/
void IndexDetails::getKeysFromObject(JSObj& obj, set<JSObj>& keys) { 
	JSObj keyPattern = info.obj().getObjectField("key");
	if( keyPattern.objsize() == 0 ) {
		cout << keyPattern.toString() << endl;
		cout << info.obj().toString() << endl;
		assert(false);
	}
	JSObjBuilder b;
	JSObj key = obj.extractFields(keyPattern, b);
	if( key.isEmpty() )
		return;
	Element f = key.firstElement();
	if( f.type() != Array ) {
		b.decouple();
		key.iWillFree();
		assert( !key.isEmpty() );
		keys.insert(key);
		return;
	}
	JSObj arr = f.embeddedObject();
//	cout << arr.toString() << endl;
	JSElemIter i(arr);
	while( i.more() ) { 
		Element e = i.next();
		if( e.eoo() ) break;
		JSObjBuilder b;

		b.appendAs(e, f.fieldName());
		JSObj o = b.doneAndDecouple();
		assert( !o.isEmpty() );
		keys.insert(o);
	}
}

int nUnindexes = 0;

void _unindexRecord(const char *ns, IndexDetails& id, JSObj& obj, const DiskLoc& dl) { 
	set<JSObj> keys;
	id.getKeysFromObject(obj, keys);
	for( set<JSObj>::iterator i=keys.begin(); i != keys.end(); i++ ) {
		JSObj j = *i;
//		cout << "UNINDEX: j:" << j.toString() << " head:" << id.head.toString() << dl.toString() << endl;
		if( otherTraceLevel >= 5 ) {
			cout << "_unindexRecord() " << obj.toString();
			cout << "\n  unindex:" << j.toString() << endl;
		}
		nUnindexes++;
		bool ok = false;
		try {
			ok = id.head.btree()->unindex(id.head, id, j, dl);
		}
		catch(AssertionException) { 
			problem() << "Assertion failure: _unindex failed " << id.indexNamespace() << endl;
			cout << "Assertion failure: _unindex failed" << '\n';
			cout << "  obj:" << obj.toString() << '\n';
			cout << "  key:" << j.toString() << '\n';
			cout << "  dl:" << dl.toString() << endl;
			sayDbContext();
		}

		if( !ok ) { 
			cout << "unindex failed (key too big?) " << id.indexNamespace() << '\n';
		}
	}
}

/* unindex all keys in all indexes for this record. */
void  unindexRecord(const char *ns, NamespaceDetails *d, Record *todelete, const DiskLoc& dl) {
	if( d->nIndexes == 0 ) return;
	JSObj obj(todelete);
	for( int i = 0; i < d->nIndexes; i++ ) { 
		_unindexRecord(ns, d->indexes[i], obj, dl);
	}
}

void DataFileMgr::deleteRecord(const char *ns, Record *todelete, const DiskLoc& dl, bool cappedOK) 
{
	dassert( todelete == dl.rec() );

	NamespaceDetails* d = nsdetails(ns);
	if( d->capped && !cappedOK ) { 
		cout << "failing remove on a capped ns " << ns << endl;
		return;
	}

	/* check if any cursors point to us.  if so, advance them. */
	aboutToDelete(dl);

	unindexRecord(ns, d, todelete, dl);

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
		if( e->firstRecord == dl ) {
			if( todelete->nextOfs == DiskLoc::NullOfs )
				e->firstRecord.Null();
			else
				e->firstRecord.setOfs(dl.a(), todelete->nextOfs);
		}
		if( e->lastRecord == dl ) {
			if( todelete->prevOfs == DiskLoc::NullOfs )
				e->lastRecord.Null();
			else
				e->lastRecord.setOfs(dl.a(), todelete->prevOfs);
		}
	}

	/* add to the free list */
	{
		d->nrecords--;
		d->datasize -= todelete->netLength();
		/* temp: if in system.indexes, don't reuse, and zero out: we want to be 
                 careful until validated more, as IndexDetails has pointers
                 to this disk location.  so an incorrectly done remove would cause 
                 a lot of problems. 
        */
		if( strstr(ns, ".system.indexes") ) { 
			memset(todelete, 0, todelete->lengthWithHeaders);
		}
		else {
			DEV memset(todelete->data, 0, todelete->netLength()); // attempt to notice invalid reuse.
			d->addDeletedRec((DeletedRecord*)todelete, dl);
		}
	}
}

void setDifference(set<JSObj>& l, set<JSObj>& r, vector<JSObj*> &diff) { 
	set<JSObj>::iterator i = l.begin();
	set<JSObj>::iterator j = r.begin();
	while( 1 ) { 
		if( i == l.end() )
			break;
		while( j != r.end() && *j < *i )
			j++;
		if( j == r.end() || !i->woEqual(*j) ) {
			const JSObj *jo = &*i;
			diff.push_back( (JSObj *) jo );
		}
		i++;
	}
}

/** Note: as written so far, if the object shrinks a lot, we don't free up space. */
void DataFileMgr::update(
		const char *ns,
		Record *toupdate, const DiskLoc& dl,
		const char *buf, int len, stringstream& ss) 
{
	dassert( toupdate == dl.rec() );

	NamespaceDetails *d = nsdetails(ns);

	if(  toupdate->netLength() < len ) {
		// doesn't fit.  must reallocate.

		if( d && d->capped ) { 
			ss << " failing a growing update on a capped ns " << ns << endl;
			return;
		}

		d->paddingTooSmall();
		if( client->profile )
			ss << " moved ";
		deleteRecord(ns, toupdate, dl);
		insert(ns, buf, len);
		return;
	}

	d->paddingFits();

	/* has any index keys changed? */
	{
		NamespaceDetails *d = nsdetails(ns);
		if( d->nIndexes ) {
			JSObj newObj(buf);
			JSObj oldObj = dl.obj();
			for( int i = 0; i < d->nIndexes; i++ ) {
				IndexDetails& idx = d->indexes[i];
				JSObj idxKey = idx.info.obj().getObjectField("key");

				set<JSObj> oldkeys;
				set<JSObj> newkeys;
				idx.getKeysFromObject(oldObj, oldkeys);
				idx.getKeysFromObject(newObj, newkeys);
				vector<JSObj*> removed;
				setDifference(oldkeys, newkeys, removed);
				string idxns = idx.indexNamespace();
				for( unsigned i = 0; i < removed.size(); i++ ) {
					try {  
						idx.head.btree()->unindex(idx.head, idx, *removed[i], dl);
					}
					catch(AssertionException) { 
						ss << " exception update unindex ";
						problem() << " caught assertion update unindex " << idxns.c_str() << endl;
					}
				}
				vector<JSObj*> added;
				setDifference(newkeys, oldkeys, added);
				assert( !dl.isNull() );
				for( unsigned i = 0; i < added.size(); i++ ) { 
					try {
						idx.head.btree()->insert(
							idx.head, 
							dl, *added[i], false, idx, true);
					}
					catch(AssertionException) {
						ss << " exception update index "; 
						cout << " caught assertion update index " << idxns.c_str() << '\n';
						problem() << " caught assertion update index " << idxns.c_str() << endl;
					}
				}
				if( client->profile )
					ss << "<br>" << added.size() << " key updates ";

			}
		}
	}

	//	update in place
	memcpy(toupdate->data, buf, len);
}

int followupExtentSize(int len, int lastExtentLen) {
	int x = initialExtentSize(len);
	int y = (int) (lastExtentLen < 4000000 ? lastExtentLen * 4.0 : lastExtentLen * 1.2);
	int sz = y > x ? y : x;
	sz = ((int)sz) & 0xffffff00;
	assert( sz > len );
	return sz;
}

int deb=0;

/* add keys to indexes for a new record */
void  _indexRecord(IndexDetails& idx, JSObj& obj, DiskLoc newRecordLoc) { 

	set<JSObj> keys;
	idx.getKeysFromObject(obj, keys);
	for( set<JSObj>::iterator i=keys.begin(); i != keys.end(); i++ ) {
		assert( !newRecordLoc.isNull() );
		try {
//			DEBUGGING << "temp index: " << newRecordLoc.toString() << obj.toString() << endl;
			idx.head.btree()->insert(idx.head, newRecordLoc,
				(JSObj&) *i, false, idx, true);
		}
		catch(AssertionException) { 
			problem() << " caught assertion _indexRecord " << idx.indexNamespace() << endl;
		}
	}
}

/* note there are faster ways to build an index in bulk, that can be 
   done eventually */
void addExistingToIndex(const char *ns, IndexDetails& idx) {
	cout << "Adding all existing records for " << ns << " to new index" << endl;
	int n = 0;
	auto_ptr<Cursor> c = theDataFileMgr.findAll(ns);
	while( c->ok() ) {
		JSObj js = c->current();
		_indexRecord(idx, js, c->currLoc());
		c->advance();
		n++;
	};
	cout << "  indexing complete for " << n << " records" << endl;
}

/* add keys to indexes for a new record */
void  indexRecord(NamespaceDetails *d, const void *buf, int len, DiskLoc newRecordLoc) { 
	JSObj obj((const char *)buf);
	for( int i = 0; i < d->nIndexes; i++ ) { 
		_indexRecord(d->indexes[i], obj, newRecordLoc);
	}
}

extern JSObj emptyObj;

DiskLoc DataFileMgr::insert(const char *ns, const void *buf, int len, bool god) {
	bool addIndex = false;
	const char *sys = strstr(ns, "system.");
	if( sys ) { 
		if( sys == ns ) {
			cout << "ERROR: attempt to insert for invalid client 'system': " << ns << endl;
			return DiskLoc();
		}
		if( strstr(ns, ".system.") ) { 
			if( strstr(ns, ".system.indexes") )
				addIndex = true;
			else if( !god ) { 
				cout << "ERROR: attempt to insert in system namespace " << ns << endl;
				return DiskLoc();
			}
		}
	}

	NamespaceDetails *d = nsdetails(ns);
	if( d == 0 ) {
		addNewNamespaceToCatalog(ns);
		/* todo: shouldn't be in the namespace catalog until after the allocations here work. 
                 also if this is an addIndex, those checks should happen before this!
		*/
		client->newestFile()->newExtent(ns, initialExtentSize(len));
		d = nsdetails(ns);
	}
	d->paddingFits();

	NamespaceDetails *tableToIndex = 0;

	const char *tabletoidxns = 0;
	if( addIndex ) { 
		JSObj io((const char *) buf);
		const char *name = io.getStringField("name"); // name of the index
		tabletoidxns = io.getStringField("ns");  // table it indexes
		JSObj key = io.getObjectField("key");
		if( name == 0 || *name == 0 || tabletoidxns == 0 || key.isEmpty() || key.objsize() > 2048 ) { 
			cout << "user warning: bad add index attempt name:" << (name?name:"") << "\n  ns:" << 
				(tabletoidxns?tabletoidxns:"") << "\n  ourns:" << ns;
			cout << "\n  idxobj:" << io.toString() << endl;
			return DiskLoc();
		}
		tableToIndex = nsdetails(tabletoidxns);
		if( tableToIndex == 0 ) {
			// try to create it
			string err;
			if( !userCreateNS(tabletoidxns, emptyObj, err) ) { 
				cout << "ERROR: failed to create collection while adding its index. " << tabletoidxns << endl;
				return DiskLoc();
			}
			tableToIndex = nsdetails(tabletoidxns);
			cout << "info: creating collection " << tabletoidxns << " on add index\n";
			assert( tableToIndex );
		}
		if( tableToIndex->nIndexes >= MaxIndexes ) { 
			cout << "user warning: bad add index attempt, too many indexes for:" << tabletoidxns << endl;
			return DiskLoc();
		}
		if( tableToIndex->findIndexByName(name) >= 0 ) { 
			//cout << "INFO: index:" << name << " already exists for:" << tabletoidxns << endl;
			return DiskLoc();
		}
		//indexFullNS = tabletoidxns; 
		//indexFullNS += ".$";
		//indexFullNS += name; // client.table.$index -- note this doesn't contain jsobjs, it contains BtreeBuckets.
	}

	DiskLoc extentLoc;
	int lenWHdr = len + Record::HeaderSize;
	lenWHdr = (int) (lenWHdr * d->paddingFactor);
	if( lenWHdr == 0 ) {
		// old datafiles, backward compatible here.
		assert( d->paddingFactor == 0 );
		d->paddingFactor = 1.0;
		lenWHdr = len + Record::HeaderSize;
	}
	DiskLoc loc = d->alloc(ns, lenWHdr, extentLoc);
	if( loc.isNull() ) {
		// out of space
		if( d->capped == 0 ) { // size capped doesn't grow
			cout << "allocating new extent for " << ns << " padding:" << d->paddingFactor << endl;
			client->newestFile()->newExtent(ns, followupExtentSize(len, d->lastExtentSize));
			loc = d->alloc(ns, lenWHdr, extentLoc);
		}
		if( loc.isNull() ) { 
			cout << "out of space in datafile. capped:" << d->capped << endl;
			assert(d->capped);
			return DiskLoc();
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

	d->nrecords++;
	d->datasize += r->netLength();

	if( tableToIndex ) { 
		IndexDetails& idxinfo = tableToIndex->indexes[tableToIndex->nIndexes];
		idxinfo.info = loc;
		idxinfo.head = BtreeBucket::addHead(idxinfo);
		tableToIndex->nIndexes++; 
		/* todo: index existing records here */
		addExistingToIndex(tabletoidxns, idxinfo);
	}

	/* add this record to our indexes */
	if( d->nIndexes )
		indexRecord(d, buf, len, loc);

//	cout << "   inserted at loc:" << hex << loc.getOfs() << " lenwhdr:" << hex << lenWHdr << dec << ' ' << ns << endl;
	return loc;
}

void DataFileMgr::init(const char *dir) {
/*	string path = dir;
	path += "temp.dat";
	temp.open(path.c_str(), 64 * 1024 * 1024);
*/
}

void pdfileInit() {
//	namespaceIndex.init(dbpath);
	theDataFileMgr.init(dbpath);
}

#include "clientcursor.h"

void dropDatabase(const char *ns) { 
	// ns is of the form "<dbname>.$cmd"
	char cl[256];
	nsToClient(ns, cl);
	problem() << "dropDatabase " << cl << endl;
	assert( client->name == cl );

	/* important: kill all open cursors on the database */
	string prefix(cl);
	prefix += '.';
	ClientCursor::invalidate(prefix.c_str());

	clients.erase(cl);
	delete client; // closes files
	client = 0;
	_deleteDataFiles(cl);
}
