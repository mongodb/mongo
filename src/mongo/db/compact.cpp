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

#include "mongo/pch.h"

#include <string>
#include <vector>

#include "mongo/db/d_concurrency.h"
#include "mongo/db/curop-inl.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/structure/collection.h"
#include "mongo/util/concurrency/task.h"
#include "mongo/util/timer.h"
#include "mongo/util/touch_pages.h"

namespace mongo {

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
    unsigned compactExtent(Collection* collection, const DiskLoc diskloc, int n,
                           int nidx, bool validate, double pf, int pb, bool useDefaultPadding,
                           bool preservePadding) {

        log() << "compact begin extent #" << n << " for namespace " << collection->ns();
        unsigned oldObjSize = 0; // we'll report what the old padding was
        unsigned oldObjSizeWithPadding = 0;

        Extent *e = diskloc.ext();
        e->assertOk();
        verify( e->validates(diskloc) );
        unsigned skipped = 0;

        Database* db = cc().database();

        {
            // the next/prev pointers within the extent might not be in order so we first
            // page the whole thing in sequentially
            log() << "compact paging in len=" << e->length/1000000.0 << "MB" << endl;
            Timer t;
            Extent* ext = db->getExtentManager().getExtent( diskloc );
            size_t length = ext->length;

            touch_pages( reinterpret_cast<const char*>(ext), length );
            int ms = t.millis();
            if( ms > 1000 )
                log() << "compact end paging in " << ms << "ms "
                      << e->length/1000000.0/ms << "MB/sec" << endl;
        }

        {
            log() << "compact copying records" << endl;
            long long datasize = 0;
            long long nrecords = 0;
            DiskLoc L = e->firstRecord;
            if( !L.isNull() ) {
                while( 1 ) {
                    Record *recOld = L.rec();
                    L = db->getExtentManager().getNextRecordInExtent(L);
                    BSONObj objOld = BSONObj::make(recOld);

                    if( !validate || objOld.valid() ) {
                        nrecords++;
                        unsigned sz = objOld.objsize();

                        oldObjSize += sz;
                        oldObjSizeWithPadding += recOld->netLength();

                        unsigned lenWHdr = sz + Record::HeaderSize;
                        unsigned lenWPadding = lenWHdr;
                        // if we are preserving the padding, the record should not change size
                        if (preservePadding) {
                            lenWPadding = recOld->lengthWithHeaders();
                        }
                        // maintain UsePowerOf2Sizes if no padding values were passed in
                        else if (collection->details()->isUserFlagSet(NamespaceDetails::Flag_UsePowerOf2Sizes)
                                && useDefaultPadding) {
                            lenWPadding = collection->details()->quantizePowerOf2AllocationSpace(lenWPadding);
                        }
                        // otherwise use the padding values (pf and pb) that were passed in
                        else {
                            lenWPadding = static_cast<unsigned>(pf*lenWPadding);
                            lenWPadding += pb;
                            lenWPadding = lenWPadding & quantizeMask(lenWPadding);
                        }
                        if (lenWPadding < lenWHdr || lenWPadding > BSONObjMaxUserSize / 2 ) { 
                            lenWPadding = lenWHdr;
                        }
                        DiskLoc loc = allocateSpaceForANewRecord(collection->ns().ns().c_str(),
                                                                 collection->details(), lenWPadding, false);
                        uassert(14024, "compact error out of space during compaction", !loc.isNull());
                        Record *recNew = loc.rec();
                        datasize += recNew->netLength();
                        recNew = (Record *) getDur().writingPtr(recNew, lenWHdr);
                        addRecordToRecListInExtent(recNew, loc);
                        memcpy(recNew->data(), objOld.objdata(), sz);
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

            verify( collection->details()->firstExtent() == diskloc );
            verify( collection->details()->lastExtent() != diskloc );
            DiskLoc newFirst = e->xnext;
            collection->details()->firstExtent().writing() = newFirst;
            newFirst.ext()->xprev.writing().Null();
            getDur().writing(e)->markEmpty();
            cc().database()->getExtentManager().freeExtents( diskloc, diskloc );

            // update datasize/record count for this namespace's extent
            collection->details()->incrementStats( datasize, nrecords );

            getDur().commitIfNeeded();

            {
                double op = 1.0;
                if( oldObjSize )
                    op = static_cast<double>(oldObjSizeWithPadding)/oldObjSize;
                log() << "compact finished extent #" << n << " containing " << nrecords << " documents (" << datasize/1000000.0 << "MB)"
                      << " oldPadding: " << op << ' ' << static_cast<unsigned>(op*100.0)/100;
            }
        }

        return skipped;
    }

    bool compactCollection(Collection* collection, string& errmsg, bool validate,
                           BSONObjBuilder& result, double pf, int pb, bool useDefaultPadding,
                           bool preservePadding) {

        verify( collection );
        NamespaceDetails* d = collection->details();

        // this is a big job, so might as well make things tidy before we start just to be nice.
        getDur().commitIfNeeded();

        list<DiskLoc> extents;
        for( DiskLoc L = d->firstExtent(); !L.isNull(); L = L.ext()->xnext )
            extents.push_back(L);
        log() << "compact " << extents.size() << " extents" << endl;

        ProgressMeterHolder pm(cc().curop()->setMessage("compact extent",
                                                        "Extent Compacting Progress",
                                                        extents.size()));

        // same data, but might perform a little different after compact?
        collection->infoCache()->reset();

        verify( d->getCompletedIndexCount() == d->getTotalIndexCount() );
        int nidx = d->getCompletedIndexCount();
        scoped_array<BSONObj> indexSpecs( new BSONObj[nidx] );
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
                indexSpecs[idxNo] = b.obj().getOwned();
            }
        }

        log() << "compact orphan deleted lists" << endl;
        d->orphanDeletedList();

        // Start over from scratch with our extent sizing and growth
        d->setLastExtentSize( 0 );

        // before dropping indexes, at least make sure we can allocate one extent!
        uassert(14025,
                "compact error no space available to allocate",
                !allocateSpaceForANewRecord(collection->ns().ns().c_str(), d, Record::HeaderSize+1, false).isNull());

        // note that the drop indexes call also invalidates all clientcursors for the namespace, which is important and wanted here
        log() << "compact dropping indexes" << endl;
        Status status = collection->getIndexCatalog()->dropAllIndexes( true );
        if ( !status.isOK() ) {
            errmsg = str::stream() << "compact drop indexes failed: " << status.toString();
            log() << status.toString() << endl;
            return false;
        }

        getDur().commitIfNeeded();

        long long skipped = 0;
        int n = 0;

        // reset data size and record counts to 0 for this namespace
        // as we're about to tally them up again for each new extent
        d->setStats( 0, 0 );

        for( list<DiskLoc>::iterator i = extents.begin(); i != extents.end(); i++ ) { 
            skipped += compactExtent(collection, *i, n++, nidx, validate, pf, pb,
                                     useDefaultPadding, preservePadding);
            pm.hit();
        }

        if( skipped ) {
            result.append("invalidObjects", skipped);
        }

        verify( d->firstExtent().ext()->xprev.isNull() );

        // indexes will do their own progress meter?
        pm.finished();

        // build indexes
        for( int i = 0; i < nidx; i++ ) {
            killCurrentOp.checkForInterrupt(false);
            BSONObj info = indexSpecs[i];
            log() << "compact create index " << info["key"].Obj().toString() << endl;
            Status status = collection->getIndexCatalog()->createIndex( info, false );
            if ( !status.isOK() ) {
                log() << "failed to create index: " << status.toString();
                uassertStatusOK( status );
            }
        }

        return true;
    }

}
