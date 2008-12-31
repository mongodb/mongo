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

_ disallow system* manipulations from the database.
*/

#include "stdafx.h"
#include "pdfile.h"
#include "db.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"
#include "btree.h"
#include <algorithm>
#include <list>
#include "query.h"
#include "repl.h"

extern bool quota;
extern int port;

const char *dbpath = "/data/db/";

DataFileMgr theDataFileMgr;
map<string,Database*> databases;
Database *database;
const char *curNs = "";
int MAGIC = 0x1000;
int curOp = -2;
int callDepth = 0;

extern int otherTraceLevel;
void addNewNamespaceToCatalog(const char *ns, BSONObj *options = 0);

string getDbContext() {
    stringstream ss;
    if ( database ) {
        ss << database->name << ' ';
        if ( curNs )
            ss << curNs << ' ';
    }
    ss<< "op:" << curOp << ' ' << callDepth;
    return ss.str();
}

/* this is a good place to set a breakpoint when debugging, as lots of warning things
   (assert, wassert) call it.
*/
void sayDbContext(const char *errmsg) {
    if ( errmsg ) {
        problem() << errmsg << endl;
    }
    log() << ' ' << getDbContext() << '\n';
    printStackTrace();
}

BSONObj::BSONObj(Record *r) {
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

int initialExtentSize(int len) {
    long long sz = len * 16;
    if ( len < 1000 ) sz = len * 64;
    if ( sz > 1000000000 )
        sz = 1000000000;
    int z = ((int)sz) & 0xffffff00;
    assert( z > len );
    DEV log() << "initialExtentSize(" << len << ") returns " << z << endl;
    return z;
}

bool _userCreateNS(const char *ns, BSONObj& j, string& err) {
    if ( nsdetails(ns) ) {
        err = "collection already exists";
        return false;
    }

    if( verbose )
        log() << "create collection " << ns << ' ' << j.toString() << '\n';

    /* todo: do this only when we have allocated space successfully? or we could insert with a { ok: 0 } field
             and then go back and set to ok : 1 after we are done.
    */
    addNewNamespaceToCatalog(ns, j.isEmpty() ? 0 : &j);

    int ies = initialExtentSize(128);
    BSONElement e = j.findElement("size");
    if ( e.isNumber() ) {
        ies = (int) e.number();
        ies += 256;
        ies &= 0xffffff00;
        if ( ies > 1024 * 1024 * 1024 + 256 ) return false;
    }

    database->suitableFile(ies)->newExtent(ns, ies);
    NamespaceDetails *d = nsdetails(ns);
    assert(d);

    e = j.findElement("capped");
    if ( e.type() == Bool && e.boolean() ) {
        d->capped = 1;
        e = j.findElement("max");
        if ( e.isNumber() ) {
            int mx = (int) e.number();
            if ( mx > 0 )
                d->max = mx;
        }
    }

    return true;
}

// { ..., capped: true, size: ..., max: ... }
// returns true if successful
bool userCreateNS(const char *ns, BSONObj j, string& err, bool logForReplication) {
    j.validateEmpty();
    bool ok = _userCreateNS(ns, j, err);
    if ( logForReplication && ok )
        logOp("c", ns, j);
    return ok;
}

/*---------------------------------------------------------------------*/

void PhysicalDataFile::open(int fn, const char *filename) {
    {
        /* check quotas
           very simple temporary implementation - we will in future look up
           the quota from the grid database
        */
        if ( quota && fn > 8 && !boost::filesystem::exists(filename) ) {
            /* todo: if we were adding / changing keys in an index did we do some
               work previously that needs cleaning up?  Possible.  We should
               check code like that and have it catch the exception and do
               something reasonable.
            */
            string s = "db disk space quota exceeded ";
            if ( database )
                s += database->name;
            uasserted(s.c_str());
        }
    }

    int length;

    if ( fn <= 4 ) {
        length = (64*1024*1024) << fn;
        if ( strstr(filename, "alleyinsider") && length < 1024 * 1024 * 1024 ) {
            DEV cout << "Warning: not making alleyinsider datafile bigger because DEV is true" << endl;
            else
                length = 1024 * 1024 * 1024;
        }
    } else
        length = 0x7ff00000;

    if ( sizeof( int* ) == 4 && fn > 4 )
        length = 512 * 1024 * 1024;

    assert( length >= 64*1024*1024 );

    if ( strstr(filename, "_hudsonSmall") ) {
        int mult = 1;
        if ( fn > 1 && fn < 1000 )
            mult = fn;
        length = 1024 * 512 * mult;
        log() << "Warning : using small files for _hudsonSmall" << endl;
    }
    assert( length % 4096 == 0 );

    assert(fn == fileNo);
    header = (PDFHeader *) mmf.map(filename, length);
    uassert("can't map file memory", header);
    header->init(fileNo, length);
}

/* prev - previous extent for this namespace.  null=this is the first one. */
Extent* PhysicalDataFile::newExtent(const char *ns, int approxSize, int loops) {
    assert( approxSize >= 0 && approxSize <= 0x7ff00000 );

    assert( header ); // null if file open failed
    int ExtentSize = approxSize <= header->unusedLength ? approxSize : header->unusedLength;
    DiskLoc loc;
    if ( ExtentSize <= 0 ) {
        /* not there could be a lot of looping here is db just started and
           no files are open yet.  we might want to do something about that. */
        if ( loops > 8 ) {
            assert( loops < 10000 );
            cout << "warning: loops=" << loops << " fileno:" << fileNo << ' ' << ns << '\n';
        }
        log() << "newExtent: " << ns << " file " << fileNo << " full, adding a new file\n";
        return database->addAFile()->newExtent(ns, approxSize, loops+1);
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
    if ( details ) {
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

    DEV log() << "new extent " << ns << " size: 0x" << hex << ExtentSize << " loc: 0x" << hex << offset
    << " emptyLoc:" << hex << emptyLoc.getOfs() << dec << endl;
    return e;
}

/*---------------------------------------------------------------------*/

/* assumes already zeroed -- insufficient for block 'reuse' perhaps */
DiskLoc Extent::init(const char *nsname, int _length, int _fileNo, int _offset) {
    magic = 0x41424344;
    myLoc.setOfs(_fileNo, _offset);
    xnext.Null();
    xprev.Null();
    ns = nsname;
    length = _length;
    firstRecord.Null();
    lastRecord.Null();

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
    if ( !found ) {
        //		cout << "info: findAll() namespace does not exist: " << ns << endl;
        return auto_ptr<Cursor>(new BasicCursor(DiskLoc()));
    }

    Extent *e = getExtent(loc);

    DEBUGGING {
        cout << "listing extents for " << ns << endl;
        DiskLoc tmp = loc;
        set<DiskLoc> extents;

        while ( 1 ) {
            Extent *f = getExtent(tmp);
            cout << "extent: " << tmp.toString() << endl;
            extents.insert(tmp);
            tmp = f->xnext;
            if ( tmp.isNull() )
                break;
            f = f->getNextExtent();
        }

        cout << endl;
        nsdetails(ns)->dumpDeleted(&extents);
    }

    while ( e->firstRecord.isNull() && !e->xnext.isNull() ) {
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
auto_ptr<Cursor> findTableScan(const char *ns, BSONObj& order, bool *isSorted) {
    BSONElement el = order.findElement("$natural"); // e.g., { $natural : -1 }
    if ( !el.eoo() && isSorted )
        *isSorted = true;

    if ( el.number() >= 0 )
        return DataFileMgr::findAll(ns);

    // "reverse natural order"
    NamespaceDetails *d = nsdetails(ns);
    if ( !d )
        return auto_ptr<Cursor>(new BasicCursor(DiskLoc()));
    Extent *e = d->lastExtent.ext();
    while ( e->lastRecord.isNull() && !e->xprev.isNull() ) {
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
        BSONObjBuilder b;
        b.append("name", nsToDrop.c_str());
        BSONObj cond = b.done(); // { name: "colltodropname" }
        string system_namespaces = database->name + ".system.namespaces";
        int n = deleteObjects(system_namespaces.c_str(), cond, false, true);
        wassert( n == 1 );
    }
    // remove from the catalog hashtable
    database->namespaceIndex.kill(nsToDrop.c_str());
}

/* delete this index.  does NOT clean up the system catalog
   (system.indexes or system.namespaces) -- only NamespaceIndex.
*/
void IndexDetails::kill() {
    string ns = indexNamespace(); // e.g. foo.coll.$ts_1

    {
        // clean up in system.indexes
        BSONObjBuilder b;
        b.append("name", indexName().c_str());
        b.append("ns", parentNS().c_str());
        BSONObj cond = b.done(); // e.g.: { name: "ts_1", ns: "foo.coll" }
        string system_indexes = database->name + ".system.indexes";
        int n = deleteObjects(system_indexes.c_str(), cond, false, true);
        wassert( n == 1 );
    }

    dropNS(ns);
    //	database->namespaceIndex.kill(ns.c_str());
    head.setInvalid();
    info.setInvalid();
}

/* Pull out the relevant key objects from obj, so we
   can index them.  Note that the set is multiple elements
   only when it's a "multikey" array.
   Keys will be left empty if key not found in the object.
*/
void IndexDetails::getKeysFromObject( const BSONObj& obj, set<BSONObj>& keys) const {
    BSONObj keyPattern = info.obj().getObjectField("key"); // e.g., keyPattern == { ts : 1 }
    if ( keyPattern.objsize() == 0 ) {
        cout << keyPattern.toString() << endl;
        cout << info.obj().toString() << endl;
        assert(false);
    }
    BSONObjBuilder b;
    const char *nameWithinArray;
    BSONObj key = obj.extractFieldsDotted(keyPattern, b, nameWithinArray);
    if ( key.isEmpty() )
        return;
    BSONObjIterator keyIter( key );
    BSONElement arrayElt;
    int arrayPos = -1;
    for ( int i = 0; keyIter.more(); ++i ) {
        BSONElement e = keyIter.next();
        if ( e.eoo() )
            break;
        if ( e.type() == Array ) {
            uassert( "Index cannot be created on parallel arrays.",
                     arrayPos == -1 );
            arrayPos = i;
            arrayElt = e;
        }
    }
    if ( arrayPos == -1 ) {
        assert( strlen( nameWithinArray ) == 0 );
        BSONObjBuilder b;
        BSONObjIterator keyIter( key );
        while ( keyIter.more() ) {
            BSONElement f = keyIter.next();
            if ( f.eoo() )
                break;
            b.append( f );
        }
        BSONObj o = b.doneAndDecouple();
        assert( !o.isEmpty() );
        keys.insert(o);
        return;
    }
    BSONObj arr = arrayElt.embeddedObject();
    BSONObjIterator arrIter(arr);
    while ( arrIter.more() ) {
        BSONElement e = arrIter.next();
        if ( e.eoo() )
            break;

        if ( strlen( nameWithinArray ) != 0 ) {
            e = e.embeddedObject().getFieldDotted( nameWithinArray );
            if ( e.eoo() )
                continue;
        }
        BSONObjBuilder b;
        BSONObjIterator keyIter( key );
        for ( int i = 0; keyIter.more(); ++i ) {
            BSONElement f = keyIter.next();
            if ( f.eoo() )
                break;
            if ( i != arrayPos )
                b.append( f );
            else
                b.appendAs( e, "" );
        }

        BSONObj o = b.doneAndDecouple();
        assert( !o.isEmpty() );
        keys.insert(o);
    }
}

int nUnindexes = 0;

void _unindexRecord(const char *ns, IndexDetails& id, BSONObj& obj, const DiskLoc& dl) {
    set<BSONObj> keys;
    id.getKeysFromObject(obj, keys);
    for ( set<BSONObj>::iterator i=keys.begin(); i != keys.end(); i++ ) {
        BSONObj j = *i;
//		cout << "UNINDEX: j:" << j.toString() << " head:" << id.head.toString() << dl.toString() << endl;
        if ( otherTraceLevel >= 5 ) {
            cout << "_unindexRecord() " << obj.toString();
            cout << "\n  unindex:" << j.toString() << endl;
        }
        nUnindexes++;
        bool ok = false;
        try {
            ok = id.head.btree()->unindex(id.head, id, j, dl);
        }
        catch (AssertionException&) {
            problem() << "Assertion failure: _unindex failed " << id.indexNamespace() << endl;
            cout << "Assertion failure: _unindex failed" << '\n';
            cout << "  obj:" << obj.toString() << '\n';
            cout << "  key:" << j.toString() << '\n';
            cout << "  dl:" << dl.toString() << endl;
            sayDbContext();
        }

        if ( !ok ) {
            cout << "unindex failed (key too big?) " << id.indexNamespace() << '\n';
        }
    }
}

/* unindex all keys in all indexes for this record. */
void  unindexRecord(const char *ns, NamespaceDetails *d, Record *todelete, const DiskLoc& dl) {
    if ( d->nIndexes == 0 ) return;
    BSONObj obj(todelete);
    for ( int i = 0; i < d->nIndexes; i++ ) {
        _unindexRecord(ns, d->indexes[i], obj, dl);
    }
}

void DataFileMgr::deleteRecord(const char *ns, Record *todelete, const DiskLoc& dl, bool cappedOK)
{
    dassert( todelete == dl.rec() );

    NamespaceDetails* d = nsdetails(ns);
    if ( d->capped && !cappedOK ) {
        cout << "failing remove on a capped ns " << ns << endl;
        return;
    }

    /* check if any cursors point to us.  if so, advance them. */
    aboutToDelete(dl);

    unindexRecord(ns, d, todelete, dl);

    /* remove ourself from the record next/prev chain */
    {
        if ( todelete->prevOfs != DiskLoc::NullOfs )
            todelete->getPrev(dl).rec()->nextOfs = todelete->nextOfs;
        if ( todelete->nextOfs != DiskLoc::NullOfs )
            todelete->getNext(dl).rec()->prevOfs = todelete->prevOfs;
    }

    /* remove ourself from extent pointers */
    {
        Extent *e = todelete->myExtent(dl);
        if ( e->firstRecord == dl ) {
            if ( todelete->nextOfs == DiskLoc::NullOfs )
                e->firstRecord.Null();
            else
                e->firstRecord.setOfs(dl.a(), todelete->nextOfs);
        }
        if ( e->lastRecord == dl ) {
            if ( todelete->prevOfs == DiskLoc::NullOfs )
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
        if ( strstr(ns, ".system.indexes") ) {
            memset(todelete, 0, todelete->lengthWithHeaders);
        }
        else {
            DEV memset(todelete->data, 0, todelete->netLength()); // attempt to notice invalid reuse.
            d->addDeletedRec((DeletedRecord*)todelete, dl);
        }
    }
}

void setDifference(set<BSONObj>& l, set<BSONObj>& r, vector<BSONObj*> &diff) {
    set<BSONObj>::iterator i = l.begin();
    set<BSONObj>::iterator j = r.begin();
    while ( 1 ) {
        if ( i == l.end() )
            break;
        while ( j != r.end() && *j < *i )
            j++;
        if ( j == r.end() || !i->woEqual(*j) ) {
            const BSONObj *jo = &*i;
            diff.push_back( (BSONObj *) jo );
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

    if (  toupdate->netLength() < len ) {
        // doesn't fit.  must reallocate.

        if ( d && d->capped ) {
            ss << " failing a growing update on a capped ns " << ns << endl;
            return;
        }

        d->paddingTooSmall();
        if ( database->profile )
            ss << " moved ";
        deleteRecord(ns, toupdate, dl);
        insert(ns, buf, len);
        return;
    }

    d->paddingFits();

    /* has any index keys changed? */
    {
        NamespaceDetails *d = nsdetails(ns);
        if ( d->nIndexes ) {
            BSONObj newObj(buf);
            BSONObj oldObj = dl.obj();
            for ( int i = 0; i < d->nIndexes; i++ ) {
                IndexDetails& idx = d->indexes[i];
                BSONObj idxKey = idx.info.obj().getObjectField("key");

                set<BSONObj> oldkeys;
                set<BSONObj> newkeys;
                idx.getKeysFromObject(oldObj, oldkeys);
                idx.getKeysFromObject(newObj, newkeys);
                vector<BSONObj*> removed;
                setDifference(oldkeys, newkeys, removed);
                string idxns = idx.indexNamespace();
                for ( unsigned i = 0; i < removed.size(); i++ ) {
                    try {
                        idx.head.btree()->unindex(idx.head, idx, *removed[i], dl);
                    }
                    catch (AssertionException&) {
                        ss << " exception update unindex ";
                        problem() << " caught assertion update unindex " << idxns.c_str() << endl;
                    }
                }
                vector<BSONObj*> added;
                setDifference(newkeys, oldkeys, added);
                assert( !dl.isNull() );
                for ( unsigned i = 0; i < added.size(); i++ ) {
                    try {
                        idx.head.btree()->insert(
                            idx.head,
                            dl, *added[i], false, idx, true);
                    }
                    catch (AssertionException&) {
                        ss << " exception update index ";
                        cout << " caught assertion update index " << idxns.c_str() << '\n';
                        problem() << " caught assertion update index " << idxns.c_str() << endl;
                    }
                }
                if ( database->profile )
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
void  _indexRecord(IndexDetails& idx, BSONObj& obj, DiskLoc newRecordLoc) {

    set<BSONObj> keys;
    idx.getKeysFromObject(obj, keys);
    for ( set<BSONObj>::iterator i=keys.begin(); i != keys.end(); i++ ) {
        assert( !newRecordLoc.isNull() );
        try {
            idx.head.btree()->insert(idx.head, newRecordLoc,
                                     (BSONObj&) *i, false, idx, true);
        }
        catch (AssertionException&) {
            problem() << " caught assertion _indexRecord " << idx.indexNamespace() << endl;
        }
    }
}

/* note there are faster ways to build an index in bulk, that can be
   done eventually */
void addExistingToIndex(const char *ns, IndexDetails& idx) {
    Timer t;
    Logstream& l = log();
    l << "building new index for " << ns << " ... ";
    l.flush();
    int n = 0;
    auto_ptr<Cursor> c = theDataFileMgr.findAll(ns);
    while ( c->ok() ) {
        BSONObj js = c->current();
        _indexRecord(idx, js, c->currLoc());
        c->advance();
        n++;
    };
    l << "done for " << n << " records" << endl;
}

/* add keys to indexes for a new record */
void  indexRecord(NamespaceDetails *d, const void *buf, int len, DiskLoc newRecordLoc) {
    BSONObj obj((const char *)buf);
    for ( int i = 0; i < d->nIndexes; i++ ) {
        _indexRecord(d->indexes[i], obj, newRecordLoc);
    }
}

extern BSONObj emptyObj;
extern BSONObj id_obj; // = fromjson("{_id:ObjId()}");

void ensureHaveIdIndex(const char *ns) {
    NamespaceDetails *d = nsdetails(ns);
    if ( d == 0 || (d->flags & NamespaceDetails::Flag_HaveIdIndex) )
        return;

    d->flags |= NamespaceDetails::Flag_HaveIdIndex;

    string system_indexes = database->name + ".system.indexes";

    BSONObjBuilder b;
    b.append("name", "_id_");
    b.append("ns", ns);
    b.append("key", id_obj);
    BSONObj o = b.done();

    /* edge case: note the insert could fail if we have hit maxindexes already */
    theDataFileMgr.insert(system_indexes.c_str(), o.objdata(), o.objsize());
}

DiskLoc DataFileMgr::insert(const char *ns, const void *buf, int len, bool god) {
    bool addIndex = false;
    const char *sys = strstr(ns, "system.");
    if ( sys ) {
        if ( sys == ns ) {
            cout << "ERROR: attempt to insert for invalid database 'system': " << ns << endl;
            return DiskLoc();
        }
        if ( strstr(ns, ".system.") ) {
            if ( strstr(ns, ".system.indexes") )
                addIndex = true;
            else if ( !god ) {
                cout << "ERROR: attempt to insert in system namespace " << ns << endl;
                return DiskLoc();
            }
        }
    }

    NamespaceDetails *d = nsdetails(ns);
    if ( d == 0 ) {
        addNewNamespaceToCatalog(ns);
        /* todo: shouldn't be in the namespace catalog until after the allocations here work.
                 also if this is an addIndex, those checks should happen before this!
        */
        database->newestFile()->newExtent(ns, initialExtentSize(len));
        d = nsdetails(ns);
    }
    d->paddingFits();

    NamespaceDetails *tableToIndex = 0;

    string tabletoidxns;
    if ( addIndex ) {
        BSONObj io((const char *) buf);
        const char *name = io.getStringField("name"); // name of the index
        tabletoidxns = io.getStringField("ns");  // table it indexes

        if ( database->name != nsToClient(tabletoidxns.c_str()) ) {
            uassert("bad table to index name on add index attempt", false);
            return DiskLoc();
        }

        BSONObj key = io.getObjectField("key");
        if ( *name == 0 || tabletoidxns.empty() || key.isEmpty() || key.objsize() > 2048 ) {
            cout << "user warning: bad add index attempt name:" << (name?name:"") << "\n  ns:" <<
                 tabletoidxns << "\n  ourns:" << ns;
            cout << "\n  idxobj:" << io.toString() << endl;
            return DiskLoc();
        }
        tableToIndex = nsdetails(tabletoidxns.c_str());
        if ( tableToIndex == 0 ) {
            // try to create it
            string err;
            if ( !userCreateNS(tabletoidxns.c_str(), emptyObj, err, false) ) {
                problem() << "ERROR: failed to create collection while adding its index. " << tabletoidxns << endl;
                return DiskLoc();
            }
            tableToIndex = nsdetails(tabletoidxns.c_str());
            log() << "info: creating collection " << tabletoidxns << " on add index\n";
            assert( tableToIndex );
        }
        if ( tableToIndex->nIndexes >= MaxIndexes ) {
            log() << "user warning: bad add index attempt, too many indexes for:" << tabletoidxns << endl;
            return DiskLoc();
        }
        if ( tableToIndex->findIndexByName(name) >= 0 ) {
            //cout << "INFO: index:" << name << " already exists for:" << tabletoidxns << endl;
            return DiskLoc();
        }
        //indexFullNS = tabletoidxns;
        //indexFullNS += ".$";
        //indexFullNS += name; // database.table.$index -- note this doesn't contain jsobjs, it contains BtreeBuckets.
    }

    DiskLoc extentLoc;
    int lenWHdr = len + Record::HeaderSize;
    lenWHdr = (int) (lenWHdr * d->paddingFactor);
    if ( lenWHdr == 0 ) {
        // old datafiles, backward compatible here.
        assert( d->paddingFactor == 0 );
        d->paddingFactor = 1.0;
        lenWHdr = len + Record::HeaderSize;
    }
    DiskLoc loc = d->alloc(ns, lenWHdr, extentLoc);
    if ( loc.isNull() ) {
        // out of space
        if ( d->capped == 0 ) { // size capped doesn't grow
            DEV log() << "allocating new extent for " << ns << " padding:" << d->paddingFactor << endl;
            database->newestFile()->newExtent(ns, followupExtentSize(len, d->lastExtentSize));
            loc = d->alloc(ns, lenWHdr, extentLoc);
        }
        if ( loc.isNull() ) {
            log() << "out of space in datafile " << ns << " capped:" << d->capped << endl;
            assert(d->capped);
            return DiskLoc();
        }
    }

    Record *r = loc.rec();
    assert( r->lengthWithHeaders >= lenWHdr );
    memcpy(r->data, buf, len);
    Extent *e = r->myExtent(loc);
    if ( e->lastRecord.isNull() ) {
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

    if ( tableToIndex ) {
        IndexDetails& idxinfo = tableToIndex->indexes[tableToIndex->nIndexes];
        idxinfo.info = loc;
        idxinfo.head = BtreeBucket::addHead(idxinfo);
        tableToIndex->addingIndex(tabletoidxns.c_str(), idxinfo);
        /* todo: index existing records here */
        addExistingToIndex(tabletoidxns.c_str(), idxinfo);
    }

    /* add this record to our indexes */
    if ( d->nIndexes )
        indexRecord(d, buf, len, loc);

//	cout << "   inserted at loc:" << hex << loc.getOfs() << " lenwhdr:" << hex << lenWHdr << dec << ' ' << ns << endl;
    return loc;
}

/* special version of insert for transaction logging -- streamlined a bit.
   assumes ns is capped and no indexes
*/
Record* DataFileMgr::fast_oplog_insert(NamespaceDetails *d, const char *ns, int len) {
    RARELY assert( d == nsdetails(ns) );

    DiskLoc extentLoc;
    int lenWHdr = len + Record::HeaderSize;
    DiskLoc loc = d->alloc(ns, lenWHdr, extentLoc);
    if ( loc.isNull() ) {
        assert(false);
        return 0;
    }

    Record *r = loc.rec();
    assert( r->lengthWithHeaders >= lenWHdr );

    Extent *e = r->myExtent(loc);
    if ( e->lastRecord.isNull() ) {
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

    return r;
}

void DataFileMgr::init(const char *dir) {
    /*	boost::filesystem::path path( dir );
    	path /= "temp.dat";
    	string pathString = path.string();
    	temp.open(pathString.c_str(), 64 * 1024 * 1024);
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
    assert( database->name == cl );

    closeClient( cl );
    _deleteDataFiles(cl);
}

typedef boost::filesystem::path Path;

// back up original database files to 'temp' dir
void _renameForBackup( const char *database, const Path &reservedPath ) {
    class Renamer : public FileOp {
    public:
        Renamer( const Path &reservedPath ) : reservedPath_( reservedPath ) {}
    private:
        const boost::filesystem::path &reservedPath_;
        virtual bool apply( const Path &p ) {
            if ( !boost::filesystem::exists( p ) )
                return false;
            boost::filesystem::rename( p, reservedPath_ / ( p.leaf() + ".bak" ) );
            return true;
        }
        virtual const char * op() const {
            return "renaming";
        }
    } renamer( reservedPath );
    _applyOpToDataFiles( database, renamer );
}

// move temp files to standard data dir
void _replaceWithRecovered( const char *database, const char *reservedPathString ) {
    class : public FileOp {
        virtual bool apply( const Path &p ) {
            if ( !boost::filesystem::exists( p ) )
                return false;
            boost::filesystem::rename( p, boost::filesystem::path(dbpath) / p.leaf() );
            return true;
        }
        virtual const char * op() const {
            return "renaming";
        }
    } renamer;
    _applyOpToDataFiles( database, renamer, reservedPathString );
}

// generate a directory name for storing temp data files
Path uniqueReservedPath( const char *prefix ) {
    Path dbPath = Path( dbpath );
    Path reservedPath;
    int i = 0;
    bool exists = false;
    do {
        stringstream ss;
        ss << prefix << "_repairDatabase_" << i++;
        reservedPath = dbPath / ss.str();
        BOOST_CHECK_EXCEPTION( exists = boost::filesystem::exists( reservedPath ) );
    } while ( exists );
    return reservedPath;
}

boost::intmax_t dbSize( const char *database ) {
    class SizeAccumulator : public FileOp {
    public:
        SizeAccumulator() : totalSize_( 0 ) {}
        boost::intmax_t size() const { return totalSize_; }
    private:
        virtual bool apply( const boost::filesystem::path &p ) {
            if( !boost::filesystem::exists( p ) )
                return false;
            totalSize_ += boost::filesystem::file_size( p );
            return true;
        }
        virtual const char *op() const { return "checking size"; }
        boost::intmax_t totalSize_;
    };
    SizeAccumulator sa;
    _applyOpToDataFiles( database, sa );
    return sa.size();
}

#if !defined(_WIN32)
#include <sys/statvfs.h>
boost::intmax_t freeSpace() {
    struct statvfs info;
    assert( !statvfs( dbpath, &info ) );
    return info.f_bavail * info.f_frsize;
}
#endif

bool repairDatabase( const char *ns, string &errmsg,
                    bool preserveClonedFilesOnFailure, bool backupOriginalFiles ) {
    stringstream ss;
    ss << "localhost:" << port;
    string localhost = ss.str();

    // ns is of the form "<dbname>.$cmd"
    char dbName[256];
    nsToClient(ns, dbName);
    problem() << "repairDatabase " << dbName << endl;
    assert( database->name == dbName );

#if !defined(_WIN32)
    boost::intmax_t totalSize = dbSize( dbName );
    boost::intmax_t freeSize = freeSpace();
    if ( freeSize < totalSize ) {
        stringstream ss;
        ss << "Cannot repair database " << dbName << " having size: " << totalSize
           << " (bytes) because free disk space is: " << freeSize << " (bytes)";
        errmsg = ss.str();
        problem() << errmsg << endl;
        return false;
    }
#endif
    
    Path reservedPath =
        uniqueReservedPath( ( preserveClonedFilesOnFailure || backupOriginalFiles ) ?
                           "backup" : "tmp" );
    BOOST_CHECK_EXCEPTION( boost::filesystem::create_directory( reservedPath ) );
    string reservedPathString = reservedPath.native_directory_string();
    assert( setClient( dbName, reservedPathString.c_str() ) );

    bool res = cloneFrom(localhost.c_str(), errmsg, dbName, /*logForReplication=*/false, /*slaveok*/false);
    closeClient( dbName, reservedPathString.c_str() );

    if ( !res ) {
        problem() << "clone failed for " << dbName << " with error: " << errmsg << endl;
        if ( !preserveClonedFilesOnFailure )
            BOOST_CHECK_EXCEPTION( boost::filesystem::remove_all( reservedPath ) );
        return false;
    }

    assert( !setClientTempNs( dbName ) );
    closeClient( dbName );

    if ( backupOriginalFiles )
        _renameForBackup( dbName, reservedPath );
    else
        _deleteDataFiles( dbName );

    _replaceWithRecovered( dbName, reservedPathString.c_str() );

    if ( !backupOriginalFiles )
        BOOST_CHECK_EXCEPTION( boost::filesystem::remove_all( reservedPath ) );

    return true;
}
