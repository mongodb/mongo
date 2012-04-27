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

#include "mongo/db/namespace_details.h"

#include <algorithm>
#include <list>

#include <boost/filesystem/operations.hpp>

#include "mongo/db/btree.h"
#include "mongo/db/db.h"
#include "mongo/db/json.h"
#include "mongo/db/mongommf.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/pdfile.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/hashtab.h"
#include "mongo/util/util.h"



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

    NamespaceDetails::NamespaceDetails( const DiskLoc &loc, bool capped ) {
        /* be sure to initialize new fields here -- doesn't default to zeroes the way we use it */
        firstExtent = lastExtent = capExtent = loc;
        stats.datasize = stats.nrecords = 0;
        lastExtentSize = 0;
        nIndexes = 0;
        _isCapped = capped;
        _maxDocsInCapped = 0x7fffffff;
        _paddingFactor = 1.0;
        _systemFlags = 0;
        _userFlags = 0;
        capFirstNewRecord = DiskLoc();
        // Signal that we are on first allocation iteration through extents.
        capFirstNewRecord.setInvalid();
        // For capped case, signal that we are doing initial extent allocation.
        if ( capped )
            cappedLastDelRecLastExtent().setInvalid();
        verify( sizeof(dataFileVersion) == 2 );
        dataFileVersion = 0;
        indexFileVersion = 0;
        multiKeyIndexBits = 0;
        reservedA = 0;
        extraOffset = 0;
        indexBuildInProgress = 0;
        memset(reserved, 0, sizeof(reserved));
    }

    bool NamespaceIndex::exists() const {
        return !boost::filesystem::exists(path());
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
            MONGO_ASSERT_ON_EXCEPTION_WITH_MSG( boost::filesystem::create_directory( dir ), "create dir for db " );
    }

    unsigned lenForNewNsFiles = 16 * 1024 * 1024;

#if defined(_DEBUG)
    void NamespaceDetails::dump(const Namespace& k) {
        if( !cmdLine.dur )
            cout << "ns offsets which follow will not display correctly with --journal disabled" << endl;

        size_t ofs = 1; // 1 is sentinel that the find call below failed
        privateViews.find(this, /*out*/ofs);

        cout << "ns" << hex << setw(8) << ofs << ' ';
        cout << k.toString() << '\n';

        if( k.isExtra() ) {
            cout << "ns\t extra" << endl;
            return;
        }

        cout << "ns         " << firstExtent.toString() << ' ' << lastExtent.toString() << " nidx:" << nIndexes << '\n';
        cout << "ns         " << stats.datasize << ' ' << stats.nrecords << ' ' << nIndexes << '\n';
        cout << "ns         " << isCapped() << ' ' << _paddingFactor << ' ' << _systemFlags << ' ' << _userFlags << ' ' << dataFileVersion << '\n';
        cout << "ns         " << multiKeyIndexBits << ' ' << indexBuildInProgress << '\n';
        cout << "ns         " << (int) reserved[0] << ' ' << (int) reserved[59];
        cout << endl;
    }
#endif

    void NamespaceDetails::onLoad(const Namespace& k) {

        if( k.isExtra() ) {
            /* overflow storage for indexes - so don't treat as a NamespaceDetails object. */
            return;
        }

        if( indexBuildInProgress ) {
            verify( Lock::isW() ); // TODO(erh) should this be per db?
            if( indexBuildInProgress ) {
                log() << "indexBuildInProgress was " << indexBuildInProgress << " for " << k << ", indicating an abnormal db shutdown" << endl;
                getDur().writingInt( indexBuildInProgress ) = 0;
            }
        }
    }

    static void namespaceOnLoadCallback(const Namespace& k, NamespaceDetails& v) {
        v.onLoad(k);
    }

    bool checkNsFilesOnLoad = true;

    NOINLINE_DECL void NamespaceIndex::_init() {
        verify( !ht );

        Lock::assertWriteLocked(database_);

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

        unsigned long long len = 0;
        boost::filesystem::path nsPath = path();
        string pathString = nsPath.string();
        void *p = 0;
        if( boost::filesystem::exists(nsPath) ) {
            if( f.open(pathString, true) ) {
                len = f.length();
                if ( len % (1024*1024) != 0 ) {
                    log() << "bad .ns file: " << pathString << endl;
                    uassert( 10079 ,  "bad .ns file length, cannot open database", len % (1024*1024) == 0 );
                }
                p = f.getView();
            }
        }
        else {
            // use lenForNewNsFiles, we are making a new database
            massert( 10343, "bad lenForNewNsFiles", lenForNewNsFiles >= 1024*1024 );
            maybeMkdir();
            unsigned long long l = lenForNewNsFiles;
            if( f.create(pathString, l, true) ) {
                getDur().createdFile(pathString, l); // always a new file
                len = l;
                verify( len == lenForNewNsFiles );
                p = f.getView();
            }
        }

        if ( p == 0 ) {
            /** TODO: this shouldn't terminate? */
            log() << "error couldn't open file " << pathString << " terminating" << endl;
            dbexit( EXIT_FS );
        }


        verify( len <= 0x7fffffff );
        ht = new HashTable<Namespace,NamespaceDetails>(p, (int) len, "namespace index");
        if( checkNsFilesOnLoad )
            ht->iterAll(namespaceOnLoadCallback);
    }

    static void namespaceGetNamespacesCallback( const Namespace& k , NamespaceDetails& v , void * extra ) {
        list<string> * l = (list<string>*)extra;
        if ( ! k.hasDollarSign() )
            l->push_back( (string)k );
    }
    void NamespaceIndex::getNamespaces( list<string>& tofill , bool onlyCollections ) const {
        verify( onlyCollections ); // TODO: need to implement this
        //                                  need boost::bind or something to make this less ugly

        if ( ht )
            ht->iterAll( namespaceGetNamespacesCallback , (void*)&tofill );
    }

    void NamespaceDetails::addDeletedRec(DeletedRecord *d, DiskLoc dloc) {
        BOOST_STATIC_ASSERT( sizeof(NamespaceDetails::Extra) <= sizeof(NamespaceDetails) );

        {
            Record *r = (Record *) getDur().writingPtr(d, sizeof(Record));
            d = &r->asDeleted();
            // defensive code: try to make us notice if we reference a deleted record
            little<unsigned>::ref( r->data() ) = 0xeeeeeeee; 
        }
        DEBUGGING log() << "TEMP: add deleted rec " << dloc.toString() << ' ' << hex << d->extentOfs() << endl;
        if ( isCapped() ) {
            if ( !cappedLastDelRecLastExtent().isValid() ) {
                // Initial extent allocation.  Insert at end.
                d->nextDeleted() = DiskLoc();
                if ( cappedListOfAllDeletedRecords().isNull() )
                    getDur().writingDiskLoc( cappedListOfAllDeletedRecords() ) = dloc;
                else {
                    DiskLoc i = cappedListOfAllDeletedRecords();
                    for (; !i.drec()->nextDeleted().isNull(); i = i.drec()->nextDeleted() )
                        ;
                    i.drec()->nextDeleted().writing() = dloc;
                }
            }
            else {
                d->nextDeleted() = cappedFirstDeletedInCurExtent();
                getDur().writingDiskLoc( cappedFirstDeletedInCurExtent() ) = dloc;
                // always compact() after this so order doesn't matter
            }
        }
        else {
            int b = bucket(d->lengthWithHeaders());
            DiskLoc& list = deletedList[b];
            DiskLoc oldHead = list;
            getDur().writingDiskLoc(list) = dloc;
            d->nextDeleted() = oldHead;
        }
    }

    /* predetermine location of the next alloc without actually doing it. 
        if cannot predetermine returns null (so still call alloc() then)
    */
    DiskLoc NamespaceDetails::allocWillBeAt(const char *ns, int lenToAlloc) {
        if ( ! isCapped() ) {
            lenToAlloc = (lenToAlloc + 3) & 0xfffffffc;
            return __stdAlloc(lenToAlloc, true);
        }
        return DiskLoc();
    }

    /** allocate space for a new record from deleted lists.
        @param lenToAlloc is WITH header
        @param extentLoc OUT returns the extent location
        @return null diskloc if no room - allocate a new extent then
    */
    DiskLoc NamespaceDetails::alloc(const char *ns, int lenToAlloc, DiskLoc& extentLoc) {
        {
            // align very slightly.  
            // note that if doing more coarse-grained quantization (really just if it isn't always
            //   a constant amount but if it varied by record size) then that quantization should 
            //   NOT be done here but rather in __stdAlloc so that we can grab a deletedrecord that 
            //   is just big enough if we happen to run into one.
            lenToAlloc = (lenToAlloc + 3) & 0xfffffffc;
        }

        DiskLoc loc = _alloc(ns, lenToAlloc);
        if ( loc.isNull() )
            return loc;

        DeletedRecord *r = loc.drec();
        //r = getDur().writing(r);

        /* note we want to grab from the front so our next pointers on disk tend
        to go in a forward direction which is important for performance. */
        int regionlen = r->lengthWithHeaders();
        extentLoc.set(loc.a(), r->extentOfs());
        verify( r->extentOfs() < loc.getOfs() );

        DEBUGGING out() << "TEMP: alloc() returns " << loc.toString() << ' ' << ns << " lentoalloc:" << lenToAlloc << " ext:" << extentLoc.toString() << endl;

        int left = regionlen - lenToAlloc;
        if ( ! isCapped() ) {
            if ( left < 24 || left < (lenToAlloc >> 3) ) {
                // you get the whole thing.
                return loc;
            }
        }

        /* split off some for further use. */
        getDur().writingInt(r->lengthWithHeaders()) = lenToAlloc;
        DiskLoc newDelLoc = loc;
        newDelLoc.inc(lenToAlloc);
        DeletedRecord *newDel = DataFileMgr::makeDeletedRecord(newDelLoc, left);
        DeletedRecord *newDelW = getDur().writing(newDel);
        newDelW->extentOfs() = r->extentOfs();
        newDelW->lengthWithHeaders() = left;
        newDelW->nextDeleted().Null();

        addDeletedRec(newDel, newDelLoc);

        return loc;
    }

    /* for non-capped collections.
       @param peekOnly just look up where and don't reserve
       returned item is out of the deleted list upon return
    */
    DiskLoc NamespaceDetails::__stdAlloc(int len, bool peekOnly) {
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
            if ( r->lengthWithHeaders() >= len &&
                 r->lengthWithHeaders() < bestmatchlen ) {
                bestmatchlen = r->lengthWithHeaders();
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
                cur = r->nextDeleted();
                prev = &r->nextDeleted();
            }
        }

        /* unlink ourself from the deleted list */
        if( !peekOnly ) {
            DeletedRecord *bmr = bestmatch.drec();
            *getDur().writing(bestprev) = bmr->nextDeleted();
            bmr->nextDeleted().writing().setInvalid(); // defensive.
            verify(bmr->extentOfs() < bestmatch.getOfs());
        }

        return bestmatch;
    }

    void NamespaceDetails::dumpDeleted(set<DiskLoc> *extents) {
        for ( int i = 0; i < Buckets; i++ ) {
            DiskLoc dl = deletedList[i];
            while ( !dl.isNull() ) {
                DeletedRecord *r = dl.drec();
                DiskLoc extLoc(dl.a(), r->extentOfs());
                if ( extents == 0 || extents->count(extLoc) <= 0 ) {
                    out() << "  bucket " << i << endl;
                    out() << "   " << dl.toString() << " ext:" << extLoc.toString();
                    if ( extents && extents->count(extLoc) <= 0 )
                        out() << '?';
                    out() << " len:" << r->lengthWithHeaders() << endl;
                }
                dl = r->nextDeleted();
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
                out() << "    magic: " << hex << e.ext()->magic << dec << " extent->ns: " << e.ext()->nsDiagnostic.toString() << '\n';
                out() << "    fr: " << e.ext()->firstRecord.toString() <<
                      " lr: " << e.ext()->lastRecord.toString() << " extent->len: " << e.ext()->length << '\n';
            }
            verify( len * 5 > lastExtentSize ); // assume it is unusually large record; if not, something is broken
        }
    }

    /* alloc with capped table handling. */
    DiskLoc NamespaceDetails::_alloc(const char *ns, int len) {
        if ( ! isCapped() )
            return __stdAlloc(len, false);

        return cappedAlloc(ns,len);
    }

    void NamespaceIndex::kill_ns(const char *ns) {
        Lock::assertWriteLocked(ns);
        if ( !ht )
            return;
        Namespace n(ns);
        ht->kill(n);

        for( int i = 0; i<=1; i++ ) {
            try {
                Namespace extra(n.extraName(i).c_str());
                ht->kill(extra);
            }
            catch(DBException&) { 
                dlog(3) << "caught exception in kill_ns" << endl;
            }
        }
    }

    void NamespaceIndex::add_ns(const char *ns, DiskLoc& loc, bool capped) {
        NamespaceDetails details( loc, capped );
        add_ns( ns, details );
    }
    void NamespaceIndex::add_ns( const char *ns, const NamespaceDetails &details ) {
        Lock::assertWriteLocked(ns);
        init();
        Namespace n(ns);
        uassert( 10081 , "too many namespaces/collections", ht->put(n, details));
    }

    /* extra space for indexes when more than 10 */
    NamespaceDetails::Extra* NamespaceIndex::newExtra(const char *ns, int i, NamespaceDetails *d) {
        Lock::assertWriteLocked(ns);
        verify( i >= 0 && i <= 1 );
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
            verify( extraOffset == 0 );
            *getDur().writing(&extraOffset) = ofs;
            verify( extra() == e );
        }
        else {
            Extra *hd = extra();
            verify( hd->next(this) == 0 );
            hd->setNext(ofs);
        }
        return e;
    }

    void NamespaceDetails::setIndexIsMultikey(const char *thisns, int i) {
        dassert( i < NIndexesMax );
        unsigned long long x = ((unsigned long long) 1) << i;
        if( multiKeyIndexBits & x ) return;
        *getDur().writing(&multiKeyIndexBits) |= x;
        NamespaceDetailsTransient::get(thisns).clearQueryCache();
    }

    /* you MUST call when adding an index.  see pdfile.cpp */
    IndexDetails& NamespaceDetails::addIndex(const char *thisns, bool resetTransient) {
        IndexDetails *id;
        try {
            id = &idx(nIndexes,true);
        }
        catch(DBException&) {
            allocExtra(thisns, nIndexes);
            id = &idx(nIndexes,false);
        }

        (*getDur().writing(&nIndexes))++;
        if ( resetTransient )
            NamespaceDetailsTransient::get(thisns).addedIndex();
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
            verify( extraOffset );
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

    long long NamespaceDetails::storageSize( int * numExtents , BSONArrayBuilder * extentInfo ) const {
        Extent * e = firstExtent.ext();
        verify( e );

        long long total = 0;
        int n = 0;
        while ( e ) {
            total += e->length;
            n++;

            if ( extentInfo ) {
                extentInfo->append( BSON( "len" << e->length << "loc: " << e->myLoc.toBSONObj() ) );
            }

            e = e->getNextExtent();
        }

        if ( numExtents )
            *numExtents = n;

        return total;
    }

    NamespaceDetails *NamespaceDetails::writingWithExtra() {
        vector< pair< long long, unsigned > > writeRanges;
        writeRanges.push_back( make_pair( 0, sizeof( NamespaceDetails ) ) );
        for( Extra *e = extra(); e; e = e->next( this ) ) {
            writeRanges.push_back( make_pair( (char*)e - (char*)this, sizeof( Extra ) ) );
        }
        return reinterpret_cast< NamespaceDetails* >( getDur().writingRangesAtOffsets( this, writeRanges ) );
    }

    void NamespaceDetails::setMaxCappedDocs( long long max ) {
        verify( max <= 0x7fffffffLL ); // TODO: this is temp
        _maxDocsInCapped = static_cast<int>(max);
    }

    /* ------------------------------------------------------------------------- */

    SimpleMutex NamespaceDetailsTransient::_qcMutex("qc");
    SimpleMutex NamespaceDetailsTransient::_isMutex("is");
    map< string, shared_ptr< NamespaceDetailsTransient > > NamespaceDetailsTransient::_nsdMap;
    typedef map< string, shared_ptr< NamespaceDetailsTransient > >::iterator ouriter;

    void NamespaceDetailsTransient::reset() {
        Lock::assertWriteLocked(_ns); 
        clearQueryCache();
        _keysComputed = false;
        _indexSpecs.clear();
    }

    /*static*/ NOINLINE_DECL NamespaceDetailsTransient& NamespaceDetailsTransient::make_inlock(const char *ns) {
        shared_ptr< NamespaceDetailsTransient > &t = _nsdMap[ ns ];
        verify( t.get() == 0 );
        Database *database = cc().database();
        verify( database );
        if( _nsdMap.size() % 20000 == 10000 ) { 
            // so we notice if insanely large #s
            log() << "opening namespace " << ns << endl;
            log() << _nsdMap.size() << " namespaces in nsdMap" << endl;
        }
        t.reset( new NamespaceDetailsTransient(database, ns) );
        return *t;
    }

    // note with repair there could be two databases with the same ns name.
    // that is NOT handled here yet!  TODO
    // repair may not use nsdt though not sure.  anyway, requires work.
    NamespaceDetailsTransient::NamespaceDetailsTransient(Database *db, const char *ns) : 
        _ns(ns), _keysComputed(false), _qcWriteCount() 
    {
        dassert(db);
    }

    NamespaceDetailsTransient::~NamespaceDetailsTransient() { 
    }

    void NamespaceDetailsTransient::clearForPrefix(const char *prefix) {
        SimpleMutex::scoped_lock lk(_qcMutex);
        vector< string > found;
        for( ouriter i = _nsdMap.begin(); i != _nsdMap.end(); ++i ) {
            if ( strncmp( i->first.c_str(), prefix, strlen( prefix ) ) == 0 ) {
                found.push_back( i->first );
                Lock::assertWriteLocked(i->first);
            }
        }
        for( vector< string >::iterator i = found.begin(); i != found.end(); ++i ) {
            _nsdMap[ *i ].reset();
        }
    }

    void NamespaceDetailsTransient::eraseForPrefix(const char *prefix) {
        SimpleMutex::scoped_lock lk(_qcMutex);
        vector< string > found;
        for( ouriter i = _nsdMap.begin(); i != _nsdMap.end(); ++i ) {
            if ( strncmp( i->first.c_str(), prefix, strlen( prefix ) ) == 0 ) {
                found.push_back( i->first );
                Lock::assertWriteLocked(i->first);
            }
        }
        for( vector< string >::iterator i = found.begin(); i != found.end(); ++i ) {
            _nsdMap.erase(*i);
        }
    }

    void NamespaceDetailsTransient::computeIndexKeys() {
        _indexKeys.clear();
        NamespaceDetails *d = nsdetails(_ns.c_str());
        if ( ! d )
            return;
        NamespaceDetails::IndexIterator i = d->ii();
        while( i.more() )
            i.next().keyPattern().getFieldNames(_indexKeys);
        _keysComputed = true;
    }

    void NamespaceDetails::setSystemFlag( int flag ) {
        getDur().writingInt(_systemFlags) |= flag;
    }

    void NamespaceDetails::clearSystemFlag( int flag ) {
        getDur().writingInt(_systemFlags) &= ~flag;
    }

    void NamespaceDetails::setUserFlag( int flag ) {
        getDur().writingInt(_userFlags) |= flag;
    }

    void NamespaceDetails::clearUserFlag( int flag ) {
        getDur().writingInt(_userFlags) &= ~flag;
    }


    int NamespaceDetails::getRecordAllocationSize( int minRecordSize ) {
        if ( _paddingFactor == 0 ) {
            warning() << "implicit updgrade of paddingFactor of very old collection" << endl;
            setPaddingFactor(1.0);
        }
        verify( _paddingFactor >= 1 );

        
        if ( isUserFlagSet( Flag_UsePowerOf2Sizes ) ) {
            int x = bucket( minRecordSize );
            x = bucketSizes[x];
            return x;
        }

        return static_cast<int>(minRecordSize * _paddingFactor);
    }

    /* ------------------------------------------------------------------------- */

    /* add a new namespace to the system catalog (<dbname>.system.namespaces).
       options: { capped : ..., size : ... }
    */
    void addNewNamespaceToCatalog(const char *ns, const BSONObj *options = 0) {
        LOG(1) << "New namespace: " << ns << endl;
        if ( strstr(ns, "system.namespaces") ) {
            // system.namespaces holds all the others, so it is not explicitly listed in the catalog.
            // TODO: fix above should not be strstr!
            return;
        }
        
        BSONObjBuilder b;
        b.append("name", ns);
        if ( options )
            b.append("options", *options);
        BSONObj j = b.done();
        char database[256];
        nsToDatabase(ns, database);
        string s = string(database) + ".system.namespaces";
        theDataFileMgr.insert(s.c_str(), j.objdata(), j.objsize(), true);
    }

    void renameNamespace( const char *from, const char *to, bool stayTemp) {
        NamespaceIndex *ni = nsindex( from );
        verify( ni );
        verify( ni->details( from ) );
        verify( ! ni->details( to ) );

        // Our namespace and index details will move to a different
        // memory location.  The only references to namespace and
        // index details across commands are in cursors and nsd
        // transient (including query cache) so clear these.
        ClientCursor::invalidate( from );
        NamespaceDetailsTransient::eraseForPrefix( from );

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
        verify( Helpers::findOne( s.c_str(), BSON( "name" << from ), oldSpec ) );

        BSONObjBuilder newSpecB;
        BSONObjIterator i( oldSpec.getObjectField( "options" ) );
        while( i.more() ) {
            BSONElement e = i.next();
            if ( strcmp( e.fieldName(), "create" ) != 0 ) {
                if (stayTemp || (strcmp(e.fieldName(), "temp") != 0))
                    newSpecB.append( e );
            }
            else {
                newSpecB << "create" << to;
            }
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
            DiskLoc newIndexSpecLoc = theDataFileMgr.insert( s.c_str(), newIndexSpec.objdata(), newIndexSpec.objsize(), true, false );
            int indexI = details->findIndexByName( oldIndexSpec.getStringField( "name" ) );
            IndexDetails &indexDetails = details->idx(indexI);
            string oldIndexNs = indexDetails.indexNamespace();
            indexDetails.info = newIndexSpecLoc;
            string newIndexNs = indexDetails.indexNamespace();

            renameIndexNamespace( oldIndexNs.c_str(), newIndexNs.c_str() );
            deleteObjects( s.c_str(), oldIndexSpec.getOwned(), true, false, true );
        }
    }

    bool legalClientSystemNS( const string& ns , bool write ) {
        if( ns == "local.system.replset" ) return true;

        if ( ns.find( ".system.users" ) != string::npos )
            return true;

        if ( ns.find( ".system.js" ) != string::npos ) {
            if ( write )
                Scope::storedFuncMod();
            return true;
        }

        return false;
    }

} // namespace mongo
