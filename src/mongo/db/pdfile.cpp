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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
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
#include <boost/optional/optional.hpp>
#include <list>

#include "mongo/base/counter.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/pdfile_private.h"
#include "mongo/db/background.h"
#include "mongo/db/btree.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/cloner.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop-inl.h"
#include "mongo/db/db.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/extsort.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/instance.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/memconcept.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/sort_phase_one.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/structure/collection.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/file.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/hashtab.h"
#include "mongo/util/mmap.h"
#include "mongo/util/processinfo.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/stats/counters.h"

namespace mongo {

    //The oplog entries inserted
    static TimerStats oplogInsertStats;
    static ServerStatusMetricField<TimerStats> displayInsertedOplogEntries(
                                                    "repl.oplog.insert",
                                                    &oplogInsertStats );
    static Counter64 oplogInsertBytesStats;
    static ServerStatusMetricField<Counter64> displayInsertedOplogEntryBytes(
                                                    "repl.oplog.insertBytes",
                                                    &oplogInsertBytesStats );

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

    /* ----------------------------------------- */
    const char FREELIST_NS[] = ".$freelist";
    string pidfilepath;

    DataFileMgr theDataFileMgr;
    DatabaseHolder _dbHolder;
    int MAGIC = 0x1000;

    DatabaseHolder& dbHolderUnchecked() {
        return _dbHolder;
    }

    void ensureIdIndexForNewNs( Collection* collection ) {
        if ( collection->ns().isSystem() && !legalClientSystemNS( collection->ns().ns(), false ) )
            return;

        uassertStatusOK( collection->getIndexCatalog()->ensureHaveIdIndex() );
    }

    string getDbContext() {
        stringstream ss;
        Client * c = currentClient.get();
        if ( c ) {
            Client::Context * cx = c->getContext();
            if ( cx ) {
                Database *database = cx->db();
                if ( database ) {
                    ss << database->name() << ' ';
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

    void _applyOpToDataFiles(const char *database, FileOp &fo, bool afterAllocator = false,
                             const string& path = storageGlobalParams.dbpath);

    void _deleteDataFiles(const char *database) {
        if (storageGlobalParams.directoryperdb) {
            FileAllocator::get()->waitUntilFinished();
            MONGO_ASSERT_ON_EXCEPTION_WITH_MSG(
                    boost::filesystem::remove_all(
                        boost::filesystem::path(storageGlobalParams.dbpath) / database),
                                                "delete data files with a directoryperdb");
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

    bool _userCreateNS(const char *ns, const BSONObj& options, string& err, bool *deferIdIndex) {
        LOG(1) << "create collection " << ns << ' ' << options << endl;

        Database* db = cc().database();

        Collection* collection = db->getCollection( ns );

        if ( collection ) {
            err = "collection already exists";
            return false;
        }

        long long size = Extent::initialSize(128);
        {
            BSONElement e = options.getField("size");
            if ( e.isNumber() ) {
                size = e.numberLong();
                uassert( 10083 , "create collection invalid size spec", size >= 0 );

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


        collection = db->createCollection( ns,
                                           options["capped"].trueValue(),
                                           &options,
                                           false ); // we do it ourselves below
        verify( collection );

        // $nExtents just for debug/testing.
        BSONElement e = options.getField( "$nExtents" );

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
                collection->increaseStorageSize( (int)size, false );
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
                collection->increaseStorageSize( (int)size, false );
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
                Extent* e = collection->increaseStorageSize( (int)desiredExtentSize, true );
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
                ensureIdIndexForNewNs( collection );
        }

        if ( mx > 0 )
            d->setMaxCappedDocs( mx );

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
        massert(10356 ,
                str::stream() << "invalid ns: " << ns,
                NamespaceString::validCollectionComponent(ns));
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

        DiskLoc loc = d->firstExtent();
        if ( loc.isNull() )
            return shared_ptr<Cursor>(new BasicCursor(DiskLoc()));
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
            Extent *e = d->lastExtent().ext();
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

    /* deletes a record, just the pdfile portion -- no index cleanup, no cursor cleanup, etc.
       caller must check if capped
    */
    void DataFileMgr::_deleteRecord(NamespaceDetails *d, const StringData& ns, Record *todelete, const DiskLoc& dl) {
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
            d->incrementStats( -1 * todelete->netLength(), -1 );

            if ( nsToCollectionSubstring(ns) == "system.indexes") {
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

    void DataFileMgr::deleteRecord(const char *ns, Record *todelete, const DiskLoc& dl,
                                   bool cappedOK, bool noWarn, bool doLog ) {
        deleteRecord( nsdetails(ns), ns, todelete, dl, cappedOK, noWarn, doLog );
    }

    void DataFileMgr::deleteRecord(NamespaceDetails* d, const StringData& ns, Record *todelete,
                                   const DiskLoc& dl, bool cappedOK, bool noWarn, bool doLog ) {
        dassert( todelete == dl.rec() );

        if ( d->isCapped() && !cappedOK ) {
            out() << "failing remove on a capped ns " << ns << endl;
            uassert( 10089 ,  "can't remove from a capped collection" , 0 );
            return;
        }

        BSONObj obj = BSONObj::make( todelete );

        BSONObj toDelete;
        if ( doLog ) {
            BSONElement e = obj["_id"];
            if ( e.type() ) {
                toDelete = e.wrap();
            }
        }
        Collection* collection = cc().database()->getCollection( ns );
        verify( collection );

        /* check if any cursors point to us.  if so, advance them. */
        ClientCursor::aboutToDelete(ns, d, dl);

        collection->getIndexCatalog()->unindexRecord( obj, dl, noWarn );

        _deleteRecord(d, ns, todelete, dl);

        collection->infoCache()->notifyOfWriteOp();

        if ( ! toDelete.isEmpty() ) {
            // TODO: this is crazy, need to fix logOp
            const char* raw = ns.rawData();
            if ( strlen(raw) == ns.size() ) {
                logOp( "d", raw, toDelete );
            }
            else {
                string temp = ns.toString();
                logOp( "d", temp.c_str(), toDelete );
            }
        }
    }

    Counter64 moveCounter;
    ServerStatusMetricField<Counter64> moveCounterDisplay( "record.moves", &moveCounter );

    /** Note: if the object shrinks a lot, we don't free up space, we leave extra at end of the record.
     */
    const DiskLoc DataFileMgr::updateRecord(
        const char *ns,
        Collection* collection,
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
        if (nsstring.coll() == "system.users") {
            V2UserDocumentParser parser;
            uassertStatusOK(parser.checkValidUserDocument(objNew));
        }

        uassert( 13596 , str::stream() << "cannot change _id of a document old:" << objOld << " new:" << objNew,
                objNew["_id"] == objOld["_id"]);

        /* duplicate key check. we descend the btree twice - once for this check, and once for the actual inserts, further
           below.  that is suboptimal, but it's pretty complicated to do it the other way without rollbacks...
        */
        OwnedPointerVector<UpdateTicket> updateTickets;
        updateTickets.mutableVector().resize(collection->details()->getTotalIndexCount());
        for (int i = 0; i < collection->details()->getTotalIndexCount(); ++i) {
            auto_ptr<IndexDescriptor> descriptor(CatalogHack::getDescriptor(collection->details(), i));
            auto_ptr<IndexAccessMethod> iam(CatalogHack::getIndex(descriptor.get()));
            InsertDeleteOptions options;
            options.logIfError = false;
            options.dupsAllowed = !(KeyPattern::isIdKeyPattern(descriptor->keyPattern())
                                    || descriptor->unique())
                                  || ignoreUniqueIndex(descriptor->getOnDisk());
            updateTickets.mutableVector()[i] = new UpdateTicket();
            Status ret = iam->validateUpdate(objOld, objNew, dl, options,
                                             updateTickets.mutableVector()[i]);

            if (Status::OK() != ret) {
                uasserted(ASSERT_ID_DUPKEY, "Update validation failed: " + ret.toString());
            }
        }

        if ( toupdate->netLength() < objNew.objsize() ) {
            // doesn't fit.  reallocate -----------------------------------------------------
            moveCounter.increment();
            uassert( 10003,
                     "failing update: objects in a capped ns cannot grow",
                     !(collection && collection->details()->isCapped()));
            collection->details()->paddingTooSmall();
            deleteRecord(ns, toupdate, dl);
            DiskLoc res = insert(ns, objNew.objdata(), objNew.objsize(), false, god);

            if (debug.nmoved == -1) // default of -1 rather than 0
                debug.nmoved = 1;
            else
                debug.nmoved += 1;

            return res;
        }

        collection->infoCache()->notifyOfWriteOp();
        collection->details()->paddingFits();

        debug.keyUpdates = 0;

        for (int i = 0; i < collection->details()->getTotalIndexCount(); ++i) {
            auto_ptr<IndexDescriptor> descriptor(CatalogHack::getDescriptor(collection->details(), i));
            auto_ptr<IndexAccessMethod> iam(CatalogHack::getIndex(descriptor.get()));
            int64_t updatedKeys;
            Status ret = iam->update(*updateTickets.vector()[i], &updatedKeys);
            if (Status::OK() != ret) {
                // This shouldn't happen unless something disastrous occurred.
                massert(16799, "update failed: " + ret.toString(), false);
            }
            debug.keyUpdates += updatedKeys;
        }

        //  update in place
        int sz = objNew.objsize();
        memcpy(getDur().writingPtr(toupdate->data(), sz), objNew.objdata(), sz);
        return dl;
    }

#pragma pack(1)
    struct IDToInsert {
        char type;
        char id[4];
        OID oid;

        IDToInsert() {
            type = 0;
        }

        bool needed() const { return type > 0; }

        void init() {
            type = static_cast<char>(jstOID);
            strcpy( id, "_id" );
            oid.init();
            verify( size() == 17 );
        }

        int size() const { return sizeof( IDToInsert ); }

        const char* rawdata() const { return reinterpret_cast<const char*>( this ); }
    };
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
        if ( d->isCapped() ) {
            // size capped doesn't grow
            return loc;
        }

        LOG(1) << "allocating new extent for " << ns << " padding:" << d->paddingFactor() << " lenWHdr: " << lenWHdr << endl;

        Collection* collection = cc().database()->getCollection( ns );
        verify( collection );

        collection->increaseStorageSize( Extent::followupSize(lenWHdr, d->lastExtentSize()), !god );

        loc = d->alloc(ns, lenWHdr);
        if ( !loc.isNull() ) {
            // got on first try
            return loc;
        }

        log() << "warning: alloc() failed after allocating new extent. "
              << "lenWHdr: " << lenWHdr << " last extent size:"
              << d->lastExtentSize() << "; trying again" << endl;

        for ( int z = 0; z < 10 && lenWHdr > d->lastExtentSize(); z++ ) {
            log() << "try #" << z << endl;
            collection->increaseStorageSize( Extent::followupSize(lenWHdr, d->lastExtentSize()), !god);

            loc = d->alloc(ns, lenWHdr);
            if ( ! loc.isNull() )
                break;
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
            if (nsToCollectionSubstring(ns) == "system.indexes")
                wouldAddIndex = true;
            else if ( legalClientSystemNS( ns , true ) ) {
                if ( obuf &&
                        StringData(ns) == StringData(".system.users", StringData::LiteralTag()) ) {
                    BSONObj t( reinterpret_cast<const char *>( obuf ) );
                    V2UserDocumentParser parser;
                    uassertStatusOK(parser.checkValidUserDocument(t));
                }
            }
            else if ( !god ) {
                uasserted(16459, str::stream() << "attempt to insert in system namespace '"
                                               << ns << "'");
            }
        }
        return true;
    }

    DiskLoc DataFileMgr::insert(const char* ns,
                                const void* obuf,
                                int32_t len,
                                bool mayInterrupt,
                                bool god,
                                bool mayAddIndex,
                                bool* addedID) {

        Database* database = cc().database();

        bool wouldAddIndex = false;
        massert( 10093 , "cannot insert into reserved $ collection", god || NamespaceString::normal( ns ) );
        uassert( 10094 , str::stream() << "invalid ns: " << ns , isValidNS( ns ) );
        {
            const char *sys = strstr(ns, "system.");
            if ( sys ) {

                if ( !insert_checkSys(sys, ns, wouldAddIndex, obuf, god) )
                    return DiskLoc();

                if ( mayAddIndex && wouldAddIndex ) {
                    // TODO: this should be handled above this function
                    BSONObj spec( static_cast<const char*>( obuf ) );
                    string collectionToIndex = spec.getStringField( "ns" );
                    uassert(10096, "invalid ns to index", collectionToIndex.find( '.' ) != string::npos);
                    massert(10097,
                            str::stream() << "trying to create index on wrong db "
                            << " db: " << database->name() << " collection: " << collectionToIndex,
                            database->ownsNS( collectionToIndex ) );

                    Collection* collection = database->getCollection( collectionToIndex );
                    if ( !collection ) {
                        collection = database->createCollection( collectionToIndex, false, NULL, true );
                        verify( collection );
                        if ( !god )
                            ensureIdIndexForNewNs( collection );
                    }

                    Status status = collection->getIndexCatalog()->createIndex( spec, mayInterrupt );
                    if ( status.code() == ErrorCodes::IndexAlreadyExists )
                        return DiskLoc();
                    uassertStatusOK( status );
                    return DiskLoc();
                }
            }
        }

        Collection* collection = database->getCollection( ns );
        if ( collection == NULL ) {
            collection = database->createCollection( ns, false, NULL, false );

            int ies = Extent::initialSize(len);
            if( str::contains(ns, '$') &&
                len + Record::HeaderSize >= BtreeData_V1::BucketSize - 256 &&
                len + Record::HeaderSize <= BtreeData_V1::BucketSize + 256 ) {
                // probably an index.  so we pick a value here for the first extent instead of using
                // initialExtentSize() which is more for user collections.
                // TODO: we could look at the # of records in the parent collection to be smarter here.
                ies = (32+4) * 1024;
            }
            collection->increaseStorageSize( ies, false);
            if ( !god )
                ensureIdIndexForNewNs( collection );
        }

        NamespaceDetails* d = collection->details();

        IDToInsert idToInsert; // only initialized if needed

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

                idToInsert.init();
                len += idToInsert.size();
            }

            BSONElementManipulator::lookForTimestamps( io );
        }

        int lenWHdr = d->getRecordAllocationSize( len + Record::HeaderSize );
        fassert( 16440, lenWHdr >= ( len + Record::HeaderSize ) );

        // If the collection is capped, check if the new object will violate a unique index
        // constraint before allocating space.
        if ( d->isCapped() && !god) {
            BSONObj temp = BSONObj( reinterpret_cast<const char *>( obuf ) );
            Status ret = collection->getIndexCatalog()->checkNoIndexConflicts( temp );
            uassert(12582, "duplicate key insert for unique index of capped collection", ret.isOK() );
        }

        DiskLoc loc = allocateSpaceForANewRecord(ns, d, lenWHdr, god);

        if ( loc.isNull() ) {
            log() << "insert: couldn't alloc space for object ns:" << ns
                  << " capped:" << d->isCapped() << endl;
            verify(d->isCapped());
            return DiskLoc();
        }

        Record *r = loc.rec();
        {
            verify( r->lengthWithHeaders() >= lenWHdr );
            r = (Record*) getDur().writingPtr(r, lenWHdr);
            if( idToInsert.needed() ) {
                /* a little effort was made here to avoid a double copy when we add an ID */
                int originalSize = *((int*) obuf);
                ((int&)*r->data()) = originalSize + idToInsert.size();
                memcpy(r->data()+4, idToInsert.rawdata(), idToInsert.size());
                memcpy(r->data()+4+idToInsert.size(), ((char*)obuf)+4, originalSize-4);
            }
            else {
                if( obuf ) // obuf can be null from internal callers
                    memcpy(r->data(), obuf, len);
            }
        }

        addRecordToRecListInExtent(r, loc);

        d->incrementStats( r->netLength(), 1 );

        // we don't bother resetting query optimizer stats for the god tables - also god is true when adding a btree bucket
        if ( !god )
            collection->infoCache()->notifyOfWriteOp();

        /* add this record to our indexes */
        if ( d->getTotalIndexCount() > 0 ) {
            try {
                BSONObj obj(r->data());
                collection->getIndexCatalog()->indexRecord(obj, loc);
            }
            catch( AssertionException& e ) {
                // should be a dup key error on _id index
                if( d->isCapped() ) {
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

        //record timing on oplog inserts
        boost::optional<TimerHolder> insertTimer;
        //skip non-oplog collections
        if (NamespaceString::oplog(ns)) {
            insertTimer = boost::in_place(&oplogInsertStats);
            oplogInsertBytesStats.increment(len); //record len of inserted records for oplog
        }

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

        d->incrementStats( r->netLength(), 1 );
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
        verify( d->name() == db );

        BackgroundOperation::assertNoBgOpInProgForDb(d->name().c_str());

        audit::logDropDatabase( currentClient.get(), db );

        // Not sure we need this here, so removed.  If we do, we need to move it down
        // within other calls both (1) as they could be called from elsewhere and
        // (2) to keep the lock order right - groupcommitmutex must be locked before
        // mmmutex (if both are locked).
        //
        //  RWLockRecursive::Exclusive lk(MongoFile::mmmutex);

        getDur().syncDataAndTruncateJournal();

        Database::closeDatabase( d->name(), d->path() );
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
        if (storageGlobalParams.directoryperdb)
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
        Path newPath(storageGlobalParams.dbpath);
        if (storageGlobalParams.directoryperdb)
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
        Path repairPath = Path(storageGlobalParams.repairpath);
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
        ss << "localhost:" << serverGlobalParams.port;
        string localhost = ss.str();

        problem() << "repairDatabase " << dbName << endl;
        verify( cc().database()->name() == dbName );
        verify(cc().database()->path() == storageGlobalParams.dbpath);

        BackgroundOperation::assertNoBgOpInProgForDb(dbName);

        getDur().syncDataAndTruncateJournal(); // Must be done before and after repair

        boost::intmax_t totalSize = dbSize( dbName );
        boost::intmax_t freeSize = File::freeSpace(storageGlobalParams.repairpath);
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
        Database::closeDatabase(dbName, storageGlobalParams.dbpath);

        if ( backupOriginalFiles ) {
            _renameForBackup( dbName, reservedPath );
        }
        else {
            _deleteDataFiles( dbName );
            MONGO_ASSERT_ON_EXCEPTION(
                    boost::filesystem::create_directory(Path(storageGlobalParams.dbpath) / dbName));
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
        if (storageGlobalParams.directoryperdb)
            p /= database;
        boost::filesystem::path q;
        q = p / (c+"ns");
        bool ok = false;
        MONGO_ASSERT_ON_EXCEPTION( ok = fo.apply( q ) );
        if ( ok ) {
            LOG(2) << fo.op() << " file " << q.string() << endl;
        }
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

} // namespace mongo
