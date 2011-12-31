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
     database.ns - namespace index
     database.1  - data files
     database.2
     ...
*/

#pragma once

#include "../pch.h"
#include "../util/mmap.h"
#include "diskloc.h"
#include "jsobjmanipulator.h"
#include "namespace-inl.h"
#include "client.h"
#include "mongommf.h"
#include "memconcept.h"

namespace mongo {

    class DataFileHeader;
    class Extent;
    class Record;
    class Cursor;
    class OpDebug;

    void dropDatabase(string db);
    bool repairDatabase(string db, string &errmsg, bool preserveClonedFilesOnFailure = false, bool backupOriginalFiles = false);

    /* low level - only drops this ns */
    void dropNS(const string& dropNs);

    /* deletes this ns, indexes and cursors */
    void dropCollection( const string &name, string &errmsg, BSONObjBuilder &result );
    bool userCreateNS(const char *ns, BSONObj j, string& err, bool logForReplication, bool *deferIdIndex = 0);
    shared_ptr<Cursor> findTableScan(const char *ns, const BSONObj& order, const DiskLoc &startLoc=DiskLoc());

    bool isValidNS( const StringData& ns );

    /*---------------------------------------------------------------------*/

    class MongoDataFile {
        friend class DataFileMgr;
        friend class BasicCursor;
    public:
        MongoDataFile(int fn) : _mb(0), fileNo(fn) { }

        /** @return true if found and opened. if uninitialized (prealloc only) does not open. */
        bool openExisting( const char *filename );

        /** creates if DNE */
        void open(const char *filename, int requestedDataSize = 0, bool preallocateOnly = false);

        /* allocate a new extent from this datafile.
           @param capped - true if capped collection
           @param loops is our recursion check variable - you want to pass in zero
        */
        Extent* createExtent(const char *ns, int approxSize, bool capped = false, int loops = 0);

        DataFileHeader *getHeader() { return header(); }

        unsigned long long length() const { return mmf.length(); }

        /* return max size an extent may be */
        static int maxSize();

        /** fsync */
        void flush( bool sync );

        /** only use fore debugging */
        Extent* debug_getExtent(DiskLoc loc) { return _getExtent( loc ); }
    private:
        void badOfs(int) const;
        void badOfs2(int) const;
        int defaultSize( const char *filename ) const;

        Extent* getExtent(DiskLoc loc) const;
        Extent* _getExtent(DiskLoc loc) const;
        Record* recordAt(DiskLoc dl);
        Record* makeRecord(DiskLoc dl, int size);
        void grow(DiskLoc dl, int size);

        char* p() const { return (char *) _mb; }
        DataFileHeader* header() { return (DataFileHeader*) _mb; }

        MongoMMF mmf;
        void *_mb; // the memory mapped view
        int fileNo;
    };

    class DataFileMgr {
        friend class BasicCursor;
    public:
        void init(const string& path );

        /* see if we can find an extent of the right size in the freelist. */
        static Extent* allocFromFreeList(const char *ns, int approxSize, bool capped = false);

        /** @return DiskLoc where item ends up */
        // changedId should be initialized to false
        const DiskLoc updateRecord(
            const char *ns,
            NamespaceDetails *d,
            NamespaceDetailsTransient *nsdt,
            Record *toupdate, const DiskLoc& dl,
            const char *buf, int len, OpDebug& debug, bool god=false);

        // The object o may be updated if modified on insert.
        void insertAndLog( const char *ns, const BSONObj &o, bool god = false );

        /** insert will add an _id to the object if not present.  if you would like to see the final object
            after such an addition, use this method.
            @param o both and in and out param 
            */
        DiskLoc insertWithObjMod(const char *ns, BSONObj & /*out*/o, bool god = false);

        /** @param obj in value only for this version. */
        void insertNoReturnVal(const char *ns, BSONObj o, bool god = false);

        DiskLoc insert(const char *ns, const void *buf, int len, bool god = false, bool mayAddIndex = true, bool *addedID = 0);
        static shared_ptr<Cursor> findAll(const char *ns, const DiskLoc &startLoc = DiskLoc());

        /* special version of insert for transaction logging -- streamlined a bit.
           assumes ns is capped and no indexes
           no _id field check
        */
        Record* fast_oplog_insert(NamespaceDetails *d, const char *ns, int len);

        static Extent* getExtent(const DiskLoc& dl);
        static Record* getRecord(const DiskLoc& dl);
        static DeletedRecord* makeDeletedRecord(const DiskLoc& dl, int len);

        void deleteRecord(const char *ns, Record *todelete, const DiskLoc& dl, bool cappedOK = false, bool noWarn = false, bool logOp=false);

        /* does not clean up indexes, etc. : just deletes the record in the pdfile. use deleteRecord() to unindex */
        void _deleteRecord(NamespaceDetails *d, const char *ns, Record *todelete, const DiskLoc& dl);

    private:
        vector<MongoDataFile *> files;
    };

    extern DataFileMgr theDataFileMgr;

#pragma pack(1)

    class DeletedRecord {
    public:
        int lengthWithHeaders;
        int extentOfs;
        DiskLoc nextDeleted;
        DiskLoc myExtentLoc(const DiskLoc& myLoc) const {
            return DiskLoc(myLoc.a(), extentOfs);
        }
        Extent* myExtent(const DiskLoc& myLoc) {
            return DataFileMgr::getExtent(DiskLoc(myLoc.a(), extentOfs));
        }
    };

    /* Record is a record in a datafile.  DeletedRecord is similar but for deleted space.

    *11:03:20 AM) dm10gen: regarding extentOfs...
    (11:03:42 AM) dm10gen: an extent is a continugous disk area, which contains many Records and DeleteRecords
    (11:03:56 AM) dm10gen: a DiskLoc has two pieces, the fileno and ofs.  (64 bit total)
    (11:04:16 AM) dm10gen: to keep the headesr small, instead of storing a 64 bit ptr to the full extent address, we keep just the offset
    (11:04:29 AM) dm10gen: we can do this as we know the record's address, and it has the same fileNo
    (11:04:33 AM) dm10gen: see class DiskLoc for more info
    (11:04:43 AM) dm10gen: so that is how Record::myExtent() works
    (11:04:53 AM) dm10gen: on an alloc(), when we build a new Record, we must populate its extentOfs then
    */
    class Record {
    public:
        enum HeaderSizeValue { HeaderSize = 16 };
        int lengthWithHeaders;
        int extentOfs;
        int nextOfs;
        int prevOfs;

        /** be careful when referencing this that your write intent was correct */
        char data[4];

        int netLength() {
            return lengthWithHeaders - HeaderSize;
        }
        //void setNewLength(int netlen) { lengthWithHeaders = netlen + HeaderSize; }

        /* use this when a record is deleted. basically a union with next/prev fields */
        DeletedRecord& asDeleted() { return *((DeletedRecord*) this); }

        Extent* myExtent(const DiskLoc& myLoc) { return DataFileMgr::getExtent(DiskLoc(myLoc.a(), extentOfs)); }

        /* get the next record in the namespace, traversing extents as necessary */
        DiskLoc getNext(const DiskLoc& myLoc);
        DiskLoc getPrev(const DiskLoc& myLoc);

        DiskLoc nextInExtent(const DiskLoc& myLoc) { 
            if ( nextOfs == DiskLoc::NullOfs )
                return DiskLoc();
            assert( nextOfs );
            return DiskLoc(myLoc.a(), nextOfs);
        }

        struct NP {
            int nextOfs;
            int prevOfs;
        };
        NP* np() { return (NP*) &nextOfs; }

        // ---------------------
        // memory cache
        // ---------------------

        /** 
         * touches the data so that is in physical memory
         * @param entireRecrd if false, only the header and first byte is touched
         *                    if true, the entire record is touched
         * */
        void touch( bool entireRecrd = false );

        /**
         * @return if this record is likely in physical memory
         *         its not guaranteed because its possible it gets swapped out in a very unlucky windows
         */
        bool likelyInPhysicalMemory();

        /**
         * tell the cache this Record was accessed
         * @return this, for simple chaining
         */
        Record* accessed();

        static bool MemoryTrackingEnabled;
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

        /* which namespace this extent is for.  this is just for troubleshooting really
           and won't even be correct if the collection were renamed!
        */
        Namespace nsDiagnostic;

        int length;   /* size of the extent, including these fields */
        DiskLoc firstRecord;
        DiskLoc lastRecord;
        char _extentData[4];

        static int HeaderSize() { return sizeof(Extent)-4; }

        bool validates() {
            return !(firstRecord.isNull() ^ lastRecord.isNull()) &&
                   length >= 0 && !myLoc.isNull();
        }

        BSONObj dump() {
            return BSON( "loc" << myLoc.toString() << "xnext" << xnext.toString() << "xprev" << xprev.toString()
                      << "nsdiag" << nsDiagnostic.toString()
                      << "size" << length << "firstRecord" << firstRecord.toString() << "lastRecord" << lastRecord.toString());
        }

        void dump(iostream& s) {
            s << "    loc:" << myLoc.toString() << " xnext:" << xnext.toString() << " xprev:" << xprev.toString() << '\n';
            s << "    nsdiag:" << nsDiagnostic.toString() << '\n';
            s << "    size:" << length << " firstRecord:" << firstRecord.toString() << " lastRecord:" << lastRecord.toString() << '\n';
        }

        /* assumes already zeroed -- insufficient for block 'reuse' perhaps
        Returns a DeletedRecord location which is the data in the extent ready for us.
        Caller will need to add that to the freelist structure in namespacedetail.
        */
        DiskLoc init(const char *nsname, int _length, int _fileNo, int _offset, bool capped);

        /* like init(), but for a reuse case */
        DiskLoc reuse(const char *nsname, bool newUseIsAsCapped);

        bool isOk() const { return magic == 0x41424344; }
        void assertOk() const { assert(isOk()); }

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

        static int maxSize();
        static int minSize() { return 0x100; }
        /**
         * @param len lengt of record we need
         * @param lastRecord size of last extent which is a factor in next extent size
         */
        static int followupSize(int len, int lastExtentLen);

        /** get a suggested size for the first extent in a namespace
         *  @param len length of record we need to insert
         */
        static int initialSize(int len);

        struct FL {
            DiskLoc firstRecord;
            DiskLoc lastRecord;
        };
        /** often we want to update just the firstRecord and lastRecord fields.
            this helper is for that -- for use with getDur().writing() method
        */
        FL* fl() { return (FL*) &firstRecord; }

        /** caller must declare write intent first */
        void markEmpty();
    private:
        DiskLoc _reuse(const char *nsname, bool newUseIsAsCapped); // recycle an extent and reuse it for a different ns
    };

    /*  a datafile - i.e. the "dbname.<#>" files :

          ----------------------
          DataFileHeader
          ----------------------
          Extent (for a particular namespace)
            Record
            ...
            Record (some chained for unused space)
          ----------------------
          more Extents...
          ----------------------
    */
    class DataFileHeader {
    public:
        int version;
        int versionMinor;
        int fileLength;
        DiskLoc unused; /* unused is the portion of the file that doesn't belong to any allocated extents. -1 = no more */
        int unusedLength;
        char reserved[8192 - 4*4 - 8];

        char data[4]; // first extent starts here

        enum { HeaderSize = 8192 };

        bool isCurrentVersion() const { return ( version == PDFILE_VERSION ) && ( versionMinor == PDFILE_VERSION_MINOR ); }

        bool uninitialized() const { return version == 0; }

        void init(int fileno, int filelength, const char* filename) {
            if ( uninitialized() ) {
                DEV log() << "datafileheader::init initializing " << filename << " n:" << fileno << endl;
                if( !(filelength > 32768 ) ) { 
                    massert(13640, str::stream() << "DataFileHeader looks corrupt at file open filelength:" << filelength << " fileno:" << fileno, false);
                }

                { 
                    if( !d.dbMutex.isWriteLocked() ) { 
                        log() << "*** TEMP NOT INITIALIZING FILE " << filename << ", not in a write lock." << endl;
                        log() << "temp bypass until more elaborate change - case that is manifesting is benign anyway" << endl;
                        return;
/**
                        log() << "ERROR can't create outside a write lock" << endl;
                        printStackTrace();
                        ::abort();
**/
                    }
                }

                getDur().createdFile(filename, filelength);
                assert( HeaderSize == 8192 );
                DataFileHeader *h = getDur().writing(this);
                h->fileLength = filelength;
                h->version = PDFILE_VERSION;
                h->versionMinor = PDFILE_VERSION_MINOR;
                h->unused.set( fileno, HeaderSize );
                assert( (data-(char*)this) == HeaderSize );
                h->unusedLength = fileLength - HeaderSize - 16;
            }
        }

        bool isEmpty() const {
            return uninitialized() || ( unusedLength == fileLength - HeaderSize - 16 );
        }
    };

#pragma pack()

    inline Extent* MongoDataFile::_getExtent(DiskLoc loc) const {
        loc.assertOk();
        Extent *e = (Extent *) (p()+loc.getOfs());
        return e;
    }

    inline Extent* MongoDataFile::getExtent(DiskLoc loc) const {
        Extent *e = _getExtent(loc);
        e->assertOk();
        memconcept::is(e, memconcept::extent);
        return e;
    }

} // namespace mongo

#include "cursor.h"

namespace mongo {

    inline Record* MongoDataFile::recordAt(DiskLoc dl) {
        int ofs = dl.getOfs();
        if( ofs < DataFileHeader::HeaderSize ) badOfs(ofs); // will uassert - external call to keep out of the normal code path
        return (Record*) (p()+ofs);
    }

    inline Record* MongoDataFile::makeRecord(DiskLoc dl, int size) {
        int ofs = dl.getOfs();
        if( ofs < DataFileHeader::HeaderSize ) badOfs(ofs); // will uassert - external call to keep out of the normal code path
        return (Record*) (p()+ofs);
    }

    inline DiskLoc Record::getNext(const DiskLoc& myLoc) {
        if ( nextOfs != DiskLoc::NullOfs ) {
            /* defensive */
            if ( nextOfs >= 0 && nextOfs < 10 ) {
                sayDbContext("Assertion failure - Record::getNext() referencing a deleted record?");
                return DiskLoc();
            }

            return DiskLoc(myLoc.a(), nextOfs);
        }
        Extent *e = myExtent(myLoc);
        while ( 1 ) {
            if ( e->xnext.isNull() )
                return DiskLoc(); // end of table.
            e = e->xnext.ext();
            if ( !e->firstRecord.isNull() )
                break;
            // entire extent could be empty, keep looking
        }
        return e->firstRecord;
    }
    inline DiskLoc Record::getPrev(const DiskLoc& myLoc) {
        if ( prevOfs != DiskLoc::NullOfs )
            return DiskLoc(myLoc.a(), prevOfs);
        Extent *e = myExtent(myLoc);
        if ( e->xprev.isNull() )
            return DiskLoc();
        return e->xprev.ext()->lastRecord;
    }

    inline BSONObj DiskLoc::obj() const {
        return BSONObj(rec()->accessed());
    }
    inline DeletedRecord* DiskLoc::drec() const {
        assert( _a != -1 );
        DeletedRecord* dr = (DeletedRecord*) rec();
        memconcept::is(dr, memconcept::deletedrecord);
        return dr;
    }
    inline Extent* DiskLoc::ext() const {
        return DataFileMgr::getExtent(*this);
    }

    template< class V >
    inline 
    const BtreeBucket<V> * DiskLoc::btree() const {
        assert( _a != -1 );
        return (const BtreeBucket<V> *) rec()->data;
    }

} // namespace mongo

#include "database.h"
#include "memconcept.h"

namespace mongo {

    boost::intmax_t dbSize( const char *database );

    inline NamespaceIndex* nsindex(const char *ns) {
        Database *database = cc().database();
        assert( database );
        memconcept::is(database, memconcept::database, ns, sizeof(Database));
        DEV {
            char buf[256];
            nsToDatabase(ns, buf);
            if ( database->name != buf ) {
                out() << "ERROR: attempt to write to wrong database\n";
                out() << " ns:" << ns << '\n';
                out() << " database->name:" << database->name << endl;
                assert( database->name == buf );
            }
        }
        return &database->namespaceIndex;
    }

    inline NamespaceDetails* nsdetails(const char *ns) {
        // if this faults, did you set the current db first?  (Client::Context + dblock)
        NamespaceDetails *d = nsindex(ns)->details(ns);
        if( d ) {
            memconcept::is(d, memconcept::nsdetails, ns, sizeof(NamespaceDetails));
        }
        return d;
    }

    inline Extent* DataFileMgr::getExtent(const DiskLoc& dl) {
        assert( dl.a() != -1 );
        return cc().database()->getFile(dl.a())->getExtent(dl);
    }

    inline Record* DataFileMgr::getRecord(const DiskLoc& dl) {
        assert( dl.a() != -1 );
        Record* r = cc().database()->getFile(dl.a())->recordAt(dl);
        return r;
    }

    BOOST_STATIC_ASSERT( 16 == sizeof(DeletedRecord) );

    inline DeletedRecord* DataFileMgr::makeDeletedRecord(const DiskLoc& dl, int len) {
        assert( dl.a() != -1 );
        return (DeletedRecord*) cc().database()->getFile(dl.a())->makeRecord(dl, sizeof(DeletedRecord));
    }

    void ensureHaveIdIndex(const char *ns);

    bool dropIndexes( NamespaceDetails *d, const char *ns, const char *name, string &errmsg, BSONObjBuilder &anObjBuilder, bool maydeleteIdIndex );

    inline BSONObj::BSONObj(const Record *r) {
        init(r->data);
    }

} // namespace mongo
