// namespace.cpp

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

#include "pch.h"
#include "pdfile.h"
#include "db.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"
#include "../scripting/engine.h"
#include "btree.h"
#include <algorithm>
#include <list>
#include "query.h"
#include "queryutil.h"
#include "json.h"

namespace mongo {

    BSONObj idKeyPattern = fromjson("{\"_id\":1}");

    /* deleted lists -- linked lists of deleted records -- are placed in 'buckets' of various sizes
       so you can look for a deleterecord about the right size.
    */
    int bucketSizes[] = {
        32, 64, 128, 256, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000,
        0x8000, 0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000,
        0x400000, 0x800000
    };

    NamespaceDetails::NamespaceDetails( const DiskLoc &loc, bool _capped ) {
        /* be sure to initialize new fields here -- doesn't default to zeroes the way we use it */
        firstExtent = lastExtent = capExtent = loc;
        datasize = nrecords = 0;
        lastExtentSize = 0;
        nIndexes = 0;
        capped = _capped;
        max = 0x7fffffff;
        paddingFactor = 1.0;
        flags = 0;
        capFirstNewRecord = DiskLoc();
        // Signal that we are on first allocation iteration through extents.
        capFirstNewRecord.setInvalid();
        // For capped case, signal that we are doing initial extent allocation.
        if ( capped )
            cappedLastDelRecLastExtent().setInvalid();
		assert( sizeof(dataFileVersion) == 2 );
		dataFileVersion = 0;
		indexFileVersion = 0;
        multiKeyIndexBits = 0;
        reservedA = 0;
        extraOffset = 0;
        backgroundIndexBuildInProgress = 0;
        memset(reserved, 0, sizeof(reserved));
    }

    bool NamespaceIndex::exists() const {
        return !MMF::exists(path());
    }
    
    boost::filesystem::path NamespaceIndex::path() const {
        boost::filesystem::path ret( dir_ );
        if ( directoryperdb )
            ret /= database_;
        ret /= ( database_ + ".ns" );
        return ret;
    }

    void NamespaceIndex::maybeMkdir() const {
        if ( !directoryperdb )
            return;
        boost::filesystem::path dir( dir_ );
        dir /= database_;
        if ( !boost::filesystem::exists( dir ) )
            BOOST_CHECK_EXCEPTION( boost::filesystem::create_directory( dir ) );
    }
    
	int lenForNewNsFiles = 16 * 1024 * 1024;
    
    void NamespaceDetails::onLoad(const Namespace& k) { 
        if( k.isExtra() ) { 
            /* overflow storage for indexes - so don't treat as a NamespaceDetails object. */
            return;
        }

        assertInWriteLock();
        if( backgroundIndexBuildInProgress ) { 
            log() << "backgroundIndexBuildInProgress was " << backgroundIndexBuildInProgress << " for " << k << ", indicating an abnormal db shutdown" << endl;
            backgroundIndexBuildInProgress = 0;
        }
    }

    static void namespaceOnLoadCallback(const Namespace& k, NamespaceDetails& v) { 
        v.onLoad(k);
    }

    bool checkNsFilesOnLoad = true;

    void NamespaceIndex::init() {
        if ( ht )
            return;
        /* if someone manually deleted the datafiles for a database,
           we need to be sure to clear any cached info for the database in
           local.*.
        */
		/*
        if ( "local" != database_ ) {
            DBInfo i(database_.c_str());
            i.dbDropped();
        }
		*/
		int len = -1;
        boost::filesystem::path nsPath = path();
        string pathString = nsPath.string();
        MMF::Pointer p;
        if( MMF::exists(nsPath) ) { 
			p = f.map(pathString.c_str());
            if( !p.isNull() ) {
                len = f.length();
                if ( len % (1024*1024) != 0 ){
                    log() << "bad .ns file: " << pathString << endl;
                    uassert( 10079 ,  "bad .ns file length, cannot open database", len % (1024*1024) == 0 );
                }
            }
		}
		else {
			// use lenForNewNsFiles, we are making a new database
			massert( 10343 ,  "bad lenForNewNsFiles", lenForNewNsFiles >= 1024*1024 );
            maybeMkdir();
			long l = lenForNewNsFiles;
			p = f.map(pathString.c_str(), l);
            if( !p.isNull() ) {
                len = (int) l;
                assert( len == lenForNewNsFiles );
            }
		}

        if ( p.isNull() ) {
            problem() << "couldn't open file " << pathString << " terminating" << endl;
            dbexit( EXIT_FS );
        }

        ht = new HashTable<Namespace,NamespaceDetails,MMF::Pointer>(p, len, "namespace index");
        if( checkNsFilesOnLoad )
            ht->iterAll(namespaceOnLoadCallback);
    }
    
    static void namespaceGetNamespacesCallback( const Namespace& k , NamespaceDetails& v , void * extra ) {
        list<string> * l = (list<string>*)extra;
        if ( ! k.hasDollarSign() )
            l->push_back( (string)k );
    }

    void NamespaceIndex::getNamespaces( list<string>& tofill , bool onlyCollections ) const {
        assert( onlyCollections ); // TODO: need to implement this
        //                                  need boost::bind or something to make this less ugly
        
        if ( ht )
            ht->iterAll( namespaceGetNamespacesCallback , (void*)&tofill );
    }

    void NamespaceDetails::addDeletedRec(DeletedRecord *d, DiskLoc dloc) {
		BOOST_STATIC_ASSERT( sizeof(NamespaceDetails::Extra) <= sizeof(NamespaceDetails) );
        {
            // defensive code: try to make us notice if we reference a deleted record
            (unsigned&) (((Record *) d)->data) = 0xeeeeeeee;
        }
        dassert( dloc.drec() == d );
        DEBUGGING out() << "TEMP: add deleted rec " << dloc.toString() << ' ' << hex << d->extentOfs << endl;
        if ( capped ) {
            if ( !cappedLastDelRecLastExtent().isValid() ) {
                // Initial extent allocation.  Insert at end.
                d->nextDeleted = DiskLoc();
                if ( cappedListOfAllDeletedRecords().isNull() )
                    cappedListOfAllDeletedRecords() = dloc;
                else {
                    DiskLoc i = cappedListOfAllDeletedRecords();
                    for (; !i.drec()->nextDeleted.isNull(); i = i.drec()->nextDeleted );
                    i.drec()->nextDeleted = dloc;
                }
            } else {
                d->nextDeleted = cappedFirstDeletedInCurExtent();
                cappedFirstDeletedInCurExtent() = dloc;
                // always compact() after this so order doesn't matter
            }
        } else {
            int b = bucket(d->lengthWithHeaders);
            DiskLoc& list = deletedList[b];
            DiskLoc oldHead = list;
            list = dloc;
            d->nextDeleted = oldHead;
        }
    }

    /*
       lenToAlloc is WITH header
    */
    DiskLoc NamespaceDetails::alloc(const char *ns, int lenToAlloc, DiskLoc& extentLoc) {
        lenToAlloc = (lenToAlloc + 3) & 0xfffffffc;
        DiskLoc loc = _alloc(ns, lenToAlloc);
        if ( loc.isNull() )
            return loc;

        DeletedRecord *r = loc.drec();

        /* note we want to grab from the front so our next pointers on disk tend
        to go in a forward direction which is important for performance. */
        int regionlen = r->lengthWithHeaders;
        extentLoc.set(loc.a(), r->extentOfs);
        assert( r->extentOfs < loc.getOfs() );

        DEBUGGING out() << "TEMP: alloc() returns " << loc.toString() << ' ' << ns << " lentoalloc:" << lenToAlloc << " ext:" << extentLoc.toString() << endl;

        int left = regionlen - lenToAlloc;
        if ( capped == 0 ) {
            if ( left < 24 || left < (lenToAlloc >> 3) ) {
                // you get the whole thing.
				DataFileMgr::grow(loc, regionlen);
                return loc;
            }
        }

        /* split off some for further use. */
        r->lengthWithHeaders = lenToAlloc;
		DataFileMgr::grow(loc, lenToAlloc);
        DiskLoc newDelLoc = loc;
        newDelLoc.inc(lenToAlloc);
        DeletedRecord *newDel = DataFileMgr::makeDeletedRecord(newDelLoc, left);
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
        DiskLoc cur = deletedList[b];
        prev = &deletedList[b];
        int extra = 5; // look for a better fit, a little.
        int chain = 0;
        while ( 1 ) {
            {
                int a = cur.a();
                if ( a < -1 || a >= 100000 ) {
                    problem() << "~~ Assertion - cur out of range in _alloc() " << cur.toString() <<
                    " a:" << a << " b:" << b << " chain:" << chain << '\n';
                    sayDbContext();
                    if ( cur == *prev )
                        prev->Null();
                    cur.Null();
                }
            }
            if ( cur.isNull() ) {
                // move to next bucket.  if we were doing "extra", just break
                if ( bestmatchlen < 0x7fffffff )
                    break;
                b++;
                if ( b > MaxBucket ) {
                    // out of space. alloc a new extent.
                    return DiskLoc();
                }
                cur = deletedList[b];
                prev = &deletedList[b];
                continue;
            }
            DeletedRecord *r = cur.drec();
            if ( r->lengthWithHeaders >= len &&
                    r->lengthWithHeaders < bestmatchlen ) {
                bestmatchlen = r->lengthWithHeaders;
                bestmatch = cur;
                bestprev = prev;
            }
            if ( bestmatchlen < 0x7fffffff && --extra <= 0 )
                break;
            if ( ++chain > 30 && b < MaxBucket ) {
                // too slow, force move to next bucket to grab a big chunk
                //b++;
                chain = 0;
                cur.Null();
            }
            else {
                /*this defensive check only made sense for the mmap storage engine: 
                  if ( r->nextDeleted.getOfs() == 0 ) {
                    problem() << "~~ Assertion - bad nextDeleted " << r->nextDeleted.toString() <<
                    " b:" << b << " chain:" << chain << ", fixing.\n";
                    r->nextDeleted.Null();
                }*/
                cur = r->nextDeleted;
                prev = &r->nextDeleted;
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
        for ( int i = 0; i < Buckets; i++ ) {
            DiskLoc dl = deletedList[i];
            while ( !dl.isNull() ) {
                DeletedRecord *r = dl.drec();
                DiskLoc extLoc(dl.a(), r->extentOfs);
                if ( extents == 0 || extents->count(extLoc) <= 0 ) {
                    out() << "  bucket " << i << endl;
                    out() << "   " << dl.toString() << " ext:" << extLoc.toString();
                    if ( extents && extents->count(extLoc) <= 0 )
                        out() << '?';
                    out() << " len:" << r->lengthWithHeaders << endl;
                }
                dl = r->nextDeleted;
            }
        }
    }

    DiskLoc NamespaceDetails::firstRecord( const DiskLoc &startExtent ) const {
        for (DiskLoc i = startExtent.isNull() ? firstExtent : startExtent;
                !i.isNull(); i = i.ext()->xnext ) {
            if ( !i.ext()->firstRecord.isNull() )
                return i.ext()->firstRecord;
        }
        return DiskLoc();
    }

    DiskLoc NamespaceDetails::lastRecord( const DiskLoc &startExtent ) const {
        for (DiskLoc i = startExtent.isNull() ? lastExtent : startExtent;
                !i.isNull(); i = i.ext()->xprev ) {
            if ( !i.ext()->lastRecord.isNull() )
                return i.ext()->lastRecord;
        }
        return DiskLoc();
    }

    int n_complaints_cap = 0;
    void NamespaceDetails::maybeComplain( const char *ns, int len ) const {
        if ( ++n_complaints_cap < 8 ) {
            out() << "couldn't make room for new record (len: " << len << ") in capped ns " << ns << '\n';
            int i = 0;
            for ( DiskLoc e = firstExtent; !e.isNull(); e = e.ext()->xnext, ++i ) {
                out() << "  Extent " << i;
                if ( e == capExtent )
                    out() << " (capExtent)";
                out() << '\n';
                out() << "    magic: " << hex << e.ext()->magic << dec << " extent->ns: " << e.ext()->nsDiagnostic.buf << '\n';
                out() << "    fr: " << e.ext()->firstRecord.toString() <<
                     " lr: " << e.ext()->lastRecord.toString() << " extent->len: " << e.ext()->length << '\n';
            }
            assert( len * 5 > lastExtentSize ); // assume it is unusually large record; if not, something is broken
        }
    }

    /* alloc with capped table handling. */
    DiskLoc NamespaceDetails::_alloc(const char *ns, int len) {
        if ( !capped )
            return __stdAlloc(len);

        return cappedAlloc(ns,len);
    }

    /* extra space for indexes when more than 10 */
    NamespaceDetails::Extra* NamespaceIndex::newExtra(const char *ns, int i, NamespaceDetails *d) {
        assert( i >= 0 && i <= 1 );
        Namespace n(ns);
        Namespace extra(n.extraName(i).c_str()); // throws userexception if ns name too long
        
        massert( 10350 ,  "allocExtra: base ns missing?", d );
        massert( 10351 ,  "allocExtra: extra already exists", ht->get(extra) == 0 );

        NamespaceDetails::Extra temp;
        temp.init();
        uassert( 10082 ,  "allocExtra: too many namespaces/collections", ht->put(extra, (NamespaceDetails&) temp));
        NamespaceDetails::Extra *e = (NamespaceDetails::Extra *) ht->get(extra);
        return e;
    }
    NamespaceDetails::Extra* NamespaceDetails::allocExtra(const char *ns, int nindexessofar) {
        NamespaceIndex *ni = nsindex(ns);
        int i = (nindexessofar - NIndexesBase) / NIndexesExtra;
        Extra *e = ni->newExtra(ns, i, this);
        long ofs = e->ofsFrom(this);
        if( i == 0 ) {
            assert( extraOffset == 0 );
            extraOffset = ofs;
            assert( extra() == e );
        }
        else { 
            Extra *hd = extra();
            assert( hd->next(this) == 0 );
            hd->setNext(ofs);
        }
        return e;
    }

    /* you MUST call when adding an index.  see pdfile.cpp */
    IndexDetails& NamespaceDetails::addIndex(const char *thisns, bool resetTransient) {
        assert( nsdetails(thisns) == this );

        IndexDetails *id;
        try {
            id = &idx(nIndexes,true);
        }
        catch(DBException&) { 
            allocExtra(thisns, nIndexes);
            id = &idx(nIndexes,false);
        }

        nIndexes++;
        if ( resetTransient )
            NamespaceDetailsTransient::get_w(thisns).addedIndex();
        return *id;
    }

    // must be called when renaming a NS to fix up extra
    void NamespaceDetails::copyingFrom(const char *thisns, NamespaceDetails *src) { 
        extraOffset = 0; // we are a copy -- the old value is wrong.  fixing it up below.
        Extra *se = src->extra();
        int n = NIndexesBase;
        if( se ) {
            Extra *e = allocExtra(thisns, n);
            while( 1 ) {
                n += NIndexesExtra;
                e->copy(this, *se);
                se = se->next(src);
                if( se == 0 ) break;
                Extra *nxt = allocExtra(thisns, n);
                e->setNext( nxt->ofsFrom(this) );
                e = nxt;
            } 
            assert( extraOffset );
        }
    }

    /* returns index of the first index in which the field is present. -1 if not present.
       (aug08 - this method not currently used)
    */
    int NamespaceDetails::fieldIsIndexed(const char *fieldName) {
        massert( 10346 , "not implemented", false);
        /*
        for ( int i = 0; i < nIndexes; i++ ) {
            IndexDetails& idx = indexes[i];
            BSONObj idxKey = idx.info.obj().getObjectField("key"); // e.g., { ts : -1 }
            if ( !idxKey.getField(fieldName).eoo() )
                return i;
        }*/
        return -1;
    }
    
    long long NamespaceDetails::storageSize( int * numExtents ){
        Extent * e = firstExtent.ext();
        assert( e );
        
        long long total = 0;
        int n = 0;
        while ( e ){
            total += e->length;
            e = e->getNextExtent();
            n++;
        }
        
        if ( numExtents )
            *numExtents = n;
        
        return total;
    }
    
    /* ------------------------------------------------------------------------- */

    mongo::mutex NamespaceDetailsTransient::_qcMutex("qc");
    mongo::mutex NamespaceDetailsTransient::_isMutex("is");
    map< string, shared_ptr< NamespaceDetailsTransient > > NamespaceDetailsTransient::_map;
    typedef map< string, shared_ptr< NamespaceDetailsTransient > >::iterator ouriter;

    void NamespaceDetailsTransient::reset() {
        DEV assertInWriteLock();
        clearQueryCache();
        _keysComputed = false;
        _indexSpecs.clear();
    }
    
/*    NamespaceDetailsTransient& NamespaceDetailsTransient::get(const char *ns) {
        shared_ptr< NamespaceDetailsTransient > &t = map_[ ns ];
        if ( t.get() == 0 )
            t.reset( new NamespaceDetailsTransient(ns) );
        return *t;
    }
*/
    void NamespaceDetailsTransient::clearForPrefix(const char *prefix) {
        assertInWriteLock();
        vector< string > found;
        for( ouriter i = _map.begin(); i != _map.end(); ++i )
            if ( strncmp( i->first.c_str(), prefix, strlen( prefix ) ) == 0 )
                found.push_back( i->first );
        for( vector< string >::iterator i = found.begin(); i != found.end(); ++i ) {
            _map[ *i ].reset();
        }
    }
    
    void NamespaceDetailsTransient::computeIndexKeys() {
        _keysComputed = true;
        _indexKeys.clear();
        NamespaceDetails *d = nsdetails(_ns.c_str());
        if ( ! d )
            return;
        NamespaceDetails::IndexIterator i = d->ii();
        while( i.more() )
            i.next().keyPattern().getFieldNames(_indexKeys);
    }


    /* ------------------------------------------------------------------------- */

    /* add a new namespace to the system catalog (<dbname>.system.namespaces).
       options: { capped : ..., size : ... }
    */
    void addNewNamespaceToCatalog(const char *ns, const BSONObj *options = 0) {
        log(1) << "New namespace: " << ns << '\n';
        if ( strstr(ns, "system.namespaces") ) {
            // system.namespaces holds all the others, so it is not explicitly listed in the catalog.
            // TODO: fix above should not be strstr!
            return;
        }

        {
            BSONObjBuilder b;
            b.append("name", ns);
            if ( options )
                b.append("options", *options);
            BSONObj j = b.done();
            char database[256];
            nsToDatabase(ns, database);
            string s = database;
            s += ".system.namespaces";
            theDataFileMgr.insert(s.c_str(), j.objdata(), j.objsize(), true);
        }
    }

    void renameNamespace( const char *from, const char *to ) {
		NamespaceIndex *ni = nsindex( from );
		assert( ni && ni->details( from ) && !ni->details( to ) );
		
		// Our namespace and index details will move to a different 
		// memory location.  The only references to namespace and 
		// index details across commands are in cursors and nsd
		// transient (including query cache) so clear these.
		ClientCursor::invalidate( from );
		NamespaceDetailsTransient::clearForPrefix( from );

		NamespaceDetails *details = ni->details( from );
		ni->add_ns( to, *details );
        NamespaceDetails *todetails = ni->details( to );
        try { 
            todetails->copyingFrom(to, details); // fixes extraOffset
        }
        catch( DBException& ) { 
            // could end up here if .ns is full - if so try to clean up / roll back a little
            ni->kill_ns(to);
            throw;
        }
		ni->kill_ns( from );
		details = todetails;
		
		BSONObj oldSpec;
		char database[MaxDatabaseNameLen];
		nsToDatabase(from, database);
		string s = database;
		s += ".system.namespaces";
		assert( Helpers::findOne( s.c_str(), BSON( "name" << from ), oldSpec ) );
		
		BSONObjBuilder newSpecB;
		BSONObjIterator i( oldSpec.getObjectField( "options" ) );
		while( i.more() ) {
			BSONElement e = i.next();
			if ( strcmp( e.fieldName(), "create" ) != 0 )
				newSpecB.append( e );
			else
				newSpecB << "create" << to;
		}
		BSONObj newSpec = newSpecB.done();    
		addNewNamespaceToCatalog( to, newSpec.isEmpty() ? 0 : &newSpec );

		deleteObjects( s.c_str(), BSON( "name" << from ), false, false, true );
		// oldSpec variable no longer valid memory

		BSONObj oldIndexSpec;
		s = database;
		s += ".system.indexes";
		while( Helpers::findOne( s.c_str(), BSON( "ns" << from ), oldIndexSpec ) ) {
			BSONObjBuilder newIndexSpecB;
			BSONObjIterator i( oldIndexSpec );
			while( i.more() ) {
				BSONElement e = i.next();
				if ( strcmp( e.fieldName(), "ns" ) != 0 )
					newIndexSpecB.append( e );
				else
					newIndexSpecB << "ns" << to;
			}
			BSONObj newIndexSpec = newIndexSpecB.done();
			DiskLoc newIndexSpecLoc = theDataFileMgr.insert( s.c_str(), newIndexSpec.objdata(), newIndexSpec.objsize(), true, BSONElement(), false );
			int indexI = details->findIndexByName( oldIndexSpec.getStringField( "name" ) );
			IndexDetails &indexDetails = details->idx(indexI);
			string oldIndexNs = indexDetails.indexNamespace();
			indexDetails.info = newIndexSpecLoc;
			string newIndexNs = indexDetails.indexNamespace();
			
			BtreeBucket::renameIndexNamespace( oldIndexNs.c_str(), newIndexNs.c_str() );
			deleteObjects( s.c_str(), oldIndexSpec.getOwned(), true, false, true );
		}
	}

    bool legalClientSystemNS( const string& ns , bool write ){
        if( ns == "local.system.replset" ) return true;

        if ( ns.find( ".system.users" ) != string::npos )
            return true;

        if ( ns.find( ".system.js" ) != string::npos ){
            if ( write )
                Scope::storedFuncMod();
            return true;
        }
        
        return false;
    }
	
} // namespace mongo
