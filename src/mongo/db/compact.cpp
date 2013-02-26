/** @file compact.cpp
   compaction of deleted space in pdfiles (datafiles)
*/

/**
*    Copyright (C) 2010 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,b
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/background.h"
#include "mongo/db/commands.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/curop-inl.h"
#include "mongo/db/extsort.h"
#include "mongo/db/index.h"
#include "mongo/db/index_update.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/sort_phase_one.h"
#include "mongo/util/concurrency/task.h"
#include "mongo/util/timer.h"
#include "mongo/util/touch_pages.h"

namespace mongo {

    void freeExtents(DiskLoc firstExt, DiskLoc lastExt);

    /* this should be done in alloc record not here, but doing here for now. 
       really dumb; it's a start.
    */
    unsigned quantizeMask(unsigned x) { 
        if( x > 4096 * 20 ) 
            return ~4095;
        if( x >= 512 ) 
            return ~63;
        return ~0;
    }

    /** @return number of skipped (invalid) documents */
    unsigned compactExtent(const char *ns, NamespaceDetails *d, const DiskLoc diskloc, int n,
                const scoped_array<IndexSpec> &indexSpecs,
                scoped_array<SortPhaseOne>& phase1, int nidx, bool validate, 
                double pf, int pb)
    {
        log() << "compact begin extent #" << n << " for namespace " << ns << endl;
        unsigned oldObjSize = 0; // we'll report what the old padding was
        unsigned oldObjSizeWithPadding = 0;

        Extent *e = diskloc.ext();
        e->assertOk();
        verify( e->validates(diskloc) );
        unsigned skipped = 0;

        {
            // the next/prev pointers within the extent might not be in order so we first page the whole thing in 
            // sequentially
            log() << "compact paging in len=" << e->length/1000000.0 << "MB" << endl;
            Timer t;
            MongoDataFile* mdf = cc().database()->getFile( diskloc.a() );
            HANDLE fd = mdf->getFd();
            int offset = diskloc.getOfs();
            Extent* ext = diskloc.ext();
            size_t length = ext->length;
                
            touch_pages(fd, offset, length, ext);
            int ms = t.millis();
            if( ms > 1000 ) 
                log() << "compact end paging in " << ms << "ms " << e->length/1000000.0/ms << "MB/sec" << endl;
        }

        {
            log() << "compact copying records" << endl;
            long long datasize = 0;
            long long nrecords = 0;
            DiskLoc L = e->firstRecord;
            if( !L.isNull() ) {
                while( 1 ) {
                    Record *recOld = L.rec();
                    L = recOld->nextInExtent(L);
                    BSONObj objOld = BSONObj::make(recOld);

                    if( !validate || objOld.valid() ) {
                        nrecords++;
                        unsigned sz = objOld.objsize();

                        oldObjSize += sz;
                        oldObjSizeWithPadding += recOld->netLength();

                        unsigned lenWHdr = sz + Record::HeaderSize;
                        unsigned lenWPadding = lenWHdr;
                        {
                            lenWPadding = static_cast<unsigned>(pf*lenWPadding);
                            lenWPadding += pb;
                            lenWPadding = lenWPadding & quantizeMask(lenWPadding);
                            if( lenWPadding < lenWHdr || lenWPadding > BSONObjMaxUserSize / 2 ) { 
                                lenWPadding = lenWHdr;
                            }
                        }
                        DiskLoc loc = allocateSpaceForANewRecord(ns, d, lenWPadding, false);
                        uassert(14024, "compact error out of space during compaction", !loc.isNull());
                        Record *recNew = loc.rec();
                        datasize += recNew->netLength();
                        recNew = (Record *) getDur().writingPtr(recNew, lenWHdr);
                        addRecordToRecListInExtent(recNew, loc);
                        memcpy(recNew->data(), objOld.objdata(), sz);

                        {
                            // extract keys for all indexes we will be rebuilding
                            for( int x = 0; x < nidx; x++ ) { 
                                phase1[x].addKeys(indexSpecs[x], objOld, loc, false);
                            }
                        }
                    }
                    else { 
                        if( ++skipped <= 10 )
                            log() << "compact skipping invalid object" << endl;
                    }

                    if( L.isNull() ) { 
                        // we just did the very last record from the old extent.  it's still pointed to 
                        // by the old extent ext, but that will be fixed below after this loop
                        break;
                    }

                    // remove the old records (orphan them) periodically so our commit block doesn't get too large
                    bool stopping = false;
                    RARELY stopping = *killCurrentOp.checkForInterruptNoAssert() != 0;
                    if( stopping || getDur().aCommitIsNeeded() ) {
                        e->firstRecord.writing() = L;
                        Record *r = L.rec();
                        getDur().writingInt(r->prevOfs()) = DiskLoc::NullOfs;
                        getDur().commitIfNeeded();
                        killCurrentOp.checkForInterrupt(false);
                    }
                }
            } // if !L.isNull()

            verify( d->firstExtent == diskloc );
            verify( d->lastExtent != diskloc );
            DiskLoc newFirst = e->xnext;
            d->firstExtent.writing() = newFirst;
            newFirst.ext()->xprev.writing().Null();
            getDur().writing(e)->markEmpty();
            freeExtents( diskloc, diskloc );
            // update datasize/record count for this namespace's extent
            {
                NamespaceDetails::Stats *s = getDur().writing(&d->stats);
                s->datasize += datasize;
                s->nrecords += nrecords;
            }

            getDur().commitIfNeeded();

            { 
                double op = 1.0;
                if( oldObjSize ) 
                    op = static_cast<double>(oldObjSizeWithPadding)/oldObjSize;
                log() << "compact finished extent #" << n << " containing " << nrecords << " documents (" << datasize/1000000.0 << "MB)"
                    << " oldPadding: " << op << ' ' << static_cast<unsigned>(op*100.0)/100
                    << endl;                    
            }
        }

        return skipped;
    }

    bool _compact(const char *ns, NamespaceDetails *d, string& errmsg, bool validate, BSONObjBuilder& result, double pf, int pb) { 
        // this is a big job, so might as well make things tidy before we start just to be nice.
        getDur().commitIfNeeded();

        list<DiskLoc> extents;
        for( DiskLoc L = d->firstExtent; !L.isNull(); L = L.ext()->xnext ) 
            extents.push_back(L);
        log() << "compact " << extents.size() << " extents" << endl;

        ProgressMeterHolder pm(cc().curop()->setMessage("compact extent",
                                                        "Extent Compacting Progress",
                                                        extents.size()));

        // same data, but might perform a little different after compact?
        NamespaceDetailsTransient::get(ns).clearQueryCache();

        int nidx = d->nIndexes;
        scoped_array<IndexSpec> indexSpecs( new IndexSpec[nidx] );
        scoped_array<SortPhaseOne> phase1( new SortPhaseOne[nidx] );
        {
            NamespaceDetails::IndexIterator ii = d->ii(); 
            // For each existing index...
            for( int idxNo = 0; ii.more(); ++idxNo ) {
                // Build a new index spec based on the old index spec.
                BSONObjBuilder b;
                BSONObj::iterator i(ii.next().info.obj());
                while( i.more() ) { 
                    BSONElement e = i.next();
                    if ( str::equals( e.fieldName(), "v" ) ) {
                        // Drop any preexisting index version spec.  The default index version will
                        // be used instead for the new index.
                        continue;
                    }
                    if ( str::equals( e.fieldName(), "background" ) ) {
                        // Create the new index in the foreground.
                        continue;
                    }
                    // Pass the element through to the new index spec.
                    b.append(e);
                }
                // Add the new index spec to 'indexSpecs'.
                BSONObj o = b.obj().getOwned();
                indexSpecs[idxNo].reset(o);
                // Create an external sorter.
                phase1[idxNo].sorter.reset
                        ( new BSONObjExternalSorter
                           // Use the default index interface, since the new index will be created
                           // with the default index version.
                         ( IndexInterface::defaultVersion(),
                           o.getObjectField("key") ) );
                phase1[idxNo].sorter->hintNumObjects( d->stats.nrecords );
            }
        }

        log() << "compact orphan deleted lists" << endl;
        for( int i = 0; i < Buckets; i++ ) { 
            d->deletedList[i].writing().Null();
        }



        // Start over from scratch with our extent sizing and growth
        d->lastExtentSize=0;

        // before dropping indexes, at least make sure we can allocate one extent!
        uassert(14025, "compact error no space available to allocate", !allocateSpaceForANewRecord(ns, d, Record::HeaderSize+1, false).isNull());

        // note that the drop indexes call also invalidates all clientcursors for the namespace, which is important and wanted here
        log() << "compact dropping indexes" << endl;
        BSONObjBuilder b;
        if( !dropIndexes(d, ns, "*", errmsg, b, true) ) { 
            errmsg = "compact drop indexes failed";
            log() << errmsg << endl;
            return false;
        }

        getDur().commitIfNeeded();

        long long skipped = 0;
        int n = 0;

        // reset data size and record counts to 0 for this namespace
        // as we're about to tally them up again for each new extent
        {
            NamespaceDetails::Stats *s = getDur().writing(&d->stats);
            s->datasize = 0;
            s->nrecords = 0;
        }

        for( list<DiskLoc>::iterator i = extents.begin(); i != extents.end(); i++ ) { 
            skipped += compactExtent(ns, d, *i, n++, indexSpecs, phase1, nidx, validate, pf, pb);
            pm.hit();
        }

        if( skipped ) {
            result.append("invalidObjects", skipped);
        }

        verify( d->firstExtent.ext()->xprev.isNull() );

        // indexes will do their own progress meter?
        pm.finished();

        // build indexes
        NamespaceString s(ns);
        string si = s.db + ".system.indexes";
        for( int i = 0; i < nidx; i++ ) {
            killCurrentOp.checkForInterrupt(false);
            BSONObj info = indexSpecs[i].info;
            log() << "compact create index " << info["key"].Obj().toString() << endl;
            scoped_lock precalcLock(theDataFileMgr._precalcedMutex);
            try {
                theDataFileMgr.setPrecalced(&phase1[i]);
                theDataFileMgr.insert(si.c_str(), info.objdata(), info.objsize());
            }
            catch(...) {
                theDataFileMgr.setPrecalced(NULL);
                throw;
            }
            theDataFileMgr.setPrecalced(NULL);
        }

        return true;
    }

    bool compact(const string& ns, string &errmsg, bool validate, BSONObjBuilder& result, double pf, int pb) {
        massert( 14028, "bad ns", NamespaceString::normal(ns.c_str()) );
        massert( 14027, "can't compact a system namespace", !str::contains(ns, ".system.") ); // items in system.indexes cannot be moved there are pointers to those disklocs in NamespaceDetails

        bool ok;
        {
            Lock::DBWrite lk(ns);
            BackgroundOperation::assertNoBgOpInProgForNs(ns.c_str());
            Client::Context ctx(ns);
            NamespaceDetails *d = nsdetails(ns);
            massert( 13660, str::stream() << "namespace " << ns << " does not exist", d );
            massert( 13661, "cannot compact capped collection", !d->isCapped() );
            log() << "compact " << ns << " begin" << endl;
            if( pf != 0 || pb != 0 ) { 
                log() << "paddingFactor:" << pf << " paddingBytes:" << pb << endl;
            } 
            try { 
                ok = _compact(ns.c_str(), d, errmsg, validate, result, pf, pb);
            }
            catch(...) { 
                log() << "compact " << ns << " end (with error)" << endl;
                throw;
            }
            log() << "compact " << ns << " end" << endl;
        }
        return ok;
    }

    bool isCurrentlyAReplSetPrimary();

    class CompactCmd : public Command {
    public:
        virtual LockType locktype() const { return NONE; }
        virtual bool adminOnly() const { return false; }
        virtual bool slaveOk() const { return true; }
        virtual bool maintenanceMode() const { return true; }
        virtual bool logTheOp() { return false; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::compact);
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }
        virtual void help( stringstream& help ) const {
            help << "compact collection\n"
                "warning: this operation blocks the server and is slow. you can cancel with cancelOp()\n"
                "{ compact : <collection_name>, [force:<bool>], [validate:<bool>],\n"
                "  [paddingFactor:<num>], [paddingBytes:<num>] }\n"
                "  force - allows to run on a replica set primary\n"
                "  validate - check records are noncorrupt before adding to newly compacting extents. slower but safer (defaults to true in this version)\n";
        }
        CompactCmd() : Command("compact") { }

        virtual bool run(const string& db, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string coll = cmdObj.firstElement().valuestr();
            if( coll.empty() || db.empty() ) {
                errmsg = "no collection name specified";
                return false;
            }

            if( isCurrentlyAReplSetPrimary() && !cmdObj["force"].trueValue() ) { 
                errmsg = "will not run compact on an active replica set primary as this is a slow blocking operation. use force:true to force";
                return false;
            }
            
            string ns = db + '.' + coll;
            if ( ! NamespaceString::normal(ns.c_str()) ) {
                errmsg = "bad namespace name";
                return false;
            }
            
            // parameter validation to avoid triggering assertions in compact()
            if ( str::contains(ns, ".system.") ) {
                errmsg = "can't compact a system namespace";
                return false;
            }
            
            {
                Lock::DBWrite lk(ns);
                Client::Context ctx(ns);
                NamespaceDetails *d = nsdetails(ns);
                if( ! d ) {
                    errmsg = "namespace does not exist";
                    return false;
                }

                if ( d->isCapped() ) {
                    errmsg = "cannot compact a capped collection";
                    return false;
                }
            }

            double pf = 1.0;
            int pb = 0;
            if( cmdObj.hasElement("paddingFactor") ) {
                pf = cmdObj["paddingFactor"].Number();
                verify( pf >= 1.0 && pf <= 4.0 );
            }
            if( cmdObj.hasElement("paddingBytes") ) {
                pb = (int) cmdObj["paddingBytes"].Number();
                verify( pb >= 0 && pb <= 1024 * 1024 );
            }

            bool validate = !cmdObj.hasElement("validate") || cmdObj["validate"].trueValue(); // default is true at the moment
            bool ok = compact(ns, errmsg, validate, result, pf, pb);
            return ok;
        }
    };
    static CompactCmd compactCmd;

}
