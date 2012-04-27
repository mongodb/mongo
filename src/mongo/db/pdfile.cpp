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

#include "pch.h"
#include "pdfile.h"
#include "db.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"
#include "../util/file_allocator.h"
#include "../util/processinfo.h"
#include "../util/file.h"
#include "btree.h"
#include "btreebuilder.h"
#include <algorithm>
#include <list>
#include "repl.h"
#include "dbhelpers.h"
#include "namespace-inl.h"
#include "extsort.h"
#include "curop-inl.h"
#include "background.h"
#include "compact.h"
#include "ops/delete.h"
#include "instance.h"
#include "replutil.h"
#include "memconcept.h"
#include "mongo/db/lasterror.h"

#include <boost/filesystem/operations.hpp>

namespace mongo {

    BOOST_STATIC_ASSERT( sizeof(Extent)-4 == 48+128 );
    BOOST_STATIC_ASSERT( sizeof(DataFileHeader)-4 == 8192 );

    void printMemInfo( const char * where ) {
        cout << "mem info: ";
        if ( where )
            cout << where << " ";
        ProcessInfo pi;
        if ( ! pi.supported() ) {
            cout << " not supported" << endl;
            return;
        }

        cout << "vsize: " << pi.getVirtualMemorySize() << " resident: " << pi.getResidentSize() << " mapped: " << ( MemoryMappedFile::totalMappedLength() / ( 1024 * 1024 ) ) << endl;
    }

    bool isValidNS( const StringData& ns ) {
        // TODO: should check for invalid characters

        const char * x = strchr( ns.data() , '.' );
        if ( ! x )
            return false;

        x++;
        return *x > 0;
    }

    // TODO SERVER-4328
    bool inDBRepair = false;
    struct doingRepair {
        doingRepair() {
            verify( ! inDBRepair );
            inDBRepair = true;
        }
        ~doingRepair() {
            inDBRepair = false;
        }
    };

    SimpleMutex BackgroundOperation::m("bg");
    map<string, unsigned> BackgroundOperation::dbsInProg;
    set<string> BackgroundOperation::nsInProg;

    bool BackgroundOperation::inProgForDb(const char *db) {
        SimpleMutex::scoped_lock lk(m);
        return dbsInProg[db] != 0;
    }

    bool BackgroundOperation::inProgForNs(const char *ns) {
        SimpleMutex::scoped_lock lk(m);
        return nsInProg.count(ns) != 0;
    }

    void BackgroundOperation::assertNoBgOpInProgForDb(const char *db) {
        uassert(12586, "cannot perform operation: a background operation is currently running for this database",
                !inProgForDb(db));
    }

    void BackgroundOperation::assertNoBgOpInProgForNs(const char *ns) {
        uassert(12587, "cannot perform operation: a background operation is currently running for this collection",
                !inProgForNs(ns));
    }

    BackgroundOperation::BackgroundOperation(const char *ns) : _ns(ns) {
        SimpleMutex::scoped_lock lk(m);
        dbsInProg[_ns.db]++;
        verify( nsInProg.count(_ns.ns()) == 0 );
        nsInProg.insert(_ns.ns());
    }

    BackgroundOperation::~BackgroundOperation() {
        SimpleMutex::scoped_lock lk(m);
        dbsInProg[_ns.db]--;
        nsInProg.erase(_ns.ns());
    }

    void BackgroundOperation::dump(stringstream& ss) {
        SimpleMutex::scoped_lock lk(m);
        if( nsInProg.size() ) {
            ss << "\n<b>Background Jobs in Progress</b>\n";
            for( set<string>::iterator i = nsInProg.begin(); i != nsInProg.end(); i++ )
                ss << "  " << *i << '\n';
        }
        for( map<string,unsigned>::iterator i = dbsInProg.begin(); i != dbsInProg.end(); i++ ) {
            if( i->second )
                ss << "database " << i->first << ": " << i->second << '\n';
        }
    }

    /* ----------------------------------------- */
#ifdef _WIN32
    string dbpath = "\\data\\db\\";
#else
    string dbpath = "/data/db/";
#endif
    const char FREELIST_NS[] = ".$freelist";
    bool directoryperdb = false;
    string repairpath;
    string pidfilepath;

    DataFileMgr theDataFileMgr;
    DatabaseHolder _dbHolder;
    int MAGIC = 0x1000;

    DatabaseHolder& dbHolderUnchecked() {
        return _dbHolder;
    }

    void addNewNamespaceToCatalog(const char *ns, const BSONObj *options = 0);
    void ensureIdIndexForNewNs(const char *ns) {
        if ( ( strstr( ns, ".system." ) == 0 || legalClientSystemNS( ns , false ) ) &&
                strstr( ns, FREELIST_NS ) == 0 ) {
            log( 1 ) << "adding _id index for collection " << ns << endl;
            ensureHaveIdIndex( ns );
        }
    }

    string getDbContext() {
        stringstream ss;
        Client * c = currentClient.get();
        if ( c ) {
            Client::Context * cx = c->getContext();
            if ( cx ) {
                Database *database = cx->db();
                if ( database ) {
                    ss << database->name << ' ';
                    ss << cx->ns() << ' ';
                }
            }
        }
        return ss.str();
    }

    /*---------------------------------------------------------------------*/

    // inheritable class to implement an operation that may be applied to all
    // files in a database using _applyOpToDataFiles()
    class FileOp {
    public:
        virtual ~FileOp() {}
        // Return true if file exists and operation successful
        virtual bool apply( const boost::filesystem::path &p ) = 0;
        virtual const char * op() const = 0;
    };

    void _applyOpToDataFiles( const char *database, FileOp &fo, bool afterAllocator = false, const string& path = dbpath );

    void _deleteDataFiles(const char *database) {
        if ( directoryperdb ) {
            FileAllocator::get()->waitUntilFinished();
            MONGO_ASSERT_ON_EXCEPTION_WITH_MSG( boost::filesystem::remove_all( boost::filesystem::path( dbpath ) / database ), "delete data files with a directoryperdb" );
            return;
        }
        class : public FileOp {
            virtual bool apply( const boost::filesystem::path &p ) {
                return boost::filesystem::remove( p );
            }
            virtual const char * op() const {
                return "remove";
            }
        } deleter;
        _applyOpToDataFiles( database, deleter, true );
    }

    int Extent::initialSize(int len) {
        long long sz = len * 16;
        if ( len < 1000 ) sz = len * 64;
        if ( sz > 1000000000 )
            sz = 1000000000;
        int z = ((int)sz) & 0xffffff00;
        verify( z > len );
        return z;
    }

    void checkConfigNS(const char *ns) {
        if ( cmdLine.configsvr && 
             !( str::startsWith( ns, "config." ) || str::startsWith( ns, "admin." ) ) ) { 
            uasserted(14037, "can't create user databases on a --configsvr instance");
        }
    }

    bool _userCreateNS(const char *ns, const BSONObj& options, string& err, bool *deferIdIndex) {
        log(1) << "create collection " << ns << ' ' << options << endl;

        if ( nsdetails(ns) ) {
            err = "collection already exists";
            return false;
        }

        checkConfigNS(ns);

        long long size = Extent::initialSize(128);
        {
            BSONElement e = options.getField("size");
            if ( e.isNumber() ) {
                size = e.numberLong();
                size += 256;
                size &= 0xffffffffffffff00LL;
            }
        }

        uassert( 10083 , "create collection invalid size spec", size > 0 );

        bool newCapped = false;
        long long mx = 0;
        if( options["capped"].trueValue() ) {
            newCapped = true;
            BSONElement e = options.getField("max");
            if ( e.isNumber() ) {
                mx = e.numberLong();
            }
        }

        // $nExtents just for debug/testing.
        BSONElement e = options.getField( "$nExtents" );
        Database *database = cc().database();
        if ( e.type() == Array ) {
            // We create one extent per array entry, with size specified
            // by the array value.
            BSONObjIterator i( e.embeddedObject() );
            while( i.more() ) {
                BSONElement e = i.next();
                int size = int( e.number() );
                verify( size <= 0x7fffffff );
                // $nExtents is just for testing - always allocate new extents
                // rather than reuse existing extents so we have some predictibility
                // in the extent size used by our tests
                database->suitableFile( ns, (int) size, false, false )->createExtent( ns, (int) size, newCapped );
            }
        }
        else if ( int( e.number() ) > 0 ) {
            // We create '$nExtents' extents, each of size 'size'.
            int nExtents = int( e.number() );
            verify( size <= 0x7fffffff );
            for ( int i = 0; i < nExtents; ++i ) {
                verify( size <= 0x7fffffff );
                // $nExtents is just for testing - always allocate new extents
                // rather than reuse existing extents so we have some predictibility
                // in the extent size used by our tests
                database->suitableFile( ns, (int) size, false, false )->createExtent( ns, (int) size, newCapped );
            }
        }
        else {
            // This is the non test case, where we don't have a $nExtents spec.
            while ( size > 0 ) {
                const int max = Extent::maxSize();
                const int min = Extent::minSize();
                int desiredExtentSize = static_cast<int> (size > max ? max : size);
                desiredExtentSize = static_cast<int> (desiredExtentSize < min ? min : desiredExtentSize);

                desiredExtentSize &= 0xffffff00;
                Extent *e = database->allocExtent( ns, desiredExtentSize, newCapped, true );
                size -= e->length;
            }
        }

        NamespaceDetails *d = nsdetails(ns);
        verify(d);

        bool ensure = false;
        if ( options.getField( "autoIndexId" ).type() ) {
            if ( options["autoIndexId"].trueValue() ) {
                ensure = true;
            }
        }
        else {
            if ( !newCapped ) {
                ensure=true;
            }
        }
        if( ensure ) {
            if( deferIdIndex )
                *deferIdIndex = true;
            else
                ensureIdIndexForNewNs( ns );
        }
        
        if ( mx > 0 )
            d->setMaxCappedDocs( mx );

        bool isFreeList = strstr(ns, FREELIST_NS) != 0;
        if( !isFreeList )
            addNewNamespaceToCatalog(ns, options.isEmpty() ? 0 : &options);

        return true;
    }

    /** { ..., capped: true, size: ..., max: ... }
        @param deferIdIndex - if not not, defers id index creation.  sets the bool value to true if we wanted to create the id index.
        @return true if successful
    */
    bool userCreateNS(const char *ns, BSONObj options, string& err, bool logForReplication, bool *deferIdIndex) {
        const char *coll = strchr( ns, '.' ) + 1;
        massert( 10356 ,  str::stream() << "invalid ns: " << ns , NamespaceString::validCollectionName(ns));
        char cl[ 256 ];
        nsToDatabase( ns, cl );
        bool ok = _userCreateNS(ns, options, err, deferIdIndex);
        if ( logForReplication && ok ) {
            if ( options.getField( "create" ).eoo() ) {
                BSONObjBuilder b;
                b << "create" << coll;
                b.appendElements( options );
                options = b.obj();
            }
            string logNs = string( cl ) + ".$cmd";
            logOp("c", logNs.c_str(), options);
        }
        return ok;
    }

    /*---------------------------------------------------------------------*/

    int MongoDataFile::maxSize() {
        if ( sizeof( int* ) == 4 ) {
            return 512 * 1024 * 1024;
        }
        else if ( cmdLine.smallfiles ) {
            return 0x7ff00000 >> 2;
        }
        else {
            return 0x7ff00000;
        }
    }

    NOINLINE_DECL void MongoDataFile::badOfs2(int ofs) const {
        stringstream ss;
        ss << "bad offset:" << ofs << " accessing file: " << mmf.filename() << " - consider repairing database";
        uasserted(13441, ss.str());
    }

    NOINLINE_DECL void MongoDataFile::badOfs(int ofs) const {
        stringstream ss;
        ss << "bad offset:" << ofs << " accessing file: " << mmf.filename() << " - consider repairing database";
        uasserted(13440, ss.str());
    }

    int MongoDataFile::defaultSize( const char *filename ) const {
        int size;
        if ( fileNo <= 4 )
            size = (64*1024*1024) << fileNo;
        else
            size = 0x7ff00000;
        if ( cmdLine.smallfiles ) {
            size = size >> 2;
        }
        return size;
    }

    static void check(void *_mb) { 
        if( sizeof(char *) == 4 )
            uassert( 10084 , "can't map file memory - mongo requires 64 bit build for larger datasets", _mb != 0);
        else
            uassert( 10085 , "can't map file memory", _mb != 0);
    }

    /** @return true if found and opened. if uninitialized (prealloc only) does not open. */
    bool MongoDataFile::openExisting( const char *filename ) {
        verify( _mb == 0 );
        if( !boost::filesystem::exists(filename) )
            return false;
        if( !mmf.open(filename,false) ) {
            dlog(2) << "info couldn't open " << filename << " probably end of datafile list" << endl;
            return false;
        }
        _mb = mmf.getView(); verify(_mb);
        unsigned long long sz = mmf.length();
        verify( sz <= 0x7fffffff );
        verify( sz % 4096 == 0 );
        if( sz < 64*1024*1024 && !cmdLine.smallfiles ) { 
            if( sz >= 16*1024*1024 && sz % (1024*1024) == 0 ) { 
                log() << "info openExisting file size " << sz << " but cmdLine.smallfiles=false" << endl;
            }
            else {
                log() << "openExisting size " << sz << " less then minimum file size expectation " << filename << endl;
                verify(false);
            }
        }
        check(_mb);
        if( header()->uninitialized() )
            return false;
        return true;
    }

    void MongoDataFile::open( const char *filename, int minSize, bool preallocateOnly ) {
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

        verify( size >= 64*1024*1024 || cmdLine.smallfiles );
        verify( size % 4096 == 0 );

        if ( preallocateOnly ) {
            if ( cmdLine.prealloc ) {
                FileAllocator::get()->requestAllocation( filename, size );
            }
            return;
        }

        {
            verify( _mb == 0 );
            unsigned long long sz = size;
            if( mmf.create(filename, sz, false) )
                _mb = mmf.getView();
            verify( sz <= 0x7fffffff );
            size = (int) sz;
        }
        check(_mb);
        header()->init(fileNo, size, filename);
    }

    void MongoDataFile::flush( bool sync ) {
        mmf.flush( sync );
    }

    void addNewExtentToNamespace(const char *ns, Extent *e, DiskLoc eloc, DiskLoc emptyLoc, bool capped) {
        NamespaceIndex *ni = nsindex(ns);
        NamespaceDetails *details = ni->details(ns);
        if ( details ) {
            verify( !details->lastExtent.isNull() );
            verify( !details->firstExtent.isNull() );
            getDur().writingDiskLoc(e->xprev) = details->lastExtent;
            getDur().writingDiskLoc(details->lastExtent.ext()->xnext) = eloc;
            verify( !eloc.isNull() );
            getDur().writingDiskLoc(details->lastExtent) = eloc;
        }
        else {
            ni->add_ns(ns, eloc, capped);
            details = ni->details(ns);
        }

        {
            NamespaceDetails *dw = details->writingWithoutExtra();
            dw->lastExtentSize = e->length;
        }
        details->addDeletedRec(emptyLoc.drec(), emptyLoc);
    }

    Extent* MongoDataFile::createExtent(const char *ns, int approxSize, bool newCapped, int loops) {
        verify( approxSize <= Extent::maxSize() );
        {
            // make sizes align with VM page size
            int newSize = (approxSize + 0xfff) & 0xfffff000;
            verify( newSize >= 0 );
            if( newSize < Extent::maxSize() )
                approxSize = newSize;
        }
        massert( 10357 ,  "shutdown in progress", ! inShutdown() );
        massert( 10358 ,  "bad new extent size", approxSize >= Extent::minSize() && approxSize <= Extent::maxSize() );
        massert( 10359 ,  "header==0 on new extent: 32 bit mmap space exceeded?", header() ); // null if file open failed
        int ExtentSize = min( header()->unusedLength + 0, approxSize );
        DiskLoc loc;
        if ( ExtentSize < Extent::minSize() ) {
            /* note there could be a lot of looping here is db just started and
               no files are open yet.  we might want to do something about that. */
            if ( loops > 8 ) {
                verify( loops < 10000 );
                out() << "warning: loops=" << loops << " fileno:" << fileNo << ' ' << ns << '\n';
            }
            log() << "newExtent: " << ns << " file " << fileNo << " full, adding a new file\n";
            return cc().database()->addAFile( 0, true )->createExtent(ns, approxSize, newCapped, loops+1);
        }
        int offset = header()->unused.getOfs();

        DataFileHeader *h = header();
        h->unused.writing().set( fileNo, offset + ExtentSize );
        getDur().writingInt(h->unusedLength) = h->unusedLength - ExtentSize;
        loc.set(fileNo, offset);
        Extent *e = _getExtent(loc);
        DiskLoc emptyLoc = getDur().writing(e)->init(ns, ExtentSize, fileNo, offset, newCapped);

        addNewExtentToNamespace(ns, e, loc, emptyLoc, newCapped);

        DEV tlog(1) << "new extent " << ns << " size: 0x" << hex << ExtentSize << " loc: 0x" << hex << offset
                    << " emptyLoc:" << hex << emptyLoc.getOfs() << dec << endl;
        return e;
    }

    Extent* DataFileMgr::allocFromFreeList(const char *ns, int approxSize, bool capped) {
        string s = cc().database()->name + FREELIST_NS;
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
            if( high <= 0 ) {
                // overflowed
                high = max(approxSize, Extent::maxSize());
            }
            if ( high <= Extent::minSize() ) {
                // the minimum extent size is 4097
                high = Extent::minSize() + 1;
            }
            int n = 0;
            Extent *best = 0;
            int bestDiff = 0x7fffffff;
            {
                Timer t;
                DiskLoc L = f->firstExtent;
                while( !L.isNull() ) {
                    Extent * e = L.ext();
                    if( e->length >= low && e->length <= high ) {
                        int diff = abs(e->length - approxSize);
                        if( diff < bestDiff ) {
                            bestDiff = diff;
                            best = e;
                            if( ((double) diff) / approxSize < 0.1 ) { 
                                // close enough
                                break;
                            }
                            if( t.seconds() >= 2 ) { 
                                // have spent lots of time in write lock, and we are in [low,high], so close enough
                                // could come into play if extent freelist is very long
                                break;
                            }
                        }
                        else { 
                            OCCASIONALLY {
                                if( high < 64 * 1024 && t.seconds() >= 2 ) {
                                    // be less picky if it is taking a long time
                                    high = 64 * 1024;
                                }
                            }
                        }
                    }
                    L = e->xnext;
                    ++n;
                }
                if( t.seconds() >= 10 ) {
                    log() << "warning: slow scan in allocFromFreeList (in write lock)" << endl;
                }
            }

            if( n > 128 ) log( n < 512 ) << "warning: newExtent " << n << " scanned\n";

            if( best ) {
                Extent *e = best;
                // remove from the free list
                if( !e->xprev.isNull() )
                    e->xprev.ext()->xnext.writing() = e->xnext;
                if( !e->xnext.isNull() )
                    e->xnext.ext()->xprev.writing() = e->xprev;
                if( f->firstExtent == e->myLoc )
                    f->firstExtent.writing() = e->xnext;
                if( f->lastExtent == e->myLoc )
                    f->lastExtent.writing() = e->xprev;

                // use it
                OCCASIONALLY if( n > 512 ) log() << "warning: newExtent " << n << " scanned\n";
                DiskLoc emptyLoc = e->reuse(ns, capped);
                addNewExtentToNamespace(ns, e, e->myLoc, emptyLoc, capped);
                return e;
            }
        }

        return 0;
        //        return createExtent(ns, approxSize, capped);
    }

    /*---------------------------------------------------------------------*/

    void Extent::markEmpty() { 
        xnext.Null();
        xprev.Null();
        firstRecord.Null();
        lastRecord.Null();
    }

    DiskLoc Extent::reuse(const char *nsname, bool capped) {
        return getDur().writing(this)->_reuse(nsname, capped);
    }

    void getEmptyLoc(const char *ns, const DiskLoc extentLoc, int extentLength, bool capped, /*out*/DiskLoc& emptyLoc, /*out*/int& delRecLength) { 
        emptyLoc = extentLoc;
        emptyLoc.inc( Extent::HeaderSize() );
        delRecLength = extentLength - Extent::HeaderSize();
        if( delRecLength >= 32*1024 && str::contains(ns, '$') && !capped ) { 
            // probably an index. so skip forward to keep its records page aligned 
            little<int>& ofs = emptyLoc.GETOFS();
            int newOfs = (ofs + 0xfff) & ~0xfff; 
            delRecLength -= (newOfs-ofs);
            dassert( delRecLength > 0 );
            ofs = newOfs;
        }
    }

    DiskLoc Extent::_reuse(const char *nsname, bool capped) {
        LOG(3) << "reset extent was:" << nsDiagnostic.toString() << " now:" << nsname << '\n';
        massert( 10360 ,  "Extent::reset bad magic value", magic == 0x41424344 );
        nsDiagnostic = nsname;
        markEmpty();

        DiskLoc emptyLoc;
        int delRecLength;
        getEmptyLoc(nsname, myLoc, length, capped, emptyLoc, delRecLength);

        // todo: some dup code here and below in Extent::init
        DeletedRecord *empty = DataFileMgr::makeDeletedRecord(emptyLoc, delRecLength);
        empty = getDur().writing(empty);
        empty->lengthWithHeaders() = delRecLength;
        empty->extentOfs() = myLoc.getOfs();
        empty->nextDeleted().Null();

        return emptyLoc;
    }

    /* assumes already zeroed -- insufficient for block 'reuse' perhaps */
    DiskLoc Extent::init(const char *nsname, int _length, int _fileNo, int _offset, bool capped) {
        magic = 0x41424344;
        myLoc.set(_fileNo, _offset);
        xnext.Null();
        xprev.Null();
        nsDiagnostic = nsname;
        length = _length;
        firstRecord.Null();
        lastRecord.Null();

        DiskLoc emptyLoc;
        int delRecLength;
        getEmptyLoc(nsname, myLoc, _length, capped, emptyLoc, delRecLength);

        DeletedRecord *empty = getDur().writing( DataFileMgr::makeDeletedRecord(emptyLoc, delRecLength) );
        empty->lengthWithHeaders() = delRecLength;
        empty->extentOfs() = myLoc.getOfs();

        return emptyLoc;
    }

    /*
      Record* Extent::newRecord(int len) {
      if( firstEmptyRegion.isNull() )8
      return 0;

      verify(len > 0);
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
      verify(getRecord(lastRecord)->next.lastInExtent()); // it was the last one
      getRecord(lastRecord)->next.set(newRecordLoc); // until now
      r->prev.set(lastRecord);
      }
      else {
      r->prev.markAsFirstOrLastInExtent(this); // we are the first in the extent
      verify( firstRecord.isNull() );
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

    int Extent::maxSize() {
        int maxExtentSize = 0x7ff00000;
        if ( cmdLine.smallfiles ) {
            maxExtentSize >>= 2;
        }
        return maxExtentSize;
    }

    /*---------------------------------------------------------------------*/

    shared_ptr<Cursor> DataFileMgr::findAll(const char *ns, const DiskLoc &startLoc) {
        NamespaceDetails * d = nsdetails( ns );
        if ( ! d )
            return shared_ptr<Cursor>(new BasicCursor(DiskLoc()));

        DiskLoc loc = d->firstExtent;
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
            d->dumpDeleted(&extents);
        }

        if ( d->isCapped() )
            return shared_ptr<Cursor>( new ForwardCappedCursor( d , startLoc ) );

        if ( !startLoc.isNull() )
            return shared_ptr<Cursor>(new BasicCursor( startLoc ));

        while ( e->firstRecord.isNull() && !e->xnext.isNull() ) {
            /* todo: if extent is empty, free it for reuse elsewhere.
               that is a bit complicated have to clean up the freelists.
            */
            RARELY out() << "info DFM::findAll(): extent " << loc.toString() << " was empty, skipping ahead. ns:" << ns << endl;
            // find a nonempty extent
            // it might be nice to free the whole extent here!  but have to clean up free recs then.
            e = e->getNextExtent();
        }
        return shared_ptr<Cursor>(new BasicCursor( e->firstRecord ));
    }

    /* get a table scan cursor, but can be forward or reverse direction.
       order.$natural - if set, > 0 means forward (asc), < 0 backward (desc).
    */
    shared_ptr<Cursor> findTableScan(const char *ns, const BSONObj& order, const DiskLoc &startLoc) {
        BSONElement el = order.getField("$natural"); // e.g., { $natural : -1 }

        if ( el.number() >= 0 )
            return DataFileMgr::findAll(ns, startLoc);

        // "reverse natural order"
        NamespaceDetails *d = nsdetails(ns);

        if ( !d )
            return shared_ptr<Cursor>(new BasicCursor(DiskLoc()));

        if ( !d->isCapped() ) {
            if ( !startLoc.isNull() )
                return shared_ptr<Cursor>(new ReverseCursor( startLoc ));
            Extent *e = d->lastExtent.ext();
            while ( e->lastRecord.isNull() && !e->xprev.isNull() ) {
                OCCASIONALLY out() << "  findTableScan: extent empty, skipping ahead" << endl;
                e = e->getPrevExtent();
            }
            return shared_ptr<Cursor>(new ReverseCursor( e->lastRecord ));
        }
        else {
            return shared_ptr<Cursor>( new ReverseCappedCursor( d, startLoc ) );
        }
    }

    void printFreeList() {
        string s = cc().database()->name + FREELIST_NS;
        log() << "dump freelist " << s << endl;
        NamespaceDetails *freeExtents = nsdetails(s.c_str());
        if( freeExtents == 0 ) {
            log() << "  freeExtents==0" << endl;
            return;
        }
        DiskLoc a = freeExtents->firstExtent;
        while( !a.isNull() ) {
            Extent *e = a.ext();
            log() << "  extent " << a.toString() << " len:" << e->length << " prev:" << e->xprev.toString() << endl;
            a = e->xnext;
        }

        log() << "end freelist" << endl;
    }

    /** free a list of extents that are no longer in use.  this is a double linked list of extents 
        (could be just one in the list)
    */
    void freeExtents(DiskLoc firstExt, DiskLoc lastExt) {
        {
            verify( !firstExt.isNull() && !lastExt.isNull() );
            Extent *f = firstExt.ext();
            Extent *l = lastExt.ext();
            verify( f->xprev.isNull() );
            verify( l->xnext.isNull() );
            verify( f==l || !f->xnext.isNull() );
            verify( f==l || !l->xprev.isNull() );
        }

        string s = cc().database()->name + FREELIST_NS;
        NamespaceDetails *freeExtents = nsdetails(s.c_str());
        if( freeExtents == 0 ) {
            string err;
            _userCreateNS(s.c_str(), BSONObj(), err, 0); // todo: this actually allocates an extent, which is bad!
            freeExtents = nsdetails(s.c_str());
            massert( 10361 , "can't create .$freelist", freeExtents);
        }
        if( freeExtents->firstExtent.isNull() ) {
            freeExtents->firstExtent.writing() = firstExt;
            freeExtents->lastExtent.writing() = lastExt;
        }
        else {
            DiskLoc a = freeExtents->firstExtent;
            verify( a.ext()->xprev.isNull() );
            getDur().writingDiskLoc( a.ext()->xprev ) = lastExt;
            getDur().writingDiskLoc( lastExt.ext()->xnext ) = a;
            getDur().writingDiskLoc( freeExtents->firstExtent ) = firstExt;
        }

        //printFreeList();
    }

    /* drop a collection/namespace */
    void dropNS(const string& nsToDrop) {
        NamespaceDetails* d = nsdetails(nsToDrop.c_str());
        uassert( 10086 ,  (string)"ns not found: " + nsToDrop , d );

        BackgroundOperation::assertNoBgOpInProgForNs(nsToDrop.c_str());

        NamespaceString s(nsToDrop);
        verify( s.db == cc().database()->name );
        if( s.isSystem() ) {
            if( s.coll == "system.profile" )
                uassert( 10087 ,  "turn off profiling before dropping system.profile collection", cc().database()->profile == 0 );
            else
                uasserted( 12502, "can't drop system ns" );
        }

        {
            // remove from the system catalog
            BSONObj cond = BSON( "name" << nsToDrop );   // { name: "colltodropname" }
            string system_namespaces = cc().database()->name + ".system.namespaces";
            /*int n = */ deleteObjects(system_namespaces.c_str(), cond, false, false, true);
            // no check of return code as this ns won't exist for some of the new storage engines
        }

        // free extents
        if( !d->firstExtent.isNull() ) {
            freeExtents(d->firstExtent, d->lastExtent);
            getDur().writingDiskLoc( d->firstExtent ).setInvalid();
            getDur().writingDiskLoc( d->lastExtent ).setInvalid();
        }

        // remove from the catalog hashtable
        cc().database()->namespaceIndex.kill_ns(nsToDrop.c_str());
    }

    void dropCollection( const string &name, string &errmsg, BSONObjBuilder &result ) {
        log(1) << "dropCollection: " << name << endl;
        NamespaceDetails *d = nsdetails(name.c_str());
        if( d == 0 )
            return;

        BackgroundOperation::assertNoBgOpInProgForNs(name.c_str());

        if ( d->nIndexes != 0 ) {
            try {
                verify( dropIndexes(d, name.c_str(), "*", errmsg, result, true) );
            }
            catch( DBException& e ) {
                stringstream ss;
                ss << "drop: dropIndexes for collection failed - consider trying repair ";
                ss << " cause: " << e.what();
                uasserted(12503,ss.str());
            }
            verify( d->nIndexes == 0 );
        }
        log(1) << "\t dropIndexes done" << endl;
        result.append("ns", name.c_str());
        ClientCursor::invalidate(name.c_str());
        Top::global.collectionDropped( name );
        NamespaceDetailsTransient::eraseForPrefix( name.c_str() );
        dropNS(name);
    }

    /* unindex all keys in index for this record. */
    static void _unindexRecord(IndexDetails& id, BSONObj& obj, const DiskLoc& dl, bool logMissing = true) {
        BSONObjSet keys;
        id.getKeysFromObject(obj, keys);
        IndexInterface& ii = id.idxInterface();
        for ( BSONObjSet::iterator i=keys.begin(); i != keys.end(); i++ ) {
            BSONObj j = *i;

            bool ok = false;
            try {
                ok = ii.unindex(id.head, id, j, dl);
            }
            catch (AssertionException& e) {
                problem() << "Assertion failure: _unindex failed " << id.indexNamespace() << endl;
                out() << "Assertion failure: _unindex failed: " << e.what() << '\n';
                out() << "  obj:" << obj.toString() << '\n';
                out() << "  key:" << j.toString() << '\n';
                out() << "  dl:" << dl.toString() << endl;
                sayDbContext();
            }

            if ( !ok && logMissing ) {
                log() << "unindex failed (key too big?) " << id.indexNamespace() << " key: " << j << " " << obj["_id"] << endl;
            }
        }
    }
//zzz
    /* unindex all keys in all indexes for this record. */
    static void unindexRecord(NamespaceDetails *d, Record *todelete, const DiskLoc& dl, bool noWarn = false) {
        BSONObj obj(todelete);
        int n = d->nIndexes;
        for ( int i = 0; i < n; i++ )
            _unindexRecord(d->idx(i), obj, dl, !noWarn);
        if( d->indexBuildInProgress ) { // background index
            // always pass nowarn here, as this one may be missing for valid reasons as we are concurrently building it
            _unindexRecord(d->idx(n), obj, dl, false);
        }
    }

    /* deletes a record, just the pdfile portion -- no index cleanup, no cursor cleanup, etc.
       caller must check if capped
    */
    void DataFileMgr::_deleteRecord(NamespaceDetails *d, const char *ns, Record *todelete, const DiskLoc& dl) {
        /* remove ourself from the record next/prev chain */
        {
            if ( todelete->prevOfs() != DiskLoc::NullOfs )
                getDur().writingInt( todelete->getPrev(dl).rec()->nextOfs() ) = todelete->nextOfs();
            if ( todelete->nextOfs() != DiskLoc::NullOfs )
                getDur().writingInt( todelete->getNext(dl).rec()->prevOfs() ) = todelete->prevOfs();
        }

        /* remove ourself from extent pointers */
        {
            Extent *e = getDur().writing( todelete->myExtent(dl) );
            if ( e->firstRecord == dl ) {
                if ( todelete->nextOfs() == DiskLoc::NullOfs )
                    e->firstRecord.Null();
                else
                    e->firstRecord.set(dl.a(), todelete->nextOfs() );
            }
            if ( e->lastRecord == dl ) {
                if ( todelete->prevOfs() == DiskLoc::NullOfs )
                    e->lastRecord.Null();
                else
                    e->lastRecord.set(dl.a(), todelete->prevOfs() );
            }
        }

        /* add to the free list */
        {
            {
                NamespaceDetails::Stats *s = getDur().writing(&d->stats);
                s->datasize -= todelete->netLength();
                s->nrecords--;
            }

            if ( strstr(ns, ".system.indexes") ) {
                /* temp: if in system.indexes, don't reuse, and zero out: we want to be
                   careful until validated more, as IndexDetails has pointers
                   to this disk location.  so an incorrectly done remove would cause
                   a lot of problems.
                */
                memset(getDur().writingPtr(todelete, todelete->lengthWithHeaders() ), 0, todelete->lengthWithHeaders() );
            }
            else {
                DEV {
                    little<unsigned long long> *p = &little<unsigned long long >::ref( todelete->data() );
                    *getDur().writing(p) = 0;
                    //DEV memset(todelete->data, 0, todelete->netLength()); // attempt to notice invalid reuse.
                }
                d->addDeletedRec((DeletedRecord*)todelete, dl);
            }
        }
    }

    void DataFileMgr::deleteRecord(const char *ns, Record *todelete, const DiskLoc& dl, bool cappedOK, bool noWarn, bool doLog ) {
        dassert( todelete == dl.rec() );

        NamespaceDetails* d = nsdetails(ns);
        if ( d->isCapped() && !cappedOK ) {
            out() << "failing remove on a capped ns " << ns << endl;
            uassert( 10089 ,  "can't remove from a capped collection" , 0 );
            return;
        }
        
        BSONObj toDelete;
        if ( doLog ) {
            BSONElement e = dl.obj()["_id"];
            if ( e.type() ) {
                toDelete = e.wrap();
            }
        }

        /* check if any cursors point to us.  if so, advance them. */
        ClientCursor::aboutToDelete(dl);

        unindexRecord(d, todelete, dl, noWarn);

        _deleteRecord(d, ns, todelete, dl);
        NamespaceDetailsTransient::get( ns ).notifyOfWriteOp();

        if ( ! toDelete.isEmpty() ) {
            logOp( "d" , ns , toDelete );
        }
    }


    /** Note: if the object shrinks a lot, we don't free up space, we leave extra at end of the record.
     */
    const DiskLoc DataFileMgr::updateRecord(
        const char *ns,
        NamespaceDetails *d,
        NamespaceDetailsTransient *nsdt,
        Record *toupdate, const DiskLoc& dl,
        const char *_buf, int _len, OpDebug& debug,  bool god) {

        dassert( toupdate == dl.rec() );

        BSONObj objOld(toupdate);
        BSONObj objNew(_buf);
        DEV verify( objNew.objsize() == _len );
        DEV verify( objNew.objdata() == _buf );

        if( !objNew.hasElement("_id") && objOld.hasElement("_id") ) {
            /* add back the old _id value if the update removes it.  Note this implementation is slow
               (copies entire object multiple times), but this shouldn't happen often, so going for simple
               code, not speed.
            */
            BSONObjBuilder b;
            BSONElement e;
            verify( objOld.getObjectID(e) );
            b.append(e); // put _id first, for best performance
            b.appendElements(objNew);
            objNew = b.obj();
        }

        /* duplicate key check. we descend the btree twice - once for this check, and once for the actual inserts, further
           below.  that is suboptimal, but it's pretty complicated to do it the other way without rollbacks...
        */
        vector<IndexChanges> changes;
        bool changedId = false;
        getIndexChanges(changes, ns, *d, objNew, objOld, changedId);
        uassert( 13596 , str::stream() << "cannot change _id of a document old:" << objOld << " new:" << objNew , ! changedId );
        dupCheck(changes, *d, dl);

        if ( toupdate->netLength() < objNew.objsize() ) {
            // doesn't fit.  reallocate -----------------------------------------------------
            uassert( 10003 , "failing update: objects in a capped ns cannot grow", !(d && d->isCapped()));
            d->paddingTooSmall();
            deleteRecord(ns, toupdate, dl);
            DiskLoc res = insert(ns, objNew.objdata(), objNew.objsize(), god);

            if (debug.nmoved == -1) // default of -1 rather than 0
                debug.nmoved = 1;
            else
                debug.nmoved += 1;

            return res;
        }

        nsdt->notifyOfWriteOp();
        d->paddingFits();

        /* have any index keys changed? */
        {
            int keyUpdates = 0;
            int z = d->nIndexesBeingBuilt();
            for ( int x = 0; x < z; x++ ) {
                IndexDetails& idx = d->idx(x);
                IndexInterface& ii = idx.idxInterface();
                for ( unsigned i = 0; i < changes[x].removed.size(); i++ ) {
                    try {
                        bool found = ii.unindex(idx.head, idx, *changes[x].removed[i], dl);
                        if ( ! found ) {
                            RARELY warning() << "ns: " << ns << " couldn't unindex key: " << *changes[x].removed[i] 
                                             << " for doc: " << objOld["_id"] << endl;
                        }
                    }
                    catch (AssertionException&) {
                        debug.extra << " exception update unindex ";
                        problem() << " caught assertion update unindex " << idx.indexNamespace() << endl;
                    }
                }
                verify( !dl.isNull() );
                BSONObj idxKey = idx.info.obj().getObjectField("key");
                Ordering ordering = Ordering::make(idxKey);
                keyUpdates += changes[x].added.size();
                for ( unsigned i = 0; i < changes[x].added.size(); i++ ) {
                    try {
                        /* we did the dupCheck() above.  so we don't have to worry about it here. */
                        ii.bt_insert(
                            idx.head,
                            dl, *changes[x].added[i], ordering, /*dupsAllowed*/true, idx);
                    }
                    catch (AssertionException& e) {
                        debug.extra << " exception update index ";
                        problem() << " caught assertion update index " << idx.indexNamespace() << " " << e << " " << objNew["_id"] << endl;
                    }
                }
            }
            
            debug.keyUpdates = keyUpdates;
        }

        //  update in place
        int sz = objNew.objsize();
        memcpy(getDur().writingPtr(toupdate->data(), sz), objNew.objdata(), sz);
        return dl;
    }

    int Extent::followupSize(int len, int lastExtentLen) {
        verify( len < Extent::maxSize() );
        int x = initialSize(len);
        // changed from 1.20 to 1.35 in v2.1.x to get to larger extent size faster
        int y = (int) (lastExtentLen < 4000000 ? lastExtentLen * 4.0 : lastExtentLen * 1.35);
        int sz = y > x ? y : x;

        if ( sz < lastExtentLen ) {
            // this means there was an int overflow
            // so we should turn it into maxSize
            sz = Extent::maxSize();
        }
        else if ( sz > Extent::maxSize() ) {
            sz = Extent::maxSize();
        }

        sz = ((int)sz) & 0xffffff00;
        verify( sz > len );

        return sz;
    }

    /* step one of adding keys to index idxNo for a new record
       @return true means done.  false means multikey involved and more work to do
    */
    static void _addKeysToIndexStepOneOfTwo(BSONObjSet & /*out*/keys,
                                            IndexInterface::IndexInserter &inserter,
                                            NamespaceDetails *d,
                                            int idxNo,
                                            BSONObj& obj,
                                            DiskLoc recordLoc) {
        IndexDetails &idx = d->idx(idxNo);
        idx.getKeysFromObject(obj, keys);
        if( keys.empty() )
            return;
        bool dupsAllowed = !idx.unique();
        Ordering ordering = Ordering::make(idx.keyPattern());

        verify( !recordLoc.isNull() );

        try {
            // we can't do the two step method with multi keys as insertion of one key changes the indexes 
            // structure.  however we can do the first key of the set so we go ahead and do that FWIW
            inserter.addInsertionContinuation(
                    idx.idxInterface().beginInsertIntoIndex(
                            idxNo, idx, recordLoc, *keys.begin(), ordering, dupsAllowed));
        }
        catch (AssertionException& e) {
            if( e.getCode() == 10287 && idxNo == d->nIndexes ) {
                DEV log() << "info: caught key already in index on bg indexing (ok)" << endl;
            }
            else {
                throw;
            }
        }
    }

    /** add index keys for a newly inserted record 
        done in two steps/phases to allow potential deferal of write lock portion in the future
    */
    static void indexRecordUsingTwoSteps(const char *ns, NamespaceDetails *d, BSONObj obj,
                                         DiskLoc loc, bool shouldBeUnlocked) {
        vector<int> multi;
        vector<BSONObjSet> multiKeys;

        IndexInterface::IndexInserter inserter;

        // Step 1, read phase.
        int n = d->nIndexesBeingBuilt();
        {
            BSONObjSet keys;
            for ( int i = 0; i < n; i++ ) {
                // this call throws on unique constraint violation.  we haven't done any writes yet so that is fine.
                _addKeysToIndexStepOneOfTwo(/*out*/keys, inserter, d, i, obj, loc);
                if( keys.size() > 1 ) {
                    multi.push_back(i);
                    multiKeys.push_back(BSONObjSet());
                    multiKeys[multiKeys.size()-1].swap(keys);
                }
                keys.clear();
            }
        }

        inserter.finishAllInsertions();  // Step 2, write phase.

        // now finish adding multikeys
        for( unsigned j = 0; j < multi.size(); j++ ) {
            unsigned i = multi[j];
            BSONObjSet& keys = multiKeys[j];
            IndexDetails& idx = d->idx(i);
            IndexInterface& ii = idx.idxInterface();
            Ordering ordering = Ordering::make(idx.keyPattern());
            d->setIndexIsMultikey(ns, i);
            for( BSONObjSet::iterator k = ++keys.begin()/*skip 1*/; k != keys.end(); k++ ) {
                try {
                    ii.bt_insert(idx.head, loc, *k, ordering, !idx.unique(), idx);
                } catch (AssertionException& e) {
                    if( e.getCode() == 10287 && (int) i == d->nIndexes ) {
                        DEV log() << "info: caught key already in index on bg indexing (ok)" << endl;
                    }
                    else {
                        /* roll back previously added index entries
                           note must do self index as it is multikey and could require some cleanup itself
                        */
                        for( int j = 0; j < n; j++ ) {
                            try {
                                _unindexRecord(d->idx(j), obj, loc, false);
                            }
                            catch(...) {
                                log(3) << "unindex fails on rollback after unique key constraint prevented insert\n";
                            }
                        }
                        throw;
                    }
                }
            }
        }
    }

    /* add keys to index idxNo for a new record */
    static void addKeysToIndex(const char *ns, NamespaceDetails *d, int idxNo, BSONObj& obj,
                               DiskLoc recordLoc, bool dupsAllowed) {
        IndexDetails& idx = d->idx(idxNo);
        BSONObjSet keys;
        idx.getKeysFromObject(obj, keys);
        if( keys.empty() ) 
            return;
        BSONObj order = idx.keyPattern();
        IndexInterface& ii = idx.idxInterface();
        Ordering ordering = Ordering::make(order);
        int n = 0;
        for ( BSONObjSet::iterator i=keys.begin(); i != keys.end(); i++ ) {
            if( ++n == 2 ) {
                d->setIndexIsMultikey(ns, idxNo);
            }
            verify( !recordLoc.isNull() );
            try {
                ii.bt_insert(idx.head, recordLoc, *i, ordering, dupsAllowed, idx);
            }
            catch (AssertionException& e) {
                if( e.getCode() == 10287 && idxNo == d->nIndexes ) {
                    DEV log() << "info: caught key already in index on bg indexing (ok)" << endl;
                    continue;
                }
                if( !dupsAllowed ) {
                    // dup key exception, presumably.
                    throw;
                }
                problem() << " caught assertion addKeysToIndex " << idx.indexNamespace() << " " << obj["_id"] << endl;
            }
        }
    }

#if 0    
    void testSorting() {
        BSONObjBuilder b;
        b.appendNull("");
        BSONObj x = b.obj();

        BSONObjExternalSorter sorter(*IndexDetails::iis[1]);

        sorter.add(x, DiskLoc(3,7));
        sorter.add(x, DiskLoc(4,7));
        sorter.add(x, DiskLoc(2,7));
        sorter.add(x, DiskLoc(1,7));
        sorter.add(x, DiskLoc(3,77));

        sorter.sort();

        auto_ptr<BSONObjExternalSorter::Iterator> i = sorter.iterator();
        while( i->more() ) {
            BSONObjExternalSorter::Data d = i->next();
            /*cout << d.second.toString() << endl;
            cout << d.first.objsize() << endl;
            cout<<"SORTER next:" << d.first.toString() << endl;*/
        }
    }
#endif

    SortPhaseOne *precalced = 0;

    template< class V >
    void buildBottomUpPhases2And3(bool dupsAllowed, IndexDetails& idx, BSONObjExternalSorter& sorter, 
        bool dropDups, set<DiskLoc> &dupsToDrop, CurOp * op, SortPhaseOne *phase1, ProgressMeterHolder &pm,
        Timer& t
        )
    {
        BtreeBuilder<V> btBuilder(dupsAllowed, idx);
        BSONObj keyLast;
        auto_ptr<BSONObjExternalSorter::Iterator> i = sorter.iterator();
        verify( pm == op->setMessage( "index: (2/3) btree bottom up" , phase1->nkeys , 10 ) );
        while( i->more() ) {
            RARELY killCurrentOp.checkForInterrupt();
            BSONObjExternalSorter::Data d = i->next();

            try {
                if ( !dupsAllowed && dropDups ) {
                    LastError::Disabled led( lastError.get() );
                    btBuilder.addKey(d.first, d.second);
                }
                else {
                    btBuilder.addKey(d.first, d.second);                    
                }
            }
            catch( AssertionException& e ) {
                if ( dupsAllowed ) {
                    // unknown exception??
                    throw;
                }

                if( e.interrupted() ) {
                    killCurrentOp.checkForInterrupt();
                }

                if ( ! dropDups )
                    throw;

                /* we could queue these on disk, but normally there are very few dups, so instead we
                    keep in ram and have a limit.
                */
                dupsToDrop.insert(d.second);
                uassert( 10092 , "too may dups on index build with dropDups=true", dupsToDrop.size() < 1000000 );
            }
            pm.hit();
        }
        pm.finished();
        op->setMessage( "index: (3/3) btree-middle" );
        log(t.seconds() > 10 ? 0 : 1 ) << "\t done building bottom layer, going to commit" << endl;
        btBuilder.commit();
        if ( btBuilder.getn() != phase1->nkeys && ! dropDups ) {
            warning() << "not all entries were added to the index, probably some keys were too large" << endl;
        }
    }

    // throws DBException
    unsigned long long fastBuildIndex(const char *ns, NamespaceDetails *d, IndexDetails& idx, int idxNo) {
        CurOp * op = cc().curop();

        Timer t;

        tlog(1) << "fastBuildIndex " << ns << " idxNo:" << idxNo << ' ' << idx.info.obj().toString() << endl;

        bool dupsAllowed = !idx.unique();
        bool dropDups = idx.dropDups() || inDBRepair;
        BSONObj order = idx.keyPattern();

        getDur().writingDiskLoc(idx.head).Null();

        if ( logLevel > 1 ) printMemInfo( "before index start" );

        /* get and sort all the keys ----- */
        ProgressMeterHolder pm( op->setMessage( "index: (1/3) external sort" , d->stats.nrecords , 10 ) );
        SortPhaseOne _ours;
        SortPhaseOne *phase1 = precalced;
        if( phase1 == 0 ) {
            phase1 = &_ours;
            SortPhaseOne& p1 = *phase1;
            shared_ptr<Cursor> c = theDataFileMgr.findAll(ns);
            p1.sorter.reset( new BSONObjExternalSorter(idx.idxInterface(), order) );
            p1.sorter->hintNumObjects( d->stats.nrecords );
            const IndexSpec& spec = idx.getSpec();
            while ( c->ok() ) {
                BSONObj o = c->current();
                DiskLoc loc = c->currLoc();
                p1.addKeys(spec, o, loc);
                c->advance();
                pm.hit();
                if ( logLevel > 1 && p1.n % 10000 == 0 ) {
                    printMemInfo( "\t iterating objects" );
                }
            };
        }
        pm.finished();

        BSONObjExternalSorter& sorter = *(phase1->sorter);

        if( phase1->multi )
            d->setIndexIsMultikey(ns, idxNo);

        if ( logLevel > 1 ) printMemInfo( "before final sort" );
        phase1->sorter->sort();
        if ( logLevel > 1 ) printMemInfo( "after final sort" );

        log(t.seconds() > 5 ? 0 : 1) << "\t external sort used : " << sorter.numFiles() << " files " << " in " << t.seconds() << " secs" << endl;

        set<DiskLoc> dupsToDrop;

        /* build index --- */
        if( idx.version() == 0 )
            buildBottomUpPhases2And3<V0>(dupsAllowed, idx, sorter, dropDups, dupsToDrop, op, phase1, pm, t);
        else if( idx.version() == 1 ) 
            buildBottomUpPhases2And3<V1>(dupsAllowed, idx, sorter, dropDups, dupsToDrop, op, phase1, pm, t);
        else
            verify(false);

        if( dropDups ) 
            log() << "\t fastBuildIndex dupsToDrop:" << dupsToDrop.size() << endl;

        for( set<DiskLoc>::iterator i = dupsToDrop.begin(); i != dupsToDrop.end(); i++ ){
            theDataFileMgr.deleteRecord( ns, i->rec(), *i, false /* cappedOk */ , true /* noWarn */ , isMaster( ns ) /* logOp */ );
            getDur().commitIfNeeded();
        }

        return phase1->n;
    }

    class BackgroundIndexBuildJob : public BackgroundOperation {

        unsigned long long addExistingToIndex(const char *ns, NamespaceDetails *d, IndexDetails& idx, int idxNo) {
            bool dupsAllowed = !idx.unique();
            bool dropDups = idx.dropDups();

            ProgressMeter& progress = cc().curop()->setMessage( "bg index build" , d->stats.nrecords );

            unsigned long long n = 0;
            unsigned long long numDropped = 0;
            auto_ptr<ClientCursor> cc;
            {
                shared_ptr<Cursor> c = theDataFileMgr.findAll(ns);
                cc.reset( new ClientCursor(QueryOption_NoCursorTimeout, c, ns) );
            }

            while ( cc->ok() ) {
                BSONObj js = cc->current();
                try {
                    {
                        if ( !dupsAllowed && dropDups ) {
                            LastError::Disabled led( lastError.get() );
                            addKeysToIndex(ns, d, idxNo, js, cc->currLoc(), dupsAllowed);
                        }
                        else {
                            addKeysToIndex(ns, d, idxNo, js, cc->currLoc(), dupsAllowed);
                        }
                    }
                    cc->advance();
                }
                catch( AssertionException& e ) {
                    if( e.interrupted() ) {
                        killCurrentOp.checkForInterrupt();
                    }

                    if ( dropDups ) {
                        DiskLoc toDelete = cc->currLoc();
                        bool ok = cc->advance();
                        ClientCursor::YieldData yieldData;
                        massert( 16093, "after yield cursor deleted" , cc->prepareToYield( yieldData ) );
                        theDataFileMgr.deleteRecord( ns, toDelete.rec(), toDelete, false, true , true );
                        if( !cc->recoverFromYield( yieldData ) ) {
                            cc.release();
                            if( !ok ) {
                                /* we were already at the end. normal. */
                            }
                            else {
                                uasserted(12585, "cursor gone during bg index; dropDups");
                            }
                            break;
                        }
                        numDropped++;
                    }
                    else {
                        log() << "background addExistingToIndex exception " << e.what() << endl;
                        throw;
                    }
                }
                n++;
                progress.hit();

                getDur().commitIfNeeded();

                if ( cc->yieldSometimes( ClientCursor::WillNeed ) ) {
                    progress.setTotalWhileRunning( d->stats.nrecords );
                }
                else {
                    cc.release();
                    uasserted(12584, "cursor gone during bg index");
                    break;
                }
            }
            progress.finished();
            if ( dropDups )
                log() << "\t backgroundIndexBuild dupsToDrop: " << numDropped << endl;
            return n;
        }

        /* we do set a flag in the namespace for quick checking, but this is our authoritative info -
           that way on a crash/restart, we don't think we are still building one. */
        set<NamespaceDetails*> bgJobsInProgress;

        void prep(const char *ns, NamespaceDetails *d) {
            Lock::assertWriteLocked(ns);
            uassert( 13130 , "can't start bg index b/c in recursive lock (db.eval?)" , !Lock::nested() );
            bgJobsInProgress.insert(d);
        }
        void done(const char *ns, NamespaceDetails *d) {
            NamespaceDetailsTransient::get(ns).addedIndex(); // clear query optimizer cache
            Lock::assertWriteLocked(ns);
        }

    public:
        BackgroundIndexBuildJob(const char *ns) : BackgroundOperation(ns) { }

        unsigned long long go(string ns, NamespaceDetails *d, IndexDetails& idx, int idxNo) {
            unsigned long long n = 0;

            prep(ns.c_str(), d);
            verify( idxNo == d->nIndexes );
            try {
                idx.head.writing() = idx.idxInterface().addBucket(idx);
                n = addExistingToIndex(ns.c_str(), d, idx, idxNo);
            }
            catch(...) {
                if( cc().database() && nsdetails(ns.c_str()) == d ) {
                    verify( idxNo == d->nIndexes );
                    done(ns.c_str(), d);
                }
                else {
                    log() << "ERROR: db gone during bg index?" << endl;
                }
                throw;
            }
            verify( idxNo == d->nIndexes );
            done(ns.c_str(), d);
            return n;
        }
    };

    /**
     * For the lifetime of this object, an index build is indicated on the specified
     * namespace and the newest index is marked as absent.  This simplifies
     * the cleanup required on recovery.
     */
    class RecoverableIndexState {
    public:
        RecoverableIndexState( NamespaceDetails *d ) : _d( d ) {
            indexBuildInProgress() = 1;
            nIndexes()--;
        }
        ~RecoverableIndexState() {
            DESTRUCTOR_GUARD (
                nIndexes()++;
                indexBuildInProgress() = 0;
            )
        }
    private:
        little<int> &nIndexes() { return getDur().writingInt( _d->nIndexes ); }
        little<int> &indexBuildInProgress() { return getDur().writingInt( _d->indexBuildInProgress ); }
        NamespaceDetails *_d;
    };

    // throws DBException
    static void buildAnIndex(string ns, NamespaceDetails *d, IndexDetails& idx, int idxNo, bool background) {
        tlog() << "build index " << ns << ' ' << idx.keyPattern() << ( background ? " background" : "" ) << endl;
        Timer t;
        unsigned long long n;

        verify( !BackgroundOperation::inProgForNs(ns.c_str()) ); // should have been checked earlier, better not be...
        verify( d->indexBuildInProgress == 0 );
        verify( Lock::isWriteLocked(ns) );
        RecoverableIndexState recoverable( d );

        // Build index spec here in case the collection is empty and the index details are invalid
        idx.getSpec();

        if( inDBRepair || !background ) {
            n = fastBuildIndex(ns.c_str(), d, idx, idxNo);
            verify( !idx.head.isNull() );
        }
        else {
            BackgroundIndexBuildJob j(ns.c_str());
            n = j.go(ns, d, idx, idxNo);
        }
        tlog() << "build index done.  scanned " << n << " total records. " << t.millis() / 1000.0 << " secs" << endl;
    }

    /* add keys to indexes for a new record */
#if 0
    static void oldIndexRecord__notused(NamespaceDetails *d, BSONObj obj, DiskLoc loc) {
        int n = d->nIndexesBeingBuilt();
        for ( int i = 0; i < n; i++ ) {
            try {
                bool unique = d->idx(i).unique();
                addKeysToIndex(d, i, obj, loc, /*dupsAllowed*/!unique);
            }
            catch( DBException& ) {
                /* try to roll back previously added index entries
                   note <= i (not < i) is important here as the index we were just attempted
                   may be multikey and require some cleanup.
                */
                for( int j = 0; j <= i; j++ ) {
                    try {
                        _unindexRecord(d->idx(j), obj, loc, false);
                    }
                    catch(...) {
                        log(3) << "unindex fails on rollback after unique failure\n";
                    }
                }
                throw;
            }
        }
    }
#endif

    extern BSONObj id_obj; // { _id : 1 }

    void ensureHaveIdIndex(const char *ns) {
        NamespaceDetails *d = nsdetails(ns);
        if ( d == 0 || d->isSystemFlagSet(NamespaceDetails::Flag_HaveIdIndex) )
            return;

        d->setSystemFlag( NamespaceDetails::Flag_HaveIdIndex );

        {
            NamespaceDetails::IndexIterator i = d->ii();
            while( i.more() ) {
                if( i.next().isIdIndex() )
                    return;
            }
        }

        string system_indexes = cc().database()->name + ".system.indexes";

        BSONObjBuilder b;
        b.append("name", "_id_");
        b.append("ns", ns);
        b.append("key", id_obj);
        BSONObj o = b.done();

        /* edge case: note the insert could fail if we have hit maxindexes already */
        theDataFileMgr.insert(system_indexes.c_str(), o.objdata(), o.objsize(), true);
    }

#pragma pack(1)
    struct IDToInsert_ {
        char type;
        char _id[4];
        OID oid;
        IDToInsert_() {
            type = (char) jstOID;
            strcpy(_id, "_id");
            verify( sizeof(IDToInsert_) == 17 );
        }
    } idToInsert_;
#pragma pack()
    struct IDToInsert : public BSONElement {
        IDToInsert() : BSONElement( ( char * )( &idToInsert_ ) ) {}
    } idToInsert;

    void DataFileMgr::insertAndLog( const char *ns, const BSONObj &o, bool god, bool fromMigrate ) {
        BSONObj tmp = o;
        insertWithObjMod( ns, tmp, god );
        logOp( "i", ns, tmp, 0, 0, fromMigrate );
    }

    /** @param o the object to insert. can be modified to add _id and thus be an in/out param
     */
    DiskLoc DataFileMgr::insertWithObjMod(const char *ns, BSONObj &o, bool god) {
        bool addedID = false;
        DiskLoc loc = insert( ns, o.objdata(), o.objsize(), god, true, &addedID );
        if( addedID && !loc.isNull() )
            o = BSONObj( loc.rec() );
        return loc;
    }

    bool prepareToBuildIndex(const BSONObj& io, bool god, string& sourceNS, NamespaceDetails *&sourceCollection, BSONObj& fixedIndexObject );

    // We are now doing two btree scans for all unique indexes (one here, and one when we've
    // written the record to the collection.  This could be made more efficient inserting
    // dummy data here, keeping pointers to the btree nodes holding the dummy data and then
    // updating the dummy data with the DiskLoc of the real record.
    void checkNoIndexConflicts( NamespaceDetails *d, const BSONObj &obj ) {
        for ( int idxNo = 0; idxNo < d->nIndexes; idxNo++ ) {
            if( d->idx(idxNo).unique() ) {
                IndexDetails& idx = d->idx(idxNo);
                BSONObjSet keys;
                idx.getKeysFromObject(obj, keys);
                BSONObj order = idx.keyPattern();
                IndexInterface& ii = idx.idxInterface();
                for ( BSONObjSet::iterator i=keys.begin(); i != keys.end(); i++ ) {
                    // WARNING: findSingle may not be compound index safe.  this may need to change.  see notes in 
                    // findSingle code.
                    uassert( 12582, "duplicate key insert for unique index of capped collection",
                             ii.findSingle(idx, idx.head, *i ).isNull() );
                }
            }
        }
    }

    /** add a record to the end of the linked list chain within this extent. 
        require: you must have already declared write intent for the record header.        
    */
    void addRecordToRecListInExtent(Record *r, DiskLoc loc) {
        dassert( loc.rec() == r );
        Extent *e = r->myExtent(loc);
        if ( e->lastRecord.isNull() ) {
            Extent::FL *fl = getDur().writing(e->fl());
            fl->firstRecord = fl->lastRecord = loc;
            r->prevOfs() = r->nextOfs() = DiskLoc::NullOfs;
        }
        else {
            Record *oldlast = e->lastRecord.rec();
            r->prevOfs() = e->lastRecord.getOfs();
            r->nextOfs() = DiskLoc::NullOfs;
            getDur().writingInt(oldlast->nextOfs()) = loc.getOfs();
            getDur().writingDiskLoc(e->lastRecord) = loc;
        }
    }

    NOINLINE_DECL DiskLoc outOfSpace(const char *ns, NamespaceDetails *d, int lenWHdr, bool god, DiskLoc extentLoc) {
        DiskLoc loc;
        if ( ! d->isCapped() ) { // size capped doesn't grow
            log(1) << "allocating new extent for " << ns << " padding:" << d->paddingFactor() << " lenWHdr: " << lenWHdr << endl;
            cc().database()->allocExtent(ns, Extent::followupSize(lenWHdr, d->lastExtentSize), false, !god);
            loc = d->alloc(ns, lenWHdr, extentLoc);
            if ( loc.isNull() ) {
                log() << "warning: alloc() failed after allocating new extent. lenWHdr: " << lenWHdr << " last extent size:" << d->lastExtentSize << "; trying again\n";
                for ( int z=0; z<10 && lenWHdr > d->lastExtentSize; z++ ) {
                    log() << "try #" << z << endl;
                    cc().database()->allocExtent(ns, Extent::followupSize(lenWHdr, d->lastExtentSize), false, !god);
                    loc = d->alloc(ns, lenWHdr, extentLoc);
                    if ( ! loc.isNull() )
                        break;
                }
            }
        }
        return loc;
    }

    /** used by insert and also compact
      * @return null loc if out of space 
      */
    DiskLoc allocateSpaceForANewRecord(const char *ns, NamespaceDetails *d, int lenWHdr, bool god) {
        DiskLoc extentLoc;
        DiskLoc loc = d->alloc(ns, lenWHdr, extentLoc);
        if ( loc.isNull() ) {
            loc = outOfSpace(ns, d, lenWHdr, god, extentLoc);
        }
        return loc;
    }

    bool NOINLINE_DECL insert_checkSys(const char *sys, const char *ns, bool& wouldAddIndex, const void *obuf, bool god) {
        uassert( 10095 , "attempt to insert in reserved database name 'system'", sys != ns);
        if ( strstr(ns, ".system.") ) {
            // later:check for dba-type permissions here if have that at some point separate
            if ( strstr(ns, ".system.indexes" ) )
                wouldAddIndex = true;
            else if ( legalClientSystemNS( ns , true ) ) {
                if ( obuf && strstr( ns , ".system.users" ) ) {
                    BSONObj t( reinterpret_cast<const char *>( obuf ) );
                    uassert( 14051 , "system.users entry needs 'user' field to be a string" , t["user"].type() == String );
                    uassert( 14052 , "system.users entry needs 'pwd' field to be a string" , t["pwd"].type() == String );
                    uassert( 14053 , "system.users entry needs 'user' field to be non-empty" , t["user"].String().size() );
                    uassert( 14054 , "system.users entry needs 'pwd' field to be non-empty" , t["pwd"].String().size() );
                }
            }
            else if ( !god ) {
                // todo this should probably uasseert rather than doing this:
                log() << "ERROR: attempt to insert in system namespace " << ns << endl;
                return false;
            }
        }
        return true;
    }

    NOINLINE_DECL NamespaceDetails* insert_newNamespace(const char *ns, int len, bool god) { 
        checkConfigNS(ns);
        // This may create first file in the database.
        int ies = Extent::initialSize(len);
        if( str::contains(ns, '$') && len + Record::HeaderSize >= BtreeData_V1::BucketSize - 256 && len + Record::HeaderSize <= BtreeData_V1::BucketSize + 256 ) { 
            // probably an index.  so we pick a value here for the first extent instead of using initialExtentSize() which is more 
            // for user collections.  TODO: we could look at the # of records in the parent collection to be smarter here.
            ies = (32+4) * 1024;
        }
        cc().database()->allocExtent(ns, ies, false, false);
        NamespaceDetails *d = nsdetails(ns);
        if ( !god )
            ensureIdIndexForNewNs(ns);
        addNewNamespaceToCatalog(ns);
        return d;
    }

    void NOINLINE_DECL insert_makeIndex(NamespaceDetails *tableToIndex, const string& tabletoidxns, const DiskLoc& loc) { 
        uassert( 13143 , "can't create index on system.indexes" , tabletoidxns.find( ".system.indexes" ) == string::npos );

        BSONObj info = loc.obj();
        bool background = info["background"].trueValue();
        // if this is not readable, let's move things along
        if (background && ((!theReplSet && cc().isSyncThread()) || (theReplSet && !theReplSet->isSecondary()))) {
            log() << "info: indexing in foreground on this replica; was a background index build on the primary" << endl;
            background = false;
        }

        int idxNo = tableToIndex->nIndexes;
        IndexDetails& idx = tableToIndex->addIndex(tabletoidxns.c_str(), !background); // clear transient info caches so they refresh; increments nIndexes
        getDur().writingDiskLoc(idx.info) = loc;
        try {
            buildAnIndex(tabletoidxns, tableToIndex, idx, idxNo, background);
        }
        catch( DBException& e ) {
            // save our error msg string as an exception or dropIndexes will overwrite our message
            LastError *le = lastError.get();
            int savecode = 0;
            string saveerrmsg;
            if ( le ) {
                savecode = le->code;
                saveerrmsg = le->msg;
            }
            else {
                savecode = e.getCode();
                saveerrmsg = e.what();
            }

            // roll back this index
            string name = idx.indexName();
            BSONObjBuilder b;
            string errmsg;
            bool ok = dropIndexes(tableToIndex, tabletoidxns.c_str(), name.c_str(), errmsg, b, true);
            if( !ok ) {
                log() << "failed to drop index after a unique key error building it: " << errmsg << ' ' << tabletoidxns << ' ' << name << endl;
            }

            verify( le && !saveerrmsg.empty() );
            setLastError(savecode,saveerrmsg.c_str());
            throw;
        }
    }

    /* if god==true, you may pass in obuf of NULL and then populate the returned DiskLoc
         after the call -- that will prevent a double buffer copy in some cases (btree.cpp).

       @param mayAddIndex almost always true, except for invocation from rename namespace command.
       @param addedID if not null, set to true if adding _id element. you must assure false before calling
              if using.
    */

    DiskLoc DataFileMgr::insert(const char *ns, const void *obuf, int len, bool god, bool mayAddIndex, bool *addedID) {
        bool wouldAddIndex = false;
        massert( 10093 , "cannot insert into reserved $ collection", god || NamespaceString::normal( ns ) );
        uassert( 10094 , str::stream() << "invalid ns: " << ns , isValidNS( ns ) );
        {
            const char *sys = strstr(ns, "system.");
            if ( sys && !insert_checkSys(sys, ns, wouldAddIndex, obuf, god) )
                return DiskLoc();
        }
        bool addIndex = wouldAddIndex && mayAddIndex;

        NamespaceDetails *d = nsdetails(ns);
        if ( d == 0 ) {
            d = insert_newNamespace(ns, len, god);
        }

        NamespaceDetails *tableToIndex = 0;

        string tabletoidxns;
        BSONObj fixedIndexObject;
        if ( addIndex ) {
            verify( obuf );
            BSONObj io((const char *) obuf);
            if( !prepareToBuildIndex(io, god, tabletoidxns, tableToIndex, fixedIndexObject ) ) {
                // prepare creates _id itself, or this indicates to fail the build silently (such 
                // as if index already exists)
                return DiskLoc();
            }
            if ( ! fixedIndexObject.isEmpty() ) {
                obuf = fixedIndexObject.objdata();
                len = fixedIndexObject.objsize();
            }
        }

        int addID = 0; // 0 if not adding _id; if adding, the length of that new element
        if( !god ) {
            /* Check if we have an _id field. If we don't, we'll add it.
               Note that btree buckets which we insert aren't BSONObj's, but in that case god==true.
            */
            BSONObj io((const char *) obuf);
            BSONElement idField = io.getField( "_id" );
            uassert( 10099 ,  "_id cannot be an array", idField.type() != Array );
            // we don't add _id for capped collections as they don't have an _id index
            if( idField.eoo() && !wouldAddIndex && strstr(ns, ".local.") == 0 && d->haveIdIndex() ) {
                if( addedID )
                    *addedID = true;
                addID = len;
                idToInsert_.oid.init();
                len += idToInsert.size();
            }

            BSONElementManipulator::lookForTimestamps( io );
        }

        int lenWHdr = d->getRecordAllocationSize( len + Record::HeaderSize );

        // If the collection is capped, check if the new object will violate a unique index
        // constraint before allocating space.
        if ( d->nIndexes && d->isCapped() && !god ) {
            checkNoIndexConflicts( d, BSONObj( reinterpret_cast<const char *>( obuf ) ) );
        }

        bool earlyIndex = true;
        DiskLoc loc;
        if( addID || tableToIndex || d->isCapped() ) {
            // if need id, we don't do the early indexing. this is not the common case so that is sort of ok
            earlyIndex = false;
            loc = allocateSpaceForANewRecord(ns, d, lenWHdr, god);
        }
        else {
            loc = d->allocWillBeAt(ns, lenWHdr);
            if( loc.isNull() ) {
                // need to get a new extent so we have to do the true alloc now (not common case)
                earlyIndex = false;
                loc = allocateSpaceForANewRecord(ns, d, lenWHdr, god);
            }
        }
        if ( loc.isNull() ) {
            log() << "insert: couldn't alloc space for object ns:" << ns << " capped:" << d->isCapped() << endl;
            verify(d->isCapped());
            return DiskLoc();
        }

        if( earlyIndex ) { 
            // add record to indexes using two step method so we can do the reading outside a write lock
            if ( d->nIndexes ) {
                verify( obuf );
                BSONObj obj((const char *) obuf);
                try {
                    indexRecordUsingTwoSteps(ns, d, obj, loc, true);
                }
                catch( AssertionException& ) {
                    // should be a dup key error on _id index
                    dassert( !tableToIndex && !d->isCapped() );
                    // no need to delete/rollback the record as it was not added yet
                    throw;
                }
            }
            // really allocate now
            DiskLoc real = allocateSpaceForANewRecord(ns, d, lenWHdr, god);
            verify( real == loc );
        }

        Record *r = loc.rec();
        {
            verify( r->lengthWithHeaders() >= lenWHdr );
            r = (Record*) getDur().writingPtr(r, lenWHdr);
            if( addID ) {
                /* a little effort was made here to avoid a double copy when we add an ID */
                little<int>::ref( r->data() ) = little<int>::ref( obuf ) + idToInsert.size();
                memcpy(r->data()+4, idToInsert.rawdata(), idToInsert.size());
                memcpy(r->data()+4+idToInsert.size(), ((char *)obuf)+4, addID-4);
            }
            else {
                if( obuf ) // obuf can be null from internal callers
                    memcpy(r->data(), obuf, len);
            }
        }

        addRecordToRecListInExtent(r, loc);

        /* durability todo : this could be a bit annoying / slow to record constantly */
        {
            NamespaceDetails::Stats *s = getDur().writing(&d->stats);
            s->datasize += r->netLength();
            s->nrecords++;
        }

        // we don't bother resetting query optimizer stats for the god tables - also god is true when adding a btree bucket
        if ( !god )
            NamespaceDetailsTransient::get( ns ).notifyOfWriteOp();

        if ( tableToIndex ) {
            insert_makeIndex(tableToIndex, tabletoidxns, loc);
        }

        /* add this record to our indexes */
        if ( !earlyIndex && d->nIndexes ) {
            try {
                BSONObj obj(r->data());
                // not sure which of these is better -- either can be used.  oldIndexRecord may be faster, 
                // but twosteps handles dup key errors more efficiently.
                //oldIndexRecord(d, obj, loc);
                indexRecordUsingTwoSteps(ns, d, obj, loc, false);

            }
            catch( AssertionException& e ) {
                // should be a dup key error on _id index
                if( tableToIndex || d->isCapped() ) {
                    massert( 12583, "unexpected index insertion failure on capped collection", !d->isCapped() );
                    string s = e.toString();
                    s += " : on addIndex/capped - collection and its index will not match";
                    setLastError(0, s.c_str());
                    error() << s << endl;
                }
                else {
                    // normal case -- we can roll back
                    _deleteRecord(d, ns, r, loc);
                    throw;
                }
            }
        }

        d->paddingFits();

        return loc;
    }

    /* special version of insert for transaction logging -- streamlined a bit.
       assumes ns is capped and no indexes
    */
    Record* DataFileMgr::fast_oplog_insert(NamespaceDetails *d, const char *ns, int len) {
        verify( d );
        RARELY verify( d == nsdetails(ns) );
        DEV verify( d == nsdetails(ns) );

        DiskLoc extentLoc;
        int lenWHdr = len + Record::HeaderSize;
        DiskLoc loc = d->alloc(ns, lenWHdr, extentLoc);
        verify( !loc.isNull() );

        Record *r = loc.rec();
        verify( r->lengthWithHeaders() >= lenWHdr );

        Extent *e = r->myExtent(loc);
        if ( e->lastRecord.isNull() ) {
            Extent::FL *fl = getDur().writing( e->fl() );
            fl->firstRecord = fl->lastRecord = loc;

            Record::NP *np = getDur().writing(r->np());
            np->nextOfs = np->prevOfs = DiskLoc::NullOfs;
        }
        else {
            Record *oldlast = e->lastRecord.rec();
            Record::NP *np = getDur().writing(r->np());
            np->prevOfs = e->lastRecord.getOfs();
            np->nextOfs = DiskLoc::NullOfs;
            getDur().writingInt( oldlast->nextOfs() ) = loc.getOfs();
            e->lastRecord.writing() = loc;
        }

        /* todo: don't update for oplog?  seems wasteful. */
        {
            NamespaceDetails::Stats *s = getDur().writing(&d->stats);
            s->datasize += r->netLength();
            s->nrecords++;
        }

        return r;
    }

} // namespace mongo

#include "clientcursor.h"

namespace mongo {

    void dropAllDatabasesExceptLocal() {
        Lock::GlobalWrite lk;

        vector<string> n;
        getDatabaseNames(n);
        if( n.size() == 0 ) return;
        log() << "dropAllDatabasesExceptLocal " << n.size() << endl;
        for( vector<string>::iterator i = n.begin(); i != n.end(); i++ ) {
            if( *i != "local" ) {
                Client::Context ctx(*i);
                dropDatabase(*i);
            }
        }
    }

    void dropDatabase(string db) {
        log(1) << "dropDatabase " << db << endl;
        Lock::assertWriteLocked(db);
        Database *d = cc().database();
        verify( d );
        verify( d->name == db );

        BackgroundOperation::assertNoBgOpInProgForDb(d->name.c_str());

        // Not sure we need this here, so removed.  If we do, we need to move it down 
        // within other calls both (1) as they could be called from elsewhere and 
        // (2) to keep the lock order right - groupcommitmutex must be locked before 
        // mmmutex (if both are locked).
        //
        //  RWLockRecursive::Exclusive lk(MongoFile::mmmutex);

        getDur().syncDataAndTruncateJournal();

        Database::closeDatabase( d->name.c_str(), d->path );
        d = 0; // d is now deleted

        _deleteDataFiles( db.c_str() );
    }

    typedef boost::filesystem::path Path;

    void boostRenameWrapper( const Path &from, const Path &to ) {
        try {
            boost::filesystem::rename( from, to );
        }
        catch ( const boost::filesystem::filesystem_error & ) {
            // boost rename doesn't work across partitions
            boost::filesystem::copy_file( from, to);
            boost::filesystem::remove( from );
        }
    }

    // back up original database files to 'temp' dir
    void _renameForBackup( const char *database, const Path &reservedPath ) {
        Path newPath( reservedPath );
        if ( directoryperdb )
            newPath /= database;
        class Renamer : public FileOp {
        public:
            Renamer( const Path &newPath ) : newPath_( newPath ) {}
        private:
            const boost::filesystem::path &newPath_;
            virtual bool apply( const Path &p ) {
                if ( !boost::filesystem::exists( p ) )
                    return false;
                boostRenameWrapper( p, newPath_ / ( p.leaf() + ".bak" ) );
                return true;
            }
            virtual const char * op() const {
                return "renaming";
            }
        } renamer( newPath );
        _applyOpToDataFiles( database, renamer, true );
    }

    // move temp files to standard data dir
    void _replaceWithRecovered( const char *database, const char *reservedPathString ) {
        Path newPath( dbpath );
        if ( directoryperdb )
            newPath /= database;
        class Replacer : public FileOp {
        public:
            Replacer( const Path &newPath ) : newPath_( newPath ) {}
        private:
            const boost::filesystem::path &newPath_;
            virtual bool apply( const Path &p ) {
                if ( !boost::filesystem::exists( p ) )
                    return false;
                boostRenameWrapper( p, newPath_ / p.leaf() );
                return true;
            }
            virtual const char * op() const {
                return "renaming";
            }
        } replacer( newPath );
        _applyOpToDataFiles( database, replacer, true, reservedPathString );
    }

    // generate a directory name for storing temp data files
    Path uniqueReservedPath( const char *prefix ) {
        Path repairPath = Path( repairpath );
        Path reservedPath;
        int i = 0;
        bool exists = false;
        do {
            stringstream ss;
            ss << prefix << "_repairDatabase_" << i++;
            reservedPath = repairPath / ss.str();
            MONGO_ASSERT_ON_EXCEPTION( exists = boost::filesystem::exists( reservedPath ) );
        }
        while ( exists );
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

    bool repairDatabase( string dbNameS , string &errmsg,
                         bool preserveClonedFilesOnFailure, bool backupOriginalFiles ) {
        doingRepair dr;
        dbNameS = nsToDatabase( dbNameS );
        const char * dbName = dbNameS.c_str();

        stringstream ss;
        ss << "localhost:" << cmdLine.port;
        string localhost = ss.str();

        problem() << "repairDatabase " << dbName << endl;
        verify( cc().database()->name == dbName );
        verify( cc().database()->path == dbpath );

        BackgroundOperation::assertNoBgOpInProgForDb(dbName);

        getDur().syncDataAndTruncateJournal(); // Must be done before and after repair

        boost::intmax_t totalSize = dbSize( dbName );
        boost::intmax_t freeSize = File::freeSpace(repairpath);
        if ( freeSize > -1 && freeSize < totalSize ) {
            stringstream ss;
            ss << "Cannot repair database " << dbName << " having size: " << totalSize
               << " (bytes) because free disk space is: " << freeSize << " (bytes)";
            errmsg = ss.str();
            problem() << errmsg << endl;
            return false;
        }

        killCurrentOp.checkForInterrupt();

        Path reservedPath =
            uniqueReservedPath( ( preserveClonedFilesOnFailure || backupOriginalFiles ) ?
                                "backup" : "_tmp" );
        MONGO_ASSERT_ON_EXCEPTION( boost::filesystem::create_directory( reservedPath ) );
        string reservedPathString = reservedPath.native_directory_string();

        bool res;
        {
            // clone to temp location, which effectively does repair
            Client::Context ctx( dbName, reservedPathString );
            verify( ctx.justCreated() );

            res = cloneFrom(localhost.c_str(), errmsg, dbName,
                            /*logForReplication=*/false, /*slaveOk*/false, /*replauth*/false,
                            /*snapshot*/false, /*mayYield*/false, /*mayBeInterrupted*/true);
            Database::closeDatabase( dbName, reservedPathString.c_str() );
        }

        if ( !res ) {
            errmsg = str::stream() << "clone failed for " << dbName << " with error: " << errmsg;
            problem() << errmsg << endl;

            if ( !preserveClonedFilesOnFailure )
                MONGO_ASSERT_ON_EXCEPTION( boost::filesystem::remove_all( reservedPath ) );

            getDur().syncDataAndTruncateJournal(); // Must be done before and after repair

            return false;
        }

        MongoFile::flushAll(true);

        Client::Context ctx( dbName );
        Database::closeDatabase( dbName, dbpath );

        if ( backupOriginalFiles ) {
            _renameForBackup( dbName, reservedPath );
        }
        else {
            _deleteDataFiles( dbName );
            MONGO_ASSERT_ON_EXCEPTION( boost::filesystem::create_directory( Path( dbpath ) / dbName ) );
        }

        _replaceWithRecovered( dbName, reservedPathString.c_str() );

        if ( !backupOriginalFiles )
            MONGO_ASSERT_ON_EXCEPTION( boost::filesystem::remove_all( reservedPath ) );

        getDur().syncDataAndTruncateJournal(); // Must be done before and after repair

        return true;
    }

    void _applyOpToDataFiles( const char *database, FileOp &fo, bool afterAllocator, const string& path ) {
        if ( afterAllocator )
            FileAllocator::get()->waitUntilFinished();
        string c = database;
        c += '.';
        boost::filesystem::path p(path);
        if ( directoryperdb )
            p /= database;
        boost::filesystem::path q;
        q = p / (c+"ns");
        bool ok = false;
        MONGO_ASSERT_ON_EXCEPTION( ok = fo.apply( q ) );
        if ( ok )
            log(2) << fo.op() << " file " << q.string() << endl;
        int i = 0;
        int extra = 10; // should not be necessary, this is defensive in case there are missing files
        while ( 1 ) {
            verify( i <= DiskLoc::MaxFiles );
            stringstream ss;
            ss << c << i;
            q = p / ss.str();
            MONGO_ASSERT_ON_EXCEPTION( ok = fo.apply(q) );
            if ( ok ) {
                if ( extra != 10 ) {
                    log(1) << fo.op() << " file " << q.string() << endl;
                    log() << "  _applyOpToDataFiles() warning: extra == " << extra << endl;
                }
            }
            else if ( --extra <= 0 )
                break;
            i++;
        }
    }

    NamespaceDetails* nsdetails_notinline(const char *ns) { return nsdetails(ns); }

    bool DatabaseHolder::closeAll( const string& path , BSONObjBuilder& result , bool force ) {
        log() << "DatabaseHolder::closeAll path:" << path << endl;
        verify( Lock::isW() );
        getDur().commitNow(); // bad things happen if we close a DB with outstanding writes

        map<string,Database*>& m = _paths[path];
        _size -= m.size();

        set< string > dbs;
        for ( map<string,Database*>::iterator i = m.begin(); i != m.end(); i++ ) {
            wassert( i->second->path == path );
            dbs.insert( i->first );
        }

        currentClient.get()->getContext()->_clear();

        BSONObjBuilder bb( result.subarrayStart( "dbs" ) );
        int n = 0;
        int nNotClosed = 0;
        for( set< string >::iterator i = dbs.begin(); i != dbs.end(); ++i ) {
            string name = *i;
            log(2) << "DatabaseHolder::closeAll path:" << path << " name:" << name << endl;
            Client::Context ctx( name , path );
            if( !force && BackgroundOperation::inProgForDb(name.c_str()) ) {
                log() << "WARNING: can't close database " << name << " because a bg job is in progress - try killOp command" << endl;
                nNotClosed++;
            }
            else {
                Database::closeDatabase( name.c_str() , path );
                bb.append( bb.numStr( n++ ) , name );
            }
        }
        bb.done();
        if( nNotClosed )
            result.append("nNotClosed", nNotClosed);
        else {
            ClientCursor::assertNoCursors();
        }

        return true;
    }

} // namespace mongo
