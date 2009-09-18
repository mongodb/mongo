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
#include "../util/file_allocator.h"
#include "btree.h"
#include <algorithm>
#include <list>
#include "query.h"
#include "repl.h"
#include "dbhelpers.h"
#include "namespace.h"
#include "queryutil.h"

namespace mongo {

    extern bool quota;

    string dbpath = "/data/db/";

    DataFileMgr theDataFileMgr;
    map<string,Database*> databases;
    Database *database;
    const char *curNs = "";
    int MAGIC = 0x1000;
    int curOp = -2;
    int callDepth = 0;
    bool prealloc = true;

    extern int otherTraceLevel;
    void addNewNamespaceToCatalog(const char *ns, const BSONObj *options = 0);
    void ensureIdIndexForNewNs(const char *ns) {
        if ( !strstr( ns, ".system." ) && !strstr( ns, ".$freelist" ) ) {
            log( 1 ) << "adding _id index for new collection" << endl;
            ensureHaveIdIndex( ns );
        }        
    }

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

    BSONObj::BSONObj(const Record *r) {
        init(r->data, false);
        /*
        	_objdata = r->data;
        	_objsize = *((int*) _objdata);
        	if( _objsize > r->netLength() ) {
        		out() << "About to assert fail _objsize <= r->netLength()" << endl;
        		out() << " _objsize: " << _objsize << endl;
        		out() << " netLength(): " << r->netLength() << endl;
        		out() << " extentOfs: " << r->extentOfs << endl;
        		out() << " nextOfs: " << r->nextOfs << endl;
        		out() << " prevOfs: " << r->prevOfs << endl;
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

    bool _userCreateNS(const char *ns, const BSONObj& j, string& err) {
        if ( nsdetails(ns) ) {
            err = "collection already exists";
            return false;
        }

        log(1) << "create collection " << ns << ' ' << j << '\n';

        /* todo: do this only when we have allocated space successfully? or we could insert with a { ok: 0 } field
                 and then go back and set to ok : 1 after we are done.
        */
        if( strstr(ns, ".$freelist") == 0 )
            addNewNamespaceToCatalog(ns, j.isEmpty() ? 0 : &j);

        long long size = initialExtentSize(128);
        BSONElement e = j.findElement("size");
        if ( e.isNumber() ) {
            size = (long long) e.number();
            size += 256;
            size &= 0xffffffffffffff00LL;
        }
        
        uassert( "invalid size spec", size > 0 );

        bool newCapped = false;
        int mx = 0;
        e = j.findElement("capped");
        if ( e.type() == Bool && e.boolean() ) {
            newCapped = true;
            e = j.findElement("max");
            if ( e.isNumber() ) {
                mx = (int) e.number();
            }
        }

        // $nExtents just for debug/testing.  We create '$nExtents' extents,
        // each of size 'size'.
        e = j.findElement( "$nExtents" );
        int nExtents = int( e.number() );
        if ( nExtents > 0 ) {
            assert( size <= 0x7fffffff );
            for ( int i = 0; i < nExtents; ++i ) {
                database->suitableFile((int) size)->allocExtent( ns, (int) size, newCapped );
            }
        } else {
            while ( size > 0 ) {
                int max = MongoDataFile::maxSize() - MDFHeader::headerSize();
                int desiredExtentSize = (int) (size > max ? max : size);
                Extent *e = database->suitableFile( desiredExtentSize )->allocExtent( ns, desiredExtentSize, newCapped );
                size -= e->length;
            }
            if ( !newCapped ) {
                // ok to call this multiple times
                database->preallocateAFile();
            }
        }

        NamespaceDetails *d = nsdetails(ns);
        assert(d);

        if ( j.getField( "autoIndexId" ).isBoolean() ) {
            if ( j.getBoolField( "autoIndexId" ) ) {
                ensureIdIndexForNewNs( ns );
            }
        } else {
            if ( !newCapped ) {
                ensureIdIndexForNewNs( ns );
            }
        }

        if ( mx > 0 )
            d->max = mx;

        return true;
    }

// { ..., capped: true, size: ..., max: ... }
// returns true if successful
    bool userCreateNS(const char *ns, BSONObj j, string& err, bool logForReplication) {
        const char *coll = strchr( ns, '.' ) + 1;
        massert( "invalid ns", coll && *coll );
        char cl[ 256 ];
        nsToClient( ns, cl );
        bool ok = _userCreateNS(ns, j, err);
        if ( logForReplication && ok ) {
            if ( j.getField( "create" ).eoo() ) {
                BSONObjBuilder b;
                b << "create" << coll;
                b.appendElements( j );
                j = b.obj();
            }
            string logNs = string( cl ) + ".$cmd";
            logOp("c", logNs.c_str(), j);
        }
        return ok;
    }

    /*---------------------------------------------------------------------*/

    int MongoDataFile::maxSize() {
        if ( sizeof( int* ) == 4 )
            return 512 * 1024 * 1024;
        else
            return 0x7ff00000;
    }

    int MongoDataFile::defaultSize( const char *filename ) const {
        int size;

        if ( fileNo <= 4 )
            size = (64*1024*1024) << fileNo;
        else
            size = 0x7ff00000;

        if ( strstr(filename, "_hudsonSmall") ) {
            int mult = 1;
            if ( fileNo > 1 && fileNo < 1000 )
                mult = fileNo;
            size = 1024 * 512 * mult;
            log() << "Warning : using small files for _hudsonSmall" << endl;
        }

        return size;
    }

    void MongoDataFile::open( const char *filename, int minSize, bool preallocateOnly ) {
        {
            /* check quotas
               very simple temporary implementation - we will in future look up
               the quota from the grid database
            */
            if ( quota && fileNo > 8 && !boost::filesystem::exists(filename) ) {
                /* todo: if we were adding / changing keys in an index did we do some
                   work previously that needs cleaning up?  Possible.  We should
                   check code like that and have it catch the exception and do
                   something reasonable.
                */
                string s = "db disk space quota exceeded ";
                if ( database )
                    s += database->name;
                uasserted(s);
            }
        }

        long size = defaultSize( filename );
        while ( size < minSize ) {
            if ( size < maxSize() / 2 )
                size *= 2;
            else {
                size = maxSize();
                break;
            }
        }
        if ( size > maxSize() )
            size = maxSize();

        assert( ( size >= 64*1024*1024 ) || ( strstr( filename, "_hudsonSmall" ) ) );
        assert( size % 4096 == 0 );

        if ( preallocateOnly ) {
	  if ( prealloc ) {
            theFileAllocator().requestAllocation( filename, size );
	  }
            return;
        }
        
        header = (MDFHeader *) mmf.map(filename, size);
        if( sizeof(char *) == 4 ) 
            uassert("can't map file memory - mongo requires 64 bit build for larger datasets", header);
        else
            uassert("can't map file memory", header);
        header->init(fileNo, size);
    }

    void addNewExtentToNamespace(const char *ns, Extent *e, DiskLoc eloc, DiskLoc emptyLoc, bool capped) { 
        DiskLoc oldExtentLoc;
        NamespaceIndex *ni = nsindex(ns);
        NamespaceDetails *details = ni->details(ns);
        if ( details ) {
assert( !details->lastExtent.isNull() );
            assert( !details->firstExtent.isNull() );
            e->xprev = details->lastExtent;
            details->lastExtent.ext()->xnext = eloc;
assert( !eloc.isNull() );
            details->lastExtent = eloc;
        }
        else {
            ni->add(ns, eloc, capped);
            details = ni->details(ns);
        }

        details->lastExtentSize = e->length;
        DEBUGGING out() << "temp: newextent adddelrec " << ns << endl;
        details->addDeletedRec(emptyLoc.drec(), emptyLoc);
    }

    Extent* MongoDataFile::createExtent(const char *ns, int approxSize, bool newCapped, int loops) {
        massert( "bad new extent size", approxSize >= 0 && approxSize <= 0x7ff00000 );
        massert( "header==0 on new extent: 32 bit mmap space exceeded?", header ); // null if file open failed
        int ExtentSize = approxSize <= header->unusedLength ? approxSize : header->unusedLength;
        DiskLoc loc;
        if ( ExtentSize <= 0 ) {
            /* not there could be a lot of looping here is db just started and
               no files are open yet.  we might want to do something about that. */
            if ( loops > 8 ) {
                assert( loops < 10000 );
                out() << "warning: loops=" << loops << " fileno:" << fileNo << ' ' << ns << '\n';
            }
            log() << "newExtent: " << ns << " file " << fileNo << " full, adding a new file\n";
            return database->addAFile( 0, true )->createExtent(ns, approxSize, newCapped, loops+1);
        }
        int offset = header->unused.getOfs();
        header->unused.setOfs( fileNo, offset + ExtentSize );
        header->unusedLength -= ExtentSize;
        loc.setOfs(fileNo, offset);
        Extent *e = _getExtent(loc);
        DiskLoc emptyLoc = e->init(ns, ExtentSize, fileNo, offset);

        addNewExtentToNamespace(ns, e, loc, emptyLoc, newCapped);

        DEV log() << "new extent " << ns << " size: 0x" << hex << ExtentSize << " loc: 0x" << hex << offset
        << " emptyLoc:" << hex << emptyLoc.getOfs() << dec << endl;
        return e;
    }

    Extent* MongoDataFile::allocExtent(const char *ns, int approxSize, bool capped) { 
        string s = database->name + ".$freelist";
        NamespaceDetails *f = nsdetails(s.c_str());
        if( f ) {
            int low, high;
            if( capped ) {
                // be strict about the size
                low = approxSize;
                if( low > 2048 ) low -= 256;
                high = (int) (approxSize * 1.05) + 256;
            }
            else { 
                low = (int) (approxSize * 0.8);
                high = (int) (approxSize * 1.4);
            }
            if( high < 0 ) high = approxSize;
            int n = 0;
            Extent *best = 0;
            int bestDiff = 0x7fffffff;
            {
                DiskLoc L = f->firstExtent;
                while( !L.isNull() ) { 
                    Extent * e = L.ext();
                    if( e->length >= low && e->length <= high ) { 
                        int diff = abs(e->length - approxSize);
                        if( diff < bestDiff ) { 
                            bestDiff = diff;
                            best = e;
                            if( diff == 0 ) 
                                break;
                        }
                    }
                    L = e->xnext;
                    ++n;
                
                }
            }
            OCCASIONALLY if( n > 512 ) log() << "warning: newExtent " << n << " scanned\n";
            if( best ) {
                Extent *e = best;
                // remove from the free list
                if( !e->xprev.isNull() )
                    e->xprev.ext()->xnext = e->xnext;
                if( !e->xnext.isNull() )
                    e->xnext.ext()->xprev = e->xprev;
                if( f->firstExtent == e->myLoc )
                    f->firstExtent = e->xnext;
                if( f->lastExtent == e->myLoc )
                    f->lastExtent = e->xprev;

                // use it
                OCCASIONALLY if( n > 512 ) log() << "warning: newExtent " << n << " scanned\n";
                DiskLoc emptyLoc = e->reuse(ns);
                addNewExtentToNamespace(ns, e, e->myLoc, emptyLoc, capped);
                return e;
            }
        }

        return createExtent(ns, approxSize, capped);
    }

    /*---------------------------------------------------------------------*/

    DiskLoc Extent::reuse(const char *nsname) { 
        log(3) << "reset extent was:" << ns.buf << " now:" << nsname << '\n';
        massert( "Extent::reset bad magic value", magic == 0x41424344 );
        xnext.Null();
        xprev.Null();
        ns = nsname;
        firstRecord.Null();
        lastRecord.Null();

        DiskLoc emptyLoc = myLoc;
        emptyLoc.inc( (extentData-(char*)this) );

        int delRecLength = length - (extentData - (char *) this);
        DeletedRecord *empty1 = (DeletedRecord *) extentData;
        DeletedRecord *empty = (DeletedRecord *) getRecord(emptyLoc);
        assert( empty == empty1 );
        memset(empty, delRecLength, 1);

        empty->lengthWithHeaders = delRecLength;
        empty->extentOfs = myLoc.getOfs();
        empty->nextDeleted.Null();

        return emptyLoc;
    }

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

    auto_ptr<Cursor> DataFileMgr::findAll(const char *ns, const DiskLoc &startLoc) {
        DiskLoc loc;
        bool found = nsindex(ns)->find(ns, loc);
        if ( !found ) {
            //		out() << "info: findAll() namespace does not exist: " << ns << endl;
            return auto_ptr<Cursor>(new BasicCursor(DiskLoc()));
        }

        Extent *e = getExtent(loc);

        DEBUGGING {
            out() << "listing extents for " << ns << endl;
            DiskLoc tmp = loc;
            set<DiskLoc> extents;

            while ( 1 ) {
                Extent *f = getExtent(tmp);
                out() << "extent: " << tmp.toString() << endl;
                extents.insert(tmp);
                tmp = f->xnext;
                if ( tmp.isNull() )
                    break;
                f = f->getNextExtent();
            }

            out() << endl;
            nsdetails(ns)->dumpDeleted(&extents);
        }

        if ( !nsdetails( ns )->capped ) {
            if ( !startLoc.isNull() )
                return auto_ptr<Cursor>(new BasicCursor( startLoc ));                
            while ( e->firstRecord.isNull() && !e->xnext.isNull() ) {
                /* todo: if extent is empty, free it for reuse elsewhere.
                    that is a bit complicated have to clean up the freelists.
                */
                RARELY out() << "info DFM::findAll(): extent " << loc.toString() << " was empty, skipping ahead " << ns << endl;
                // find a nonempty extent
                // it might be nice to free the whole extent here!  but have to clean up free recs then.
                e = e->getNextExtent();
            }
            return auto_ptr<Cursor>(new BasicCursor( e->firstRecord ));
        } else {
            return auto_ptr< Cursor >( new ForwardCappedCursor( nsdetails( ns ), startLoc ) );
        }
    }

    /* get a table scan cursor, but can be forward or reverse direction.
       order.$natural - if set, > 0 means forward (asc), < 0 backward (desc).
    */
    auto_ptr<Cursor> findTableScan(const char *ns, const BSONObj& order, const DiskLoc &startLoc) {
        BSONElement el = order.findElement("$natural"); // e.g., { $natural : -1 }

        if ( el.number() >= 0 )
            return DataFileMgr::findAll(ns, startLoc);

        // "reverse natural order"
        NamespaceDetails *d = nsdetails(ns);
        if ( !d )
            return auto_ptr<Cursor>(new BasicCursor(DiskLoc()));
        if ( !d->capped ) {
            if ( !startLoc.isNull() )
                return auto_ptr<Cursor>(new ReverseCursor( startLoc ));                
            Extent *e = d->lastExtent.ext();
            while ( e->lastRecord.isNull() && !e->xprev.isNull() ) {
                OCCASIONALLY out() << "  findTableScan: extent empty, skipping ahead" << endl;
                e = e->getPrevExtent();
            }
            return auto_ptr<Cursor>(new ReverseCursor( e->lastRecord ));
        } else {
            return auto_ptr< Cursor >( new ReverseCappedCursor( d, startLoc ) );
        }
    }

    void aboutToDelete(const DiskLoc& dl);

    /* drop a collection/namespace */
    void dropNS(const string& nsToDrop) {
        NamespaceDetails* d = nsdetails(nsToDrop.c_str());
        uassert( (string)"ns not found: " + nsToDrop , d );

        uassert( "can't drop system ns", strstr(nsToDrop.c_str(), ".system.") == 0 );
        {
            // remove from the system catalog
            BSONObj cond = BSON( "name" << nsToDrop );   // { name: "colltodropname" }
            string system_namespaces = database->name + ".system.namespaces";
            /*int n = */ deleteObjects(system_namespaces.c_str(), cond, false, false, true);
			// no check of return code as this ns won't exist for some of the new storage engines
        }

        // free extents
        if( !d->firstExtent.isNull() ) {
            string s = database->name + ".$freelist";
            NamespaceDetails *freeExtents = nsdetails(s.c_str());
            if( freeExtents == 0 ) { 
                string err;
                _userCreateNS(s.c_str(), BSONObj(), err);
                freeExtents = nsdetails(s.c_str());
                massert("can't create .$freelist", freeExtents);
            }
            if( freeExtents->firstExtent.isNull() ) { 
                freeExtents->firstExtent = d->firstExtent;
                freeExtents->lastExtent = d->lastExtent;
            }
            else { 
                DiskLoc a = freeExtents->firstExtent;
                assert( a.ext()->xprev.isNull() );
                a.ext()->xprev = d->lastExtent;
                d->lastExtent.ext()->xnext = a;
                freeExtents->firstExtent = d->firstExtent;

                d->firstExtent.setInvalid();
                d->lastExtent.setInvalid();
            }
        }

        // remove from the catalog hashtable
        database->namespaceIndex.kill(nsToDrop.c_str());
    }

    void dropCollection( const string &name, string &errmsg, BSONObjBuilder &result ) {
        log(1) << "dropCollection: " << name << endl;
        NamespaceDetails *d = nsdetails(name.c_str());
        assert( d );
        if ( d->nIndexes != 0 ) {
            try { 
                assert( deleteIndexes(d, name.c_str(), "*", errmsg, result, true) );
            }
            catch( DBException& ) {
                uasserted("drop: deleteIndexes for collection failed - consider trying repair");
            }
            assert( d->nIndexes == 0 );
        }
        log(1) << "\t deleteIndexes dones" << endl;
        result.append("ns", name.c_str());
        ClientCursor::invalidate(name.c_str());
        dropNS(name);        
    }
    
    /* delete this index.  does NOT clean up the system catalog
       (system.indexes or system.namespaces) -- only NamespaceIndex.
    */
    void IndexDetails::kill() {
        string ns = indexNamespace(); // e.g. foo.coll.$ts_1
        
        // clean up parent namespace index cache
        NamespaceDetailsTransient::get( parentNS().c_str() ).deletedIndex();

        BSONObjBuilder b;
        b.append("name", indexName().c_str());
        b.append("ns", parentNS().c_str());
        BSONObj cond = b.done(); // e.g.: { name: "ts_1", ns: "foo.coll" }

        btreeStore->drop(ns.c_str());
        head.setInvalid();
        info.setInvalid();

        // clean up in system.indexes.  we do this last on purpose.  note we have 
        // to make the cond object before the drop() above though.
        string system_indexes = database->name + ".system.indexes";
        int n = deleteObjects(system_indexes.c_str(), cond, false, false, true);
        wassert( n == 1 );
    }

  void getKeys( vector< const char * > fieldNames, vector< BSONElement > fixed, const BSONObj &obj, BSONObjSetDefaultOrder &keys ) {
    BSONObjBuilder b;
    b.appendNull( "" );
    BSONElement nullElt = b.done().firstElement();
    BSONElement arrElt;
    unsigned arrIdx = ~0;
    for( unsigned i = 0; i < fieldNames.size(); ++i ) {
      if ( *fieldNames[ i ] == '\0' )
	continue;
      BSONElement e = obj.getFieldDottedOrArray( fieldNames[ i ] );
      if ( e.eoo() )
	e = nullElt; // no matching field
      if ( e.type() != Array )
	fieldNames[ i ] = ""; // no matching field or non-array match
      if ( *fieldNames[ i ] == '\0' )
	fixed[ i ] = e; // no need for further object expansion (though array expansion still possible)
      if ( e.type() == Array && arrElt.eoo() ) { // we only expand arrays on a single path -- track the path here
	arrIdx = i;
	arrElt = e;
      }
      // enforce single array path here
      uassert( "cannot index parallel arrays", e.type() != Array || e.rawdata() == arrElt.rawdata() );
    }
    bool allFound = true; // have we found elements for all field names in the key spec?
    for( vector< const char * >::const_iterator i = fieldNames.begin(); allFound && i != fieldNames.end(); ++i )
      if ( **i != '\0' )
	allFound = false;
    if ( allFound ) {
      if ( arrElt.eoo() ) {
	// no terminal array element to expand
	BSONObjBuilder b;
	for( vector< BSONElement >::iterator i = fixed.begin(); i != fixed.end(); ++i )
	  b.appendAs( *i, "" );
	keys.insert( b.obj() );
      } else {
	// terminal array element to expand, so generate all keys
	BSONObjIterator i( arrElt.embeddedObject() );
	while( i.more() ) {
	  BSONObjBuilder b;
	  for( unsigned j = 0; j < fixed.size(); ++j ) {
	    if ( j == arrIdx )
	      b.appendAs( i.next(), "" );
	    else
	      b.appendAs( fixed[ j ], "" );
	  }
	  keys.insert( b.obj() );
	}
      }
    } else {
      // nonterminal array element to expand, so recurse
      assert( !arrElt.eoo() );
      BSONObjIterator i( arrElt.embeddedObject() );
      while( i.more() ) {
	BSONElement e = i.next();
	if ( e.type() == Object )
	  getKeys( fieldNames, fixed, e.embeddedObject(), keys );
      }
    }
  }

  void getKeysFromObject( const BSONObj &keyPattern, const BSONObj &obj, BSONObjSetDefaultOrder &keys ) {
    BSONObjIterator i( keyPattern );
    vector< const char * > fieldNames;
    vector< BSONElement > fixed;
    BSONObjBuilder nullKey;
    while( i.more() ) {
      fieldNames.push_back( i.next().fieldName() );
      fixed.push_back( BSONElement() );
      nullKey.appendNull( "" );
    }
    getKeys( fieldNames, fixed, obj, keys );
    if ( keys.empty() )
      keys.insert( nullKey.obj() );
  }

    /* Pull out the relevant key objects from obj, so we
       can index them.  Note that the set is multiple elements
       only when it's a "multikey" array.
       Keys will be left empty if key not found in the object.
    */
    void IndexDetails::getKeysFromObject( const BSONObj& obj, BSONObjSetDefaultOrder& keys) const {
        BSONObj keyPattern = info.obj().getObjectField("key"); // e.g., keyPattern == { ts : 1 }
	if ( keyPattern.objsize() == 0 ) {
	  out() << keyPattern.toString() << endl;
	  out() << info.obj().toString() << endl;
	  assert(false);
	}
	mongo::getKeysFromObject( keyPattern, obj, keys );
    }

    int nUnindexes = 0;

    void _unindexRecord(IndexDetails& id, BSONObj& obj, const DiskLoc& dl, bool logMissing = true) {
        BSONObjSetDefaultOrder keys;
        id.getKeysFromObject(obj, keys);
        for ( BSONObjSetDefaultOrder::iterator i=keys.begin(); i != keys.end(); i++ ) {
            BSONObj j = *i;
//		out() << "UNINDEX: j:" << j.toString() << " head:" << id.head.toString() << dl.toString() << endl;
            if ( otherTraceLevel >= 5 ) {
                out() << "_unindexRecord() " << obj.toString();
                out() << "\n  unindex:" << j.toString() << endl;
            }
            nUnindexes++;
            bool ok = false;
            try {
                ok = id.head.btree()->unindex(id.head, id, j, dl);
            }
            catch (AssertionException&) {
                problem() << "Assertion failure: _unindex failed " << id.indexNamespace() << endl;
                out() << "Assertion failure: _unindex failed" << '\n';
                out() << "  obj:" << obj.toString() << '\n';
                out() << "  key:" << j.toString() << '\n';
                out() << "  dl:" << dl.toString() << endl;
                sayDbContext();
            }

            if ( !ok && logMissing ) {
                out() << "unindex failed (key too big?) " << id.indexNamespace() << '\n';
            }
        }
    }

    /* unindex all keys in all indexes for this record. */
    void  unindexRecord(NamespaceDetails *d, Record *todelete, const DiskLoc& dl, bool noWarn = false) {
        if ( d->nIndexes == 0 ) return;
        BSONObj obj(todelete);
        for ( int i = 0; i < d->nIndexes; i++ ) {
            _unindexRecord(d->indexes[i], obj, dl, !noWarn);
        }
    }

    /* deletes a record, just the pdfile portion -- no index cleanup, no cursor cleanup, etc. 
       caller must check if capped
    */
    void DataFileMgr::_deleteRecord(NamespaceDetails *d, const char *ns, Record *todelete, const DiskLoc& dl)
    {
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

    void DataFileMgr::deleteRecord(const char *ns, Record *todelete, const DiskLoc& dl, bool cappedOK, bool noWarn)
    {
        dassert( todelete == dl.rec() );

        NamespaceDetails* d = nsdetails(ns);
        if ( d->capped && !cappedOK ) {
            out() << "failing remove on a capped ns " << ns << endl;
            return;
        }

        /* check if any cursors point to us.  if so, advance them. */
        aboutToDelete(dl);

        unindexRecord(d, todelete, dl, noWarn);

        _deleteRecord(d, ns, todelete, dl);
        NamespaceDetailsTransient::get( ns ).registerWriteOp();
    }

    void setDifference(BSONObjSetDefaultOrder &l, BSONObjSetDefaultOrder &r, vector<BSONObj*> &diff) {
        BSONObjSetDefaultOrder::iterator i = l.begin();
        BSONObjSetDefaultOrder::iterator j = r.begin();
        while ( 1 ) {
            if ( i == l.end() )
                break;
            while ( j != r.end() && j->woCompare( *i ) < 0 )
                j++;
            if ( j == r.end() || i->woCompare(*j) != 0  ) {
                const BSONObj *jo = &*i;
                diff.push_back( (BSONObj *) jo );
            }
            i++;
        }
    }

    struct IndexChanges/*on an update*/ {
        BSONObjSetDefaultOrder oldkeys;
        BSONObjSetDefaultOrder newkeys;
        vector<BSONObj*> removed; // these keys were removed as part of the change
        vector<BSONObj*> added;   // these keys were added as part of the change

        void dupCheck(IndexDetails& idx) {
            if( added.empty() || !idx.unique() )
                return;
            for( vector<BSONObj*>::iterator i = added.begin(); i != added.end(); i++ )
                uassert("E11001 duplicate key on update", !idx.hasKey(**i));
        }
    };

    inline void getIndexChanges(vector<IndexChanges>& v, NamespaceDetails& d, BSONObj newObj, BSONObj oldObj) { 
        v.resize(d.nIndexes);
        for ( int i = 0; i < d.nIndexes; i++ ) {
            IndexDetails& idx = d.indexes[i];
            BSONObj idxKey = idx.info.obj().getObjectField("key"); // eg { ts : 1 }
            IndexChanges& ch = v[i];
            idx.getKeysFromObject(oldObj, ch.oldkeys);
            idx.getKeysFromObject(newObj, ch.newkeys);
            if( ch.newkeys.size() > 1 ) 
                d.setIndexIsMultikey(i);
            setDifference(ch.oldkeys, ch.newkeys, ch.removed);
            setDifference(ch.newkeys, ch.oldkeys, ch.added);
        }
    }

    inline void dupCheck(vector<IndexChanges>& v, NamespaceDetails& d) {
        for ( int i = 0; i < d.nIndexes; i++ ) {
            IndexDetails& idx = d.indexes[i];
            v[i].dupCheck(idx);
        }
    }

    /** Note: if the object shrinks a lot, we don't free up space, we leave extra at end of the record.
    */
    void DataFileMgr::update(
        const char *ns,
        Record *toupdate, const DiskLoc& dl,
        const char *_buf, int _len, stringstream& ss)
    {
        dassert( toupdate == dl.rec() );

        NamespaceDetails *d = nsdetails(ns);

        BSONObj objOld(toupdate);
        BSONObj objNew(_buf);
        assert( objNew.objsize() == _len );
        assert( objNew.objdata() == _buf );

        if( !objNew.hasElement("_id") && objOld.hasElement("_id") ) {
            /* add back the old _id value if the update removes it.  Note this implementation is slow 
               (copies entire object multiple times), but this shouldn't happen often, so going for simple
               code, not speed.
               */
            BSONObjBuilder b;
            BSONElement e;
            assert( objOld.getObjectID(e) );
            b.append(e); // put _id first, for best performance
            b.appendElements(objNew);
            objNew = b.obj();
        }

        /* duplicate key check. we descend the btree twice - once for this check, and once for the actual inserts, further  
           below.  that is suboptimal, but it's pretty complicated to do it the other way without rollbacks...
        */
        vector<IndexChanges> changes;
        getIndexChanges(changes, *d, objNew, objOld);
        dupCheck(changes, *d);

        if ( toupdate->netLength() < objNew.objsize() ) {
            // doesn't fit.  reallocate -----------------------------------------------------
            uassert("E10003 failing update: objects in a capped ns cannot grow", !(d && d->capped));
            d->paddingTooSmall();
            if ( database->profile )
                ss << " moved ";
            deleteRecord(ns, toupdate, dl);
            insert(ns, objNew.objdata(), objNew.objsize(), false);
            return;
        }

        NamespaceDetailsTransient::get( ns ).registerWriteOp();
        d->paddingFits();

        /* have any index keys changed? */
        if( d->nIndexes ) {
            for ( int x = 0; x < d->nIndexes; x++ ) {
                IndexDetails& idx = d->indexes[x];
                for ( unsigned i = 0; i < changes[x].removed.size(); i++ ) {
                    try {
                        idx.head.btree()->unindex(idx.head, idx, *changes[x].removed[i], dl);
                    }
                    catch (AssertionException&) {
                        ss << " exception update unindex ";
                        problem() << " caught assertion update unindex " << idx.indexNamespace() << endl;
                    }
                }
                assert( !dl.isNull() );
                BSONObj idxKey = idx.info.obj().getObjectField("key");
                for ( unsigned i = 0; i < changes[x].added.size(); i++ ) {
                    try {
                        /* we did the dupCheck() above.  so we don't have to worry about it here. */
                        idx.head.btree()->bt_insert(
                            idx.head,
                            dl, *changes[x].added[i], idxKey, /*dupsAllowed*/true, idx);
                    }
                    catch (AssertionException&) {
                        ss << " exception update index ";
                        out() << " caught assertion update index " << idx.indexNamespace() << '\n';
                        problem() << " caught assertion update index " << idx.indexNamespace() << endl;
                    }
                    if ( database->profile )
                        ss << '\n' << changes[x].added.size() << " key updates ";
                }

            }
        }

        //	update in place
        memcpy(toupdate->data, objNew.objdata(), objNew.objsize());
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
    inline void  _indexRecord(NamespaceDetails *d, int idxNo, BSONObj& obj, DiskLoc newRecordLoc, bool dupsAllowed) {
        IndexDetails& idx = d->indexes[idxNo];
        BSONObjSetDefaultOrder keys;
        idx.getKeysFromObject(obj, keys);
        BSONObj order = idx.keyPattern();
        int n = 0;
        for ( BSONObjSetDefaultOrder::iterator i=keys.begin(); i != keys.end(); i++ ) {
            if( ++n == 2 ) { 
                d->setIndexIsMultikey(idxNo);
            }
            assert( !newRecordLoc.isNull() );
            try {
                idx.head.btree()->bt_insert(idx.head, newRecordLoc,
                                         *i, order, dupsAllowed, idx);
            }
            catch (AssertionException& ) {
                if( !dupsAllowed ) {
                    // dup key exception, presumably.
                    throw;
                }
                problem() << " caught assertion _indexRecord " << idx.indexNamespace() << endl;
            }
        }
    }

    /* note there are faster ways to build an index in bulk, that can be
       done eventually */
    void addExistingToIndex(const char *ns, NamespaceDetails *d, IndexDetails& idx, int idxNo) {
        bool dupsAllowed = !idx.unique();
        bool dropDups = idx.dropDups();

        Timer t;
        Nullstream& l = log();
        l << "building new index on " << idx.keyPattern() << " for " << ns << "...";
        l.flush();
        int n = 0;
        auto_ptr<Cursor> c = theDataFileMgr.findAll(ns);
        while ( c->ok() ) {
            BSONObj js = c->current();
            try { 
                _indexRecord(d, idxNo, js, c->currLoc(),dupsAllowed);
                c->advance();
            } catch( AssertionException& e ) { 
                if ( dropDups ) {
                    DiskLoc toDelete = c->currLoc();
                    c->advance();
                    theDataFileMgr.deleteRecord( ns, toDelete.rec(), toDelete, false, true );
                } else {
                    l << endl;
                    log(2) << "addExistingToIndex exception " << e.what() << endl;
                    throw;
                }
            }
            n++;
        };
        l << "done for " << n << " records";
        l << endl;
    }

    void buildIndex(string ns, NamespaceDetails *d, IndexDetails& idx, int idxNo) { 
        idx.head = BtreeBucket::addHead(idx);
        addExistingToIndex(ns.c_str(), d, idx, idxNo);
    }

    /* add keys to indexes for a new record */
    void  indexRecord(NamespaceDetails *d, const void *buf, int len, DiskLoc newRecordLoc) {
        BSONObj obj((const char *)buf);

        /*UNIQUE*/
        for ( int i = 0; i < d->nIndexes; i++ ) {
            try { 
                bool unique = d->indexes[i].unique();
                _indexRecord(d, i, obj, newRecordLoc, /*dupsAllowed*/!unique);
            }
            catch( DBException& ) { 
                // try to roll back previously added index entries
                for( int j = 0; j <= i; j++ ) { 
                    try {
                        _unindexRecord(d->indexes[j], obj, newRecordLoc, false);
                    }
                    catch(...) { 
                        log(3) << "unindex fails on rollback after unique failure\n";
                    }
                }
                throw;
            }
        }
    }

    extern BSONObj id_obj;

    void ensureHaveIdIndex(const char *ns) {
        NamespaceDetails *d = nsdetails(ns);
        if ( d == 0 || (d->flags & NamespaceDetails::Flag_HaveIdIndex) )
            return;

        d->flags |= NamespaceDetails::Flag_HaveIdIndex;

        for( int i = 0; i < d->nIndexes; ++i )
            if ( d->indexes[ i ].isIdIndex() )
                return;
        
        string system_indexes = database->name + ".system.indexes";

        BSONObjBuilder b;
        b.append("name", "_id_");
        b.append("ns", ns);
        b.append("key", id_obj);
        BSONObj o = b.done();

        /* edge case: note the insert could fail if we have hit maxindexes already */
        theDataFileMgr.insert(system_indexes.c_str(), o.objdata(), o.objsize(), true);
    }

    // should be { <something> : <simpletype[1|-1]>, .keyp.. } 
    bool validKeyPattern(BSONObj kp) { 
        BSONObjIterator i(kp);
        while( i.moreWithEOO() ) { 
            BSONElement e = i.next();
            if( e.type() == Object || e.type() == Array ) 
                return false;
        }
        return true;
    }

#pragma pack(1)
    struct IDToInsert_ { 
        char type;
        char _id[4];
        OID oid;
        IDToInsert_() {
            type = (char) jstOID;
            strcpy(_id, "_id");
            assert( sizeof(IDToInsert_) == 17 );
        }
    } idToInsert_;
    struct IDToInsert : public BSONElement {
        IDToInsert() : BSONElement( ( char * )( &idToInsert_ ) ) {}
    } idToInsert;
#pragma pack()
    
    void DataFileMgr::insertAndLog( const char *ns, const BSONObj &o, bool god ) {
        BSONObj tmp = o;
        insert( ns, tmp, god );
        logOp( "i", ns, tmp );
    }
    
    DiskLoc DataFileMgr::insert(const char *ns, BSONObj &o, bool god) {
        DiskLoc loc = insert( ns, o.objdata(), o.objsize(), god );
        if ( !loc.isNull() )
            o = BSONObj( loc.rec() );
        return loc;
    }

    DiskLoc DataFileMgr::insert(const char *ns, const void *obuf, int len, bool god, const BSONElement &writeId, bool mayAddIndex) {
        bool wouldAddIndex = false;
        uassert("cannot insert into reserved $ collection", god || strchr(ns, '$') == 0 );
        const char *sys = strstr(ns, "system.");
        if ( sys ) {
            uassert("attempt to insert in reserved database name 'system'", sys != ns);
            if ( strstr(ns, ".system.") ) {
                // later:check for dba-type permissions here if have that at some point separate
                if ( strstr(ns, ".system.indexes" ) )
                    wouldAddIndex = true;
                else if ( strstr(ns, ".system.users") )
                    ;
                else if ( !god ) {
                    out() << "ERROR: attempt to insert in system namespace " << ns << endl;
                    return DiskLoc();
                }
            }
            else
                sys = 0;
        }

        bool addIndex = wouldAddIndex && mayAddIndex;

        NamespaceDetails *d = nsdetails(ns);
        if ( d == 0 ) {
            addNewNamespaceToCatalog(ns);
            /* todo: shouldn't be in the namespace catalog until after the allocations here work.
                     also if this is an addIndex, those checks should happen before this!
            */
            // This creates first file in the database.
            database->newestFile()->allocExtent(ns, initialExtentSize(len));
            d = nsdetails(ns);
            if ( !god )
                ensureIdIndexForNewNs(ns);
        }
        d->paddingFits();

        NamespaceDetails *tableToIndex = 0;

        string tabletoidxns;
        if ( addIndex ) {
            BSONObj io((const char *) obuf);
            const char *name = io.getStringField("name"); // name of the index
            tabletoidxns = io.getStringField("ns");  // table it indexes

            if ( database->name != nsToClient(tabletoidxns.c_str()) ) {
                uassert("bad table to index name on add index attempt", false);
                return DiskLoc();
            }

            BSONObj key = io.getObjectField("key");
            if( !validKeyPattern(key) ) {
                string s = string("bad index key pattern ") + key.toString();
                uassert(s.c_str(), false);
            }
            if ( *name == 0 || tabletoidxns.empty() || key.isEmpty() || key.objsize() > 2048 ) {
                out() << "user warning: bad add index attempt name:" << (name?name:"") << "\n  ns:" <<
                     tabletoidxns << "\n  ourns:" << ns;
                out() << "\n  idxobj:" << io.toString() << endl;
                string s = "bad add index attempt " + tabletoidxns + " key:" + key.toString();
                uasserted(s);
            }
            tableToIndex = nsdetails(tabletoidxns.c_str());
            if ( tableToIndex == 0 ) {
                // try to create it
                string err;
                if ( !userCreateNS(tabletoidxns.c_str(), BSONObj(), err, false) ) {
                    problem() << "ERROR: failed to create collection while adding its index. " << tabletoidxns << endl;
                    return DiskLoc();
                }
                tableToIndex = nsdetails(tabletoidxns.c_str());
                log() << "info: creating collection " << tabletoidxns << " on add index\n";
                assert( tableToIndex );
            }
            if ( tableToIndex->nIndexes >= MaxIndexes ) {
                stringstream ss;
                ss << "add index fails, too many indexes for " << tabletoidxns << " key:" << key.toString();
                string s = ss.str();
                log() << s << '\n';
                uasserted(s);
            }
            if ( tableToIndex->findIndexByName(name) >= 0 ) {
                //out() << "INFO: index:" << name << " already exists for:" << tabletoidxns << endl;
                return DiskLoc();
            }
            if ( !god && IndexDetails::isIdIndexPattern( key ) ) {
                ensureHaveIdIndex( tabletoidxns.c_str() );
                return DiskLoc();
            }
            //indexFullNS = tabletoidxns;
            //indexFullNS += ".$";
            //indexFullNS += name; // database.table.$index -- note this doesn't contain jsobjs, it contains BtreeBuckets.
        }

        const BSONElement *newId = &writeId;
        int addID = 0;
        if( !god ) {
            /* Check if we have an _id field. If we don't, we'll add it. 
               Note that btree buckets which we insert aren't BSONObj's, but in that case god==true.
            */
            BSONObj io((const char *) obuf);
            BSONElement idField = io.getField( "_id" );
            uassert( "_id cannot be an array", idField.type() != Array );
            if( idField.eoo() && !wouldAddIndex && strstr(ns, ".local.") == 0 ) {
                addID = len;
                if ( writeId.eoo() ) {
                    // Very likely we'll add this elt, so little harm in init'ing here.
                    idToInsert_.oid.init();
                    newId = &idToInsert;
                }
                len += newId->size();
            }
            
            BSONElementManipulator::lookForTimestamps( io );
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
                log(1) << "allocating new extent for " << ns << " padding:" << d->paddingFactor << " lenWHdr: " << lenWHdr << endl;
                database->newestFile()->allocExtent(ns, followupExtentSize(lenWHdr, d->lastExtentSize));
                loc = d->alloc(ns, lenWHdr, extentLoc);
                if ( loc.isNull() ){
                    log() << "WARNING: alloc() failed after allocating new extent. lenWHdr: " << lenWHdr << " last extent size:" << d->lastExtentSize << "; trying again\n";
                    for ( int zzz=0; zzz<10 && lenWHdr > d->lastExtentSize; zzz++ ){
                        log() << "try #" << zzz << endl;
                        database->newestFile()->allocExtent(ns, followupExtentSize(len, d->lastExtentSize));
                        loc = d->alloc(ns, lenWHdr, extentLoc);
                        if ( ! loc.isNull() )
                            break;
                    }
                }
            }
            if ( loc.isNull() ) {
                log() << "out of space in datafile " << ns << " capped:" << d->capped << endl;
                assert(d->capped);
                return DiskLoc();
            }
        }

        Record *r = loc.rec();
        assert( r->lengthWithHeaders >= lenWHdr );
        if( addID ) { 
            /* a little effort was made here to avoid a double copy when we add an ID */
            ((int&)*r->data) = *((int*) obuf) + newId->size();
            memcpy(r->data+4, newId->rawdata(), newId->size());
            memcpy(r->data+4+newId->size(), ((char *)obuf)+4, addID-4);
        }
        else {
            memcpy(r->data, obuf, len);
        }
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

        if ( !god )
            NamespaceDetailsTransient::get( ns ).registerWriteOp();
        
        if ( tableToIndex ) {
            int idxNo = tableToIndex->nIndexes;
            IndexDetails& idx = tableToIndex->indexes[idxNo];
            idx.info = loc;
            tableToIndex->addingIndex(tabletoidxns.c_str(), idx); // clear transient info caches so they refresh
            try {
                buildIndex(tabletoidxns, tableToIndex, idx, idxNo);
            } catch( DBException& ) {
                // roll back this index
                string name = idx.indexName();
                BSONObjBuilder b;
                string errmsg;
                bool ok = deleteIndexes(tableToIndex, tabletoidxns.c_str(), name.c_str(), errmsg, b, true);
                if( !ok ) {
                    log() << "failed to drop index after a unique key error building it: " << errmsg << ' ' << tabletoidxns << ' ' << name << endl;
                }
                throw;
            }
        }

        /* add this record to our indexes */
        if ( d->nIndexes ) {
            try { 
                indexRecord(d, r->data/*buf*/, len, loc);
            } 
            catch( AssertionException& e ) { 
                // should be a dup key error on _id index
                if( tableToIndex || d->capped ) { 
                    string s = e.toString();
                    s += " : on addIndex/capped - collection and its index will not match";
                    uassert_nothrow(s.c_str());
                    log() << s << '\n';
                }
                else { 
                    // normal case -- we can roll back
                    _deleteRecord(d, ns, r, loc);
                    throw;
                }
            }
        }

        //	out() << "   inserted at loc:" << hex << loc.getOfs() << " lenwhdr:" << hex << lenWHdr << dec << ' ' << ns << endl;
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

    void DataFileMgr::init(const string& path ) {
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

} // namespace mongo

#include "clientcursor.h"

namespace mongo {

    void dropDatabase(const char *ns) {
        // ns is of the form "<dbname>.$cmd"
        char cl[256];
        nsToClient(ns, cl);
        log(1) << "dropDatabase " << cl << endl;
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
        _applyOpToDataFiles( database, renamer, true );
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
        _applyOpToDataFiles( database, renamer, true, reservedPathString );
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
            boost::intmax_t size() const {
                return totalSize_;
            }
        private:
            virtual bool apply( const boost::filesystem::path &p ) {
                if ( !boost::filesystem::exists( p ) )
                    return false;
                totalSize_ += boost::filesystem::file_size( p );
                return true;
            }
            virtual const char *op() const {
                return "checking size";
            }
            boost::intmax_t totalSize_;
        };
        SizeAccumulator sa;
        _applyOpToDataFiles( database, sa );
        return sa.size();
    }

#if !defined(_WIN32)
} // namespace mongo
#include <sys/statvfs.h>
namespace mongo {
#endif
    boost::intmax_t freeSpace() {
#if !defined(_WIN32)
        struct statvfs info;
        assert( !statvfs( dbpath.c_str() , &info ) );
        return boost::intmax_t( info.f_bavail ) * info.f_frsize;
#else
        return -1;
#endif
    }

    bool repairDatabase( const char *ns, string &errmsg,
                         bool preserveClonedFilesOnFailure, bool backupOriginalFiles ) {
        stringstream ss;
        ss << "localhost:" << cmdLine.port;
        string localhost = ss.str();

        // ns is of the form "<dbname>.$cmd"
        char dbName[256];
        nsToClient(ns, dbName);
        problem() << "repairDatabase " << dbName << endl;
        assert( database->name == dbName );

        boost::intmax_t totalSize = dbSize( dbName );
        boost::intmax_t freeSize = freeSpace();
        if ( freeSize > -1 && freeSize < totalSize ) {
            stringstream ss;
            ss << "Cannot repair database " << dbName << " having size: " << totalSize
            << " (bytes) because free disk space is: " << freeSize << " (bytes)";
            errmsg = ss.str();
            problem() << errmsg << endl;
            return false;
        }

        Path reservedPath =
            uniqueReservedPath( ( preserveClonedFilesOnFailure || backupOriginalFiles ) ?
                                "backup" : "tmp" );
        BOOST_CHECK_EXCEPTION( boost::filesystem::create_directory( reservedPath ) );
        string reservedPathString = reservedPath.native_directory_string();
        assert( setClient( dbName, reservedPathString.c_str() ) );

        bool res = cloneFrom(localhost.c_str(), errmsg, dbName, 
            /*logForReplication=*/false, /*slaveok*/false, /*replauth*/false, /*snapshot*/false);
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

    void _applyOpToDataFiles( const char *database, FileOp &fo, bool afterAllocator, const string& path ) {
        if ( afterAllocator )
            theFileAllocator().waitUntilFinished();
        string c = database;
        c += '.';
        boost::filesystem::path p(path);
        boost::filesystem::path q;
        q = p / (c+"ns");
        bool ok = false;
        BOOST_CHECK_EXCEPTION( ok = fo.apply( q ) );
        if ( ok )
            log(2) << fo.op() << " file " << q.string() << '\n';
        int i = 0;
        int extra = 10; // should not be necessary, this is defensive in case there are missing files
        while ( 1 ) {
            assert( i <= DiskLoc::MaxFiles );
            stringstream ss;
            ss << c << i;
            q = p / ss.str();
            BOOST_CHECK_EXCEPTION( ok = fo.apply(q) );
            if ( ok ) {
                if ( extra != 10 ){
                    log(1) << fo.op() << " file " << q.string() << '\n';
                    log() << "  _applyOpToDataFiles() warning: extra == " << extra << endl;
                }
            }
            else if ( --extra <= 0 )
                break;
            i++;
        }
    }
    
} // namespace mongo
