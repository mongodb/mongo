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
#include "objwrappers.h"
#include "btree.h"
#include <algorithm>

const char *dbpath = "/data/db/";

DataFileMgr theDataFileMgr;
map<string,Client*> clients;
Client *client;
const char *curNs = "";
int MAGIC = 0x1000;
int curOp = -2;

extern int otherTraceLevel;

void sayDbContext() { 
	cout << " client: " << (client ? client->name.c_str() : "null");
	cout << " op:" << curOp << endl;
	if( client )
		cout << " ns: " << curNs << endl;
}

JSObj::JSObj(Record *r) { 
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
}

/*---------------------------------------------------------------------*/ 

int bucketSizes[] = { 
	32, 64, 128, 256, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000,
	0x8000, 0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000,
	0x400000, 0x800000
};

//NamespaceIndexMgr namespaceIndexMgr;

void NamespaceDetails::addDeletedRec(DeletedRecord *d, DiskLoc dloc) { 
	int b = bucket(d->lengthWithHeaders);
	DiskLoc& list = deletedList[b];
	DiskLoc oldHead = list;
	list = dloc;
	d->nextDeleted = oldHead;
}

/* lenToAlloc is WITH header */
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

void sayDbContext();

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
		int a = cur.a();
		if( a < -1 || a >= 100000 ) { 
			cout << "Assertion failure - a() out of range in _alloc() " << a << endl;
			sayDbContext();
			cur.Null();
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

void newNamespace(const char *ns) {
	cout << "New namespace: " << ns << endl;
	if( strstr(ns, "system.namespaces") == 0 ) {
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

/*---------------------------------------------------------------------*/ 

void PhysicalDataFile::open(int fn, const char *filename) {
	int length;
	/*TEMP!
	if( fn <= 48 )  { 
		length = 16 * 1024 * 1024;
	}
	else */
	if( fn <= 4 ) {
		length = (64*1024*1024) << fn;
		if( strstr(filename, "alleyinsider") && length < 512 * 1024 * 1024 )
			length = 512 * 1024 * 1024;
	} else
		length = 0x7ff00000;

	assert( length >= 64*1024*1024 && length % 4096 == 0 );

	assert(fn == fileNo);
	header = (PDFHeader *) mmf.map(filename, length);
	assert(header);
	header->init(fileNo, length);
}

/* prev - previous extent for this namespace.  null=this is the first one. */
Extent* PhysicalDataFile::newExtent(const char *ns, int approxSize) {
	int ExtentSize = approxSize <= header->unusedLength ? approxSize : header->unusedLength;
	DiskLoc loc;
	if( ExtentSize <= 0 ) {
		cout << "INFO: newExtent(): file full, adding a new file " << ns << endl;
		return client->addAFile()->newExtent(ns, approxSize);
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
		cout << "info: findAll() namespace does not exist: " << ns << endl;
		return auto_ptr<Cursor>(new BasicCursor(DiskLoc()));
	}
	Extent *e = getExtent(loc);
	while( e->firstRecord.isNull() && !e->xnext.isNull() ) {
		cout << "  DFM::findAll(): extent empty, skipping ahead" << endl;
		// find a nonempty extent
		// it might be nice to free the whole extent here!  but have to clean up free recs then.
		e = e->getNextExtent();
	}
	return auto_ptr<Cursor>(new BasicCursor( e->firstRecord ));
}

void aboutToDelete(const DiskLoc& dl);

/* pull out the relevant key objects from obj, so we
   can index them.  Note that the set is multiple elements 
   only when it's a "multikey" array.
   keys will be left empty if key not found in the object.
*/
void IndexDetails::getKeysFromObject(JSObj& obj, set<JSObj>& keys) { 
	JSObj keyPattern = info.obj().getObjectField("key");
	JSObjBuilder b;
	JSObj key = obj.extractFields(keyPattern, b);
	if( key.isEmpty() )
		return;
	Element f = key.firstElement();
	if( f.type() != Array ) {
		b.decouple();
		key.iWillFree();
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
		keys.insert(o);
	}
}

int nUnindexes = 0;

void _unindexRecord(const char *ns, IndexDetails& id, JSObj& obj, const DiskLoc& dl) { 
	set<JSObj> keys;
	id.getKeysFromObject(obj, keys);
	for( set<JSObj>::iterator i=keys.begin(); i != keys.end(); i++ ) {
		JSObj j = *i;
//		cout << "TEMP: j:" << j.toString() << endl;
		if( otherTraceLevel >= 5 ) {
			cout << "_unindexRecord() " << obj.toString();
			cout << "\n  unindex:" << j.toString() << endl;
		}
		nUnindexes++;
		bool ok = false;
		try {
			ok = id.head.btree()->unindex(id.head, ns, j, dl);
		}
		catch(AssertionException) { 
			cout << " caught assertion _unindexRecord " << id.indexNamespace() << '\n';
		}

#if defined(_WIN32)
		id.head.btree()->fullValidate(id.head);
#endif
		if( !ok ) { 
			cout << "Assertion failure: _unindex failed" << '\n';
			cout << "  obj:" << obj.toString() << '\n';
			cout << "  key:" << j.toString() << '\n';
			cout << "  dl:" << dl.toString() << endl;
		}
	}
}

void  unindexRecord(const char *ns, NamespaceDetails *d, Record *todelete, const DiskLoc& dl) {
	if( d->nIndexes == 0 ) return;
	JSObj obj(todelete);
	for( int i = 0; i < d->nIndexes; i++ ) { 
		_unindexRecord(ns, d->indexes[i], obj, dl);
	}
}

void DataFileMgr::deleteRecord(const char *ns, Record *todelete, const DiskLoc& dl) 
{
	/* check if any cursors point to us.  if so, advance them. */
	aboutToDelete(dl);

	NamespaceDetails* d = nsdetails(ns);
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
		d->addDeletedRec((DeletedRecord*)todelete, dl);
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
		const char *buf, int len) 
{
	if( toupdate->netLength() < len ) {
		// doesn't fit.
		deleteRecord(ns, toupdate, dl);
		insert(ns, buf, len);
		return;
	}

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
						idx.head.btree()->unindex(idx.head, idxns.c_str(), *removed[i], dl);
					}
					catch(AssertionException) { 
						cout << " caught assertion update unindex " << idxns.c_str() << '\n';
					}
				}
				vector<JSObj*> added;
				setDifference(newkeys, oldkeys, added);
				assert( !dl.isNull() );
				for( unsigned i = 0; i < added.size(); i++ ) { 
					try {
						idx.head.btree()->insert(
							idx.head, idxns.c_str(),
							dl, *added[i], false, idx, true);
					}
					catch(AssertionException) { 
						cout << " caught assertion update index " << idxns.c_str() << '\n';
					}
				}

			}
		}
	}

//	cout << "doing update in place" << endl;
	memcpy(toupdate->data, buf, len);
}

int initialExtentSize(int len) { 
//	if( 1 )
//		return 4000000;
	long long sz = len * 16;
	if( len < 1000 ) sz = len * 64;
	if( sz > 1000000000 )
		sz = 1000000000;
	int z = ((int)sz) & 0xffffff00;
	assert( z > len );
	cout << "initialExtentSize(" << len << ") returns " << z << endl;
	return z;
}

int followupExtentSize(int len, int lastExtentLen) {
	int x = initialExtentSize(len);
	int y = (int) (lastExtentLen < 4000000 ? lastExtentLen * 4.0 : lastExtentLen * 1.2);
	int sz = y > x ? y : x;
	sz = ((int)sz) & 0xffffff00;
	assert( sz > len );
	return sz;
}

/* add keys to indexes for a new record */
void  _indexRecord(IndexDetails& idx, JSObj& obj, DiskLoc newRecordLoc) { 
	set<JSObj> keys;
	idx.getKeysFromObject(obj, keys);
	for( set<JSObj>::iterator i=keys.begin(); i != keys.end(); i++ ) {
		//		cout << "temp: _indexRecord " << i->toString() << endl;
		assert( !newRecordLoc.isNull() );
		try {
			idx.head.btree()->insert(idx.head, idx.indexNamespace().c_str(), newRecordLoc,
				(JSObj&) *i, false, idx, true);
		}
		catch(AssertionException) { 
			cout << " caught assertion _indexRecord " << idx.indexNamespace() << '\n';
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

DiskLoc DataFileMgr::insert(const char *ns, const void *buf, int len, bool god) {
	if( 0 ) { //strcmp(ns, "test.z") == 0 ) {
		JSObj obj((const char *) buf);
		cout << "  insert:" << obj.toString() << endl;
	}

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
		newNamespace(ns);
		client->newestFile()->newExtent(ns, initialExtentSize(len));
		d = nsdetails(ns);
	}

	NamespaceDetails *tableToIndex = 0;
	string indexFullNS;
	const char *tabletoidxns = 0;
	if( addIndex ) { 
		JSObj io((const char *) buf);
		const char *name = io.getStringField("name"); // name of the index
		tabletoidxns = io.getStringField("ns");  // table it indexes
		JSObj key = io.getObjectField("key");
		if( name == 0 || *name == 0 || tabletoidxns == 0 || key.isEmpty() || key.objsize() > 2048 ) { 
			cout << "user warning: bad add index attempt name:" << (name?name:"") << " ns:" << 
				(tabletoidxns?tabletoidxns:"") << endl;
			return DiskLoc();
		}
		tableToIndex = nsdetails(tabletoidxns);
		if( tableToIndex == 0 ) {
			cout << "ERROR: bad add index attempt, no such table(ns):" << tabletoidxns << endl;
			return DiskLoc();
		}
		if( tableToIndex->nIndexes >= MaxIndexes ) { 
			cout << "ERROR: bad add index attempt, too many indexes for:" << tabletoidxns << endl;
			return DiskLoc();
		}
		if( tableToIndex->findIndexByName(name) >= 0 ) { 
			//cout << "INFO: index:" << name << " already exists for:" << tabletoidxns << endl;
			return DiskLoc();
		}
		indexFullNS = tabletoidxns; 
		indexFullNS += ".$";
		indexFullNS += name; // client.table.$index -- note this doesn't contain jsobjs, it contains BtreeBuckets.
	}

	DiskLoc extentLoc;
	int lenWHdr = len + Record::HeaderSize;
	DiskLoc loc = d->alloc(lenWHdr, extentLoc);
	if( loc.isNull() ) {
		// out of space
		cout << "allocating new extent for " << ns << endl;
		client->newestFile()->newExtent(ns, followupExtentSize(len, d->lastExtentSize));
		loc = d->alloc(lenWHdr, extentLoc);
		if( loc.isNull() ) { 
			cout << "****** ERROR: out of space in datafile.  write more code." << endl;
			assert(false);
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
		idxinfo.head = BtreeBucket::addHead(indexFullNS.c_str());
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
