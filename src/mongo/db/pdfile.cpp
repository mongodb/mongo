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

#include "mongo/pch.h"

#include "mongo/db/pdfile.h"

#include <algorithm>
#include <boost/filesystem/operations.hpp>
#include <list>

#include "mongo/base/counter.h"
#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/pdfile_private.h"
#include "mongo/db/background.h"
#include "mongo/db/btree.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop-inl.h"
#include "mongo/db/db.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/extsort.h"
#include "mongo/db/index_update.h"
#include "mongo/db/instance.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/memconcept.h"
#include "mongo/db/namespace-inl.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/repl.h"
#include "mongo/db/replutil.h"
#include "mongo/db/sort_phase_one.h"
#include "mongo/util/file.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/hashtab.h"
#include "mongo/util/mmap.h"
#include "mongo/util/processinfo.h"

namespace mongo {

    BOOST_STATIC_ASSERT( sizeof(Extent)-4 == 48+128 );
    BOOST_STATIC_ASSERT( sizeof(DataFileHeader)-4 == 8192 );

    bool isValidNS( const StringData& ns ) {
        // TODO: should check for invalid characters

        size_t idx = ns.find( '.' );
        if ( idx == string::npos )
            return false;

        if ( idx == ns.size() - 1 )
            return false;

        return true;
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
            LOG( 1 ) << "adding _id index for collection " << ns << endl;
            ensureHaveIdIndex( ns, false );
        }
    }

    static void _ensureSystemIndexes(const char* ns) {
        NamespaceString nsstring(ns);
        if (StringData(nsstring.coll).substr(0, 7) == "system.") {
            authindex::createSystemIndexes(nsstring);
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
             !( str::startsWith( ns, "config." ) ||
                str::startsWith( ns, "local." ) ||
                str::startsWith( ns, "admin." ) ) ) {
            uasserted(14037, "can't create user databases on a --configsvr instance");
        }
    }

    bool _userCreateNS(const char *ns, const BSONObj& options, string& err, bool *deferIdIndex) {
        LOG(1) << "create collection " << ns << ' ' << options << endl;

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
                uassert( 10083 , "create collection invalid size spec", size > 0 );

                size += 0xff;
                size &= 0xffffffffffffff00LL;
                if ( size < Extent::minSize() )
                    size = Extent::minSize();
            }
        }

        bool newCapped = false;
        long long mx = 0;
        if( options["capped"].trueValue() ) {
            newCapped = true;
            BSONElement e = options.getField("max");
            if ( e.isNumber() ) {
                mx = e.numberLong();
                uassert( 16495,
                         "max in a capped collection has to be < 2^31 or not set",
                         NamespaceDetails::validMaxCappedDocs(&mx) );
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
        
        bool ensure = true;

        // respect autoIndexId if set. otherwise, create an _id index for all colls, except for
        // capped ones in local w/o autoIndexID (reason for the exception is for the oplog and
        //  non-replicated capped colls)
        if( options.hasField( "autoIndexId" ) ||
            (newCapped && nsToDatabase( ns ) == "local" ) ) {
            ensure = options.getField( "autoIndexId" ).trueValue();
        }

        if( ensure ) {
            if( deferIdIndex )
                *deferIdIndex = true;
            else
                ensureIdIndexForNewNs( ns );
        }

        _ensureSystemIndexes(ns);

        if ( mx > 0 )
            d->setMaxCappedDocs( mx );

        bool isFreeList = strstr(ns, FREELIST_NS) != 0;
        if( !isFreeList )
            addNewNamespaceToCatalog(ns, options.isEmpty() ? 0 : &options);
        
        if ( options["flags"].numberInt() ) {
            d->replaceUserFlags( options["flags"].numberInt() );
        }

        return true;
    }

    /** { ..., capped: true, size: ..., max: ... }
        @param deferIdIndex - if not not, defers id index creation.  sets the bool value to true if we wanted to create the id index.
        @return true if successful
    */
    bool userCreateNS(const char *ns, BSONObj options, string& err, bool logForReplication, bool *deferIdIndex) {
        const char *coll = strchr( ns, '.' ) + 1;
        massert( 10356 ,  str::stream() << "invalid ns: " << ns , NamespaceString::validCollectionName(ns));
        bool ok = _userCreateNS(ns, options, err, deferIdIndex);
        if ( logForReplication && ok ) {
            if ( options.getField( "create" ).eoo() ) {
                BSONObjBuilder b;
                b << "create" << coll;
                b.appendElements( options );
                options = b.obj();
            }
            string logNs = nsToDatabase(ns) + ".$cmd";
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
        int ExtentSize = min(header()->unusedLength, approxSize);
        DiskLoc loc;
        if ( ExtentSize < Extent::minSize() ) {
            /* note there could be a lot of looping here is db just started and
               no files are open yet.  we might want to do something about that. */
            if ( loops > 8 ) {
                verify( loops < 10000 );
                out() << "warning: loops=" << loops << " fileno:" << fileNo << ' ' << ns << '\n';
            }
            log() << "newExtent: " << ns << " file " << fileNo << " full, adding a new file" << endl;
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
        NamespaceDetails *f = nsdetails(s);
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

            if( n > 128 ) LOG( n < 512 ? 1 : 0 ) << "warning: newExtent " << n << " scanned\n";

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
                OCCASIONALLY if( n > 512 ) log() << "warning: newExtent " << n << " scanned" << endl;
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
            int& ofs = emptyLoc.GETOFS();
            int newOfs = (ofs + 0xfff) & ~0xfff; 
            delRecLength -= (newOfs-ofs);
            dassert( delRecLength > 0 );
            ofs = newOfs;
        }
    }

    DiskLoc Extent::_reuse(const char *nsname, bool capped) {
        LOG(3) << "_reuse extent was:" << nsDiagnostic.toString() << " now:" << nsname << endl;
        if (magic != extentSignature) {
            StringBuilder sb;
            sb << "bad extent signature " << toHex(&magic, 4)
               << " for namespace '" << nsDiagnostic.toString()
               << "' found in Extent::_reuse";
            msgasserted(10360, sb.str());
        }
        nsDiagnostic = nsname;
        markEmpty();

        DiskLoc emptyLoc;
        int delRecLength;
        getEmptyLoc(nsname, myLoc, length, capped, emptyLoc, delRecLength);

        // todo: some dup code here and below in Extent::init
        DeletedRecord* empty = getDur().writing(DataFileMgr::getDeletedRecord(emptyLoc));
        empty->lengthWithHeaders() = delRecLength;
        empty->extentOfs() = myLoc.getOfs();
        empty->nextDeleted().Null();
        return emptyLoc;
    }

    /* assumes already zeroed -- insufficient for block 'reuse' perhaps */
    DiskLoc Extent::init(const char *nsname, int _length, int _fileNo, int _offset, bool capped) {
        magic = extentSignature;
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

        DeletedRecord* empty = getDur().writing(DataFileMgr::getDeletedRecord(emptyLoc));
        empty->lengthWithHeaders() = delRecLength;
        empty->extentOfs() = myLoc.getOfs();
        empty->nextDeleted().Null();
        return emptyLoc;
    }

    bool Extent::validates(const DiskLoc diskLoc, BSONArrayBuilder* errors) {
        bool extentOk = true;
        if (magic != extentSignature) {
            if (errors) {
                StringBuilder sb;
                sb << "bad extent signature " << toHex(&magic, 4)
                    << " in extent " << diskLoc.toString();
                *errors << sb.str();
            }
            extentOk = false;
        }
        if (myLoc != diskLoc) {
            if (errors) {
                StringBuilder sb;
                sb << "extent " << diskLoc.toString()
                    << " self-pointer is " << myLoc.toString();
                *errors << sb.str();
            }
            extentOk = false;
        }
        if (firstRecord.isNull() != lastRecord.isNull()) {
            if (errors) {
                StringBuilder sb;
                if (firstRecord.isNull()) {
                    sb << "in extent " << diskLoc.toString()
                        << ", firstRecord is null but lastRecord is "
                        << lastRecord.toString();
                }
                else {
                    sb << "in extent " << diskLoc.toString()
                        << ", firstRecord is " << firstRecord.toString()
                        << " but lastRecord is null";
                }
                *errors << sb.str();
            }
            extentOk = false;
        }
        if (length < minSize()) {
            if (errors) {
                StringBuilder sb;
                sb << "length of extent " << diskLoc.toString()
                    << " is " << length
                    << ", which is less than minimum length of " << minSize();
                *errors << sb.str();
            }
            extentOk = false;
        }
        return extentOk;
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

    DataFileMgr::DataFileMgr() : _precalcedMutex("PrecalcedMutex"), _precalced(NULL) {
    }

    SortPhaseOne* DataFileMgr::getPrecalced() const {
        return _precalced;
    }

    void DataFileMgr::setPrecalced(SortPhaseOne* precalced) {
        _precalced = precalced;
    }

    shared_ptr<Cursor> DataFileMgr::findAll(const StringData& ns, const DiskLoc &startLoc) {
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
            return shared_ptr<Cursor>( ForwardCappedCursor::make( d , startLoc ) );

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
        NamespaceDetails *freeExtents = nsdetails(s);
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
        NamespaceDetails *freeExtents = nsdetails(s);
        if( freeExtents == 0 ) {
            string err;
            _userCreateNS(s.c_str(), BSONObj(), err, 0); // todo: this actually allocates an extent, which is bad!
            freeExtents = nsdetails(s);
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
        NamespaceDetails* d = nsdetails(nsToDrop);
        uassert( 10086 ,  (string)"ns not found: " + nsToDrop , d );

        BackgroundOperation::assertNoBgOpInProgForNs(nsToDrop.c_str());

        NamespaceString s(nsToDrop);
        verify( s.db == cc().database()->name );
        if( s.isSystem() ) {
            if( s.coll == "system.profile" ) {
                uassert( 10087,
                         "turn off profiling before dropping system.profile collection",
                         cc().database()->getProfilingLevel() == 0 );
            }
            else {
                uasserted( 12502, "can't drop system ns" );
            }
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
        LOG(1) << "dropCollection: " << name << endl;
        NamespaceDetails *d = nsdetails(name);
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
        LOG(1) << "\t dropIndexes done" << endl;
        result.append("ns", name.c_str());
        ClientCursor::invalidate(name.c_str());
        Top::global.collectionDropped( name );
        NamespaceDetailsTransient::eraseCollection( name );
        dropNS(name);
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
                    unsigned long long *p = reinterpret_cast<unsigned long long *>( todelete->data() );
                    *getDur().writing(p) = 0;
                    //DEV memset(todelete->data, 0, todelete->netLength()); // attempt to notice invalid reuse.
                }
                d->addDeletedRec((DeletedRecord*)todelete, dl);
            }
        }
    }

    void DataFileMgr::deleteRecord(const char *ns, Record *todelete, const DiskLoc& dl, bool cappedOK, bool noWarn, bool doLog ) {
        deleteRecord( nsdetails(ns), ns, todelete, dl, cappedOK, noWarn, doLog );
    }

    void DataFileMgr::deleteRecord(NamespaceDetails* d, const char *ns, Record *todelete, const DiskLoc& dl, bool cappedOK, bool noWarn, bool doLog ) {
        dassert( todelete == dl.rec() );

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
        ClientCursor::aboutToDelete(d, dl);

        unindexRecord(d, todelete, dl, noWarn);

        _deleteRecord(d, ns, todelete, dl);
        NamespaceDetailsTransient::get( ns ).notifyOfWriteOp();

        if ( ! toDelete.isEmpty() ) {
            logOp( "d" , ns , toDelete );
        }
    }

    Counter64 moveCounter;
    ServerStatusMetricField<Counter64> moveCounterDisplay( "record.moves", &moveCounter );

    /** Note: if the object shrinks a lot, we don't free up space, we leave extra at end of the record.
     */
    const DiskLoc DataFileMgr::updateRecord(
        const char *ns,
        NamespaceDetails *d,
        NamespaceDetailsTransient *nsdt,
        Record *toupdate, const DiskLoc& dl,
        const char *_buf, int _len, OpDebug& debug,  bool god) {

        dassert( toupdate == dl.rec() );

        BSONObj objOld = BSONObj::make(toupdate);
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

        NamespaceString nsstring(ns);
        if (nsstring.coll == "system.users") {
            uassertStatusOK(AuthorizationManager::checkValidPrivilegeDocument(nsstring.db, objNew));
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
            moveCounter.increment();
            uassert( 10003 , "failing update: objects in a capped ns cannot grow", !(d && d->isCapped()));
            d->paddingTooSmall();
            deleteRecord(ns, toupdate, dl);
            DiskLoc res = insert(ns, objNew.objdata(), objNew.objsize(), false, god);

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
            int z = d->getTotalIndexCount();
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
    struct IDToInsert : public BSONElement {
        IDToInsert() : BSONElement( ( char * )( &idToInsert_ ) ) {}
    } idToInsert;
#pragma pack()

    void DataFileMgr::insertAndLog( const char *ns, const BSONObj &o, bool god, bool fromMigrate ) {
        BSONObj tmp = o;
        insertWithObjMod( ns, tmp, false, god );
        logOp( "i", ns, tmp, 0, 0, fromMigrate );
    }

    /** @param o the object to insert. can be modified to add _id and thus be an in/out param
     */
    DiskLoc DataFileMgr::insertWithObjMod(const char* ns, BSONObj& o, bool mayInterrupt, bool god) {
        bool addedID = false;
        DiskLoc loc = insert( ns, o.objdata(), o.objsize(), mayInterrupt, god, true, &addedID );
        if( addedID && !loc.isNull() )
            o = BSONObj::make( loc.rec() );
        return loc;
    }

    // We are now doing two btree scans for all unique indexes (one here, and one when we've
    // written the record to the collection.  This could be made more efficient inserting
    // dummy data here, keeping pointers to the btree nodes holding the dummy data and then
    // updating the dummy data with the DiskLoc of the real record.
    void checkNoIndexConflicts( NamespaceDetails *d, const BSONObj &obj ) {
        for ( int idxNo = 0; idxNo < d->nIndexes; idxNo++ ) {
            if( d->idx(idxNo).unique() ) {
                IndexDetails& idx = d->idx(idxNo);
                if (ignoreUniqueIndex(idx))
                    continue;
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

    NOINLINE_DECL DiskLoc outOfSpace(const char* ns, NamespaceDetails* d, int lenWHdr, bool god) {
        DiskLoc loc;
        if ( ! d->isCapped() ) { // size capped doesn't grow
            LOG(1) << "allocating new extent for " << ns << " padding:" << d->paddingFactor() << " lenWHdr: " << lenWHdr << endl;
            cc().database()->allocExtent(ns, Extent::followupSize(lenWHdr, d->lastExtentSize), false, !god);
            loc = d->alloc(ns, lenWHdr);
            if ( loc.isNull() ) {
                log() << "warning: alloc() failed after allocating new extent. lenWHdr: " << lenWHdr << " last extent size:" << d->lastExtentSize << "; trying again" << endl;
                for ( int z=0; z<10 && lenWHdr > d->lastExtentSize; z++ ) {
                    log() << "try #" << z << endl;
                    cc().database()->allocExtent(ns, Extent::followupSize(lenWHdr, d->lastExtentSize), false, !god);
                    loc = d->alloc(ns, lenWHdr);
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
    DiskLoc allocateSpaceForANewRecord(const char* ns, NamespaceDetails* d, int lenWHdr, bool god) {
        DiskLoc loc = d->alloc(ns, lenWHdr);
        if ( loc.isNull() ) {
            loc = outOfSpace(ns, d, lenWHdr, god);
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
                    uassertStatusOK(AuthorizationManager::checkValidPrivilegeDocument(
                                            nsToDatabaseSubstring(ns), t));
                }
            }
            else if ( !god ) {
                uasserted(16459, str::stream() << "attempt to insert in system namespace '"
                                               << ns << "'");
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
        _ensureSystemIndexes(ns);
        addNewNamespaceToCatalog(ns);
        return d;
    }

    void NOINLINE_DECL insert_makeIndex(NamespaceDetails* tableToIndex,
                                        const string& tabletoidxns,
                                        const DiskLoc& loc,
                                        bool mayInterrupt) {
        uassert( 13143 , "can't create index on system.indexes" , tabletoidxns.find( ".system.indexes" ) == string::npos );

        BSONObj info = loc.obj();
        bool background = info["background"].trueValue();
        if (background && !isMasterNs(tabletoidxns.c_str())) {
            /* don't do background indexing on slaves.  there are nuances.  this could be added later
                but requires more code.
                */
            log() << "info: indexing in foreground on this replica; was a background index build on the primary" << endl;
            background = false;
        }

        // The total number of indexes right before we write to the collection
        int oldNIndexes = -1;
        int idxNo = tableToIndex->getTotalIndexCount();
        std::string idxName = info["name"].valuestr();

        // Set curop description before setting indexBuildInProg, so that there's something
        // commands can find and kill as soon as indexBuildInProg is set. Only set this if it's a
        // killable index, so we don't overwrite commands in currentOp.
        if (mayInterrupt) {
            cc().curop()->setQuery(info);
        }

        try {
            IndexDetails& idx = tableToIndex->getNextIndexDetails(tabletoidxns.c_str());
            // It's important that this is outside the inner try/catch so that we never try to call
            // kill_idx on a half-formed disk loc (if this asserts).
            getDur().writingDiskLoc(idx.info) = loc;

            try {
                getDur().writingInt(tableToIndex->indexBuildsInProgress) += 1;
                buildAnIndex(tabletoidxns, tableToIndex, idx, background, mayInterrupt);
            }
            catch (DBException& e) {
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

                // Recalculate the index # so we can remove it from the list in the next catch
                idxNo = IndexBuildsInProgress::get(tabletoidxns.c_str(), idxName);
                // roll back this index
                idx.kill_idx();

                verify(le && !saveerrmsg.empty());
                setLastError(savecode,saveerrmsg.c_str());
                throw;
            }

            // Recompute index numbers
            tableToIndex = nsdetails(tabletoidxns);
            idxNo = IndexBuildsInProgress::get(tabletoidxns.c_str(), idxName);
            verify(idxNo > -1);

            // Make sure the newly created index is relocated to nIndexes, if it isn't already there
            if (idxNo != tableToIndex->nIndexes) {
                log() << "switching indexes at position " << idxNo << " and "
                      << tableToIndex->nIndexes << endl;
                // We cannot use idx here, as it may point to a different index entry if it was
                // flipped during building
                IndexDetails temp = tableToIndex->idx(idxNo);
                *getDur().writing(&tableToIndex->idx(idxNo)) =
                    tableToIndex->idx(tableToIndex->nIndexes);
                *getDur().writing(&tableToIndex->idx(tableToIndex->nIndexes)) = temp;

                // We also have to flip multikey entries
                bool tempMultikey = tableToIndex->isMultikey(idxNo);
                tableToIndex->setIndexIsMultikey(tabletoidxns.c_str(), idxNo,
                                                 tableToIndex->isMultikey(tableToIndex->nIndexes));
                tableToIndex->setIndexIsMultikey(tabletoidxns.c_str(), tableToIndex->nIndexes,
                                                 tempMultikey);

                idxNo = tableToIndex->nIndexes;
            }

            // Store the current total of indexes in case something goes wrong actually adding the
            // index
            oldNIndexes = tableToIndex->getTotalIndexCount();

            // clear transient info caches so they refresh; increments nIndexes
            tableToIndex->addIndex(tabletoidxns.c_str());
            getDur().writingInt(tableToIndex->indexBuildsInProgress) -= 1;
        }
        catch (...) {
            // Generally, this will be called as an exception from building the index bubbles up.
            // Thus, the index will have already been cleaned up.  This catch just ensures that the
            // metadata is consistent on any exception. It may leak like a sieve if the index
            // successfully finished building and addIndex or kill_idx threw.

            // Check if nIndexes was incremented
            if (oldNIndexes != -1 && oldNIndexes != tableToIndex->nIndexes) {
                getDur().writingInt(tableToIndex->nIndexes) = oldNIndexes;
            }

            // Move any other in prog indexes "back" one. It is important that idxNo is set
            // correctly so that the correct index is removed
            IndexBuildsInProgress::remove(tabletoidxns.c_str(), idxNo);
            getDur().writingInt(tableToIndex->indexBuildsInProgress) -= 1;

            throw;
        }
    }

    // indexName is passed in because index details may not be pointing to something valid at this
    // point
    int IndexBuildsInProgress::get(const char* ns, const std::string& indexName) {
        Lock::assertWriteLocked(ns);
        NamespaceDetails* nsd = nsdetails(ns);

        // Go through unfinished index builds and try to find this index
        for (int i=nsd->nIndexes; i<nsd->nIndexes+nsd->indexBuildsInProgress; i++) {
            if (indexName == nsd->idx(i).indexName()) {
                return i;
            }
        }

        return -1;
    }

    void IndexBuildsInProgress::remove(const char* ns, int offset) {
        Lock::assertWriteLocked(ns);
        NamespaceDetails* nsd = nsdetails(ns);

        for (int i=offset; i<nsd->getTotalIndexCount(); i++) {
            if (i < NamespaceDetails::NIndexesMax-1) {
                *getDur().writing(&nsd->idx(i)) = nsd->idx(i+1);
                nsd->setIndexIsMultikey(ns, i, nsd->isMultikey(i+1));
            }
            else {
                *getDur().writing(&nsd->idx(i)) = IndexDetails();
                nsd->setIndexIsMultikey(ns, i, false);
            }
        }
    }

    DiskLoc DataFileMgr::insert(const char* ns,
                                const void* obuf,
                                int32_t len,
                                bool mayInterrupt,
                                bool god,
                                bool mayAddIndex,
                                bool* addedID) {
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
            if( !prepareToBuildIndex(io,
                                     mayInterrupt,
                                     god,
                                     tabletoidxns,
                                     tableToIndex,
                                     fixedIndexObject) ) {
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
            // we don't add _id for capped collections in local as they don't have an _id index
            if( idField.eoo() &&
                !wouldAddIndex &&
                nsToDatabase( ns ) != "local" &&
                d->haveIdIndex() ) {

                if( addedID )
                    *addedID = true;
                addID = len;
                idToInsert_.oid.init();
                len += idToInsert.size();
            }

            BSONElementManipulator::lookForTimestamps( io );
        }

        int lenWHdr = d->getRecordAllocationSize( len + Record::HeaderSize );
        fassert( 16440, lenWHdr >= ( len + Record::HeaderSize ) );
        
        // If the collection is capped, check if the new object will violate a unique index
        // constraint before allocating space.
        if (d->nIndexes && 
            d->isCapped() && 
            !god) {
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
                ((int&)*r->data()) = *((int*) obuf) + idToInsert.size();
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
            insert_makeIndex(tableToIndex, tabletoidxns, loc, mayInterrupt);
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

        massert( 16509,
                 str::stream()
                 << "fast_oplog_insert requires a capped collection "
                 << " but " << ns << " is not capped",
                 d->isCapped() );

        int lenWHdr = len + Record::HeaderSize;
        DiskLoc loc = d->alloc(ns, lenWHdr);
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

    void dropDatabase(const std::string& db) {
        LOG(1) << "dropDatabase " << db << endl;
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
                boostRenameWrapper( p, newPath_ / ( p.leaf().string() + ".bak" ) );
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
        string reservedPathString = reservedPath.string();

        bool res;
        {
            // clone to temp location, which effectively does repair
            Client::Context ctx( dbName, reservedPathString );
            verify( ctx.justCreated() );

            res = Cloner::cloneFrom(localhost.c_str(), errmsg, dbName,
                                    /*logForReplication=*/false, /*slaveOk*/false,
                                    /*replauth*/false, /*snapshot*/false, /*mayYield*/false,
                                    /*mayBeInterrupted*/true);
 
            Database::closeDatabase( dbName, reservedPathString.c_str() );
        }

        getDur().syncDataAndTruncateJournal(); // Must be done before and after repair
        MongoFile::flushAll(true); // need both in case journaling is disabled

        if ( !res ) {
            errmsg = str::stream() << "clone failed for " << dbName << " with error: " << errmsg;
            problem() << errmsg << endl;

            if ( !preserveClonedFilesOnFailure )
                MONGO_ASSERT_ON_EXCEPTION( boost::filesystem::remove_all( reservedPath ) );

            return false;
        }

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
            LOG(2) << fo.op() << " file " << q.string() << endl;
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
                    LOG(1) << fo.op() << " file " << q.string() << endl;
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
            LOG(2) << "DatabaseHolder::closeAll path:" << path << " name:" << name << endl;
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
