// collection_compact.cpp

/**
*    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/db/catalog/collection.h"

#include "mongo/base/counter.h"
#include "mongo/base/owned_pointer_map.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/util/touch_pages.h"

namespace mongo {

    class CompactDocWriter : public DocWriter {
    public:
        /**
         * param allocationSize - allocation size WITH header
         */
        CompactDocWriter( const BSONObj& doc, size_t allocationSize )
            : _doc( doc ), _allocationSize( allocationSize ) {
        }

        virtual ~CompactDocWriter() {}

        virtual void writeDocument( char* buf ) const {
            memcpy( buf, _doc.objdata(), _doc.objsize() );
        }

        virtual size_t documentSize() const {
            return _allocationSize - Record::HeaderSize;
        }

        virtual bool addPadding() const {
            return false;
        }

    private:
        BSONObj _doc;
        size_t _allocationSize;
    };

    void Collection::_compactExtent(const DiskLoc diskloc, int extentNumber,
                                    MultiIndexBlock& indexesToInsertTo,
                                    const CompactOptions* compactOptions, CompactStats* stats ) {

        log() << "compact begin extent #" << extentNumber
              << " for namespace " << _ns << " " << diskloc;

        unsigned oldObjSize = 0; // we'll report what the old padding was
        unsigned oldObjSizeWithPadding = 0;

        Extent *e = getExtentManager()->getExtent( diskloc );
        e->assertOk();
        verify( e->validates(diskloc) );

        {
            // the next/prev pointers within the extent might not be in order so we first
            // page the whole thing in sequentially
            log() << "compact paging in len=" << e->length/1000000.0 << "MB" << endl;
            Timer t;
            size_t length = e->length;

            touch_pages( reinterpret_cast<const char*>(e), length );
            int ms = t.millis();
            if( ms > 1000 )
                log() << "compact end paging in " << ms << "ms "
                      << e->length/1000000.0/t.seconds() << "MB/sec" << endl;
        }

        {
            log() << "compact copying records" << endl;
            long long datasize = 0;
            long long nrecords = 0;
            DiskLoc L = e->firstRecord;
            if( !L.isNull() ) {
                while( 1 ) {
                    Record *recOld = _recordStore->recordFor(L);
                    BSONObj objOld = docFor( L );
                    L = getExtentManager()->getNextRecordInExtent(L);

                    if ( compactOptions->validateDocuments && !objOld.valid() ) {
                        // object is corrupt!
                        log() << "compact skipping corrupt document!";
                        stats->corruptDocuments++;
                    }
                    else {
                        unsigned docSize = objOld.objsize();

                        nrecords++;
                        oldObjSize += docSize;
                        oldObjSizeWithPadding += recOld->netLength();

                        unsigned lenWHdr = docSize + Record::HeaderSize;
                        unsigned lenWPadding = lenWHdr;

                        switch( compactOptions->paddingMode ) {
                        case CompactOptions::NONE:
                            if ( details()->isUserFlagSet(NamespaceDetails::Flag_UsePowerOf2Sizes) )
                                lenWPadding = details()->quantizePowerOf2AllocationSpace(lenWPadding);
                            break;
                        case CompactOptions::PRESERVE:
                            // if we are preserving the padding, the record should not change size
                            lenWPadding = recOld->lengthWithHeaders();
                            break;
                        case CompactOptions::MANUAL:
                            lenWPadding = compactOptions->computeRecordSize(lenWPadding);
                            if (lenWPadding < lenWHdr || lenWPadding > BSONObjMaxUserSize / 2 ) {
                                lenWPadding = lenWHdr;
                            }
                            break;
                        }

                        CompactDocWriter writer( objOld, lenWPadding );
                        StatusWith<DiskLoc> status = _recordStore->insertRecord( &writer, 0 );
                        uassertStatusOK( status.getStatus() );
                        datasize += _recordStore->recordFor( status.getValue() )->netLength();

                        InsertDeleteOptions options;
                        options.logIfError = false;
                        options.dupsAllowed = true; // in compact we should be doing no checking

                        indexesToInsertTo.insert( objOld, status.getValue(), options );
                    }

                    if( L.isNull() ) {
                        // we just did the very last record from the old extent.  it's still pointed to
                        // by the old extent ext, but that will be fixed below after this loop
                        break;
                    }

                    // remove the old records (orphan them) periodically so our commit block doesn't get too large
                    bool stopping = false;
                    RARELY stopping = *killCurrentOp.checkForInterruptNoAssert() != 0;
                    if( stopping || getDur().isCommitNeeded() ) {
                        e->firstRecord.writing() = L;
                        Record *r = _recordStore->recordFor(L);
                        getDur().writingInt(r->prevOfs()) = DiskLoc::NullOfs;
                        getDur().commitIfNeeded();
                        killCurrentOp.checkForInterrupt();
                    }
                }
            } // if !L.isNull()

            verify( details()->firstExtent() == diskloc );
            verify( details()->lastExtent() != diskloc );
            DiskLoc newFirst = e->xnext;
            details()->firstExtent().writing() = newFirst;
            getExtentManager()->getExtent( newFirst )->xprev.writing().Null();
            getDur().writing(e)->markEmpty();
            getExtentManager()->freeExtents( diskloc, diskloc );

            getDur().commitIfNeeded();

            {
                double op = 1.0;
                if( oldObjSize )
                    op = static_cast<double>(oldObjSizeWithPadding)/oldObjSize;
                log() << "compact finished extent #" << extentNumber << " containing " << nrecords
                      << " documents (" << datasize/1000000.0 << "MB)"
                      << " oldPadding: " << op << ' ' << static_cast<unsigned>(op*100.0)/100;
            }
        }

    }

    BSONObj _compactAdjustIndexSpec( const BSONObj& oldSpec ) {
        BSONObjBuilder b;
        BSONObj::iterator i( oldSpec );
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
        return b.obj();
    }

    StatusWith<CompactStats> Collection::compact( const CompactOptions* compactOptions ) {

        if ( isCapped() )
            return StatusWith<CompactStats>( ErrorCodes::BadValue,
                                             "cannot compact capped collection" );

        if ( _indexCatalog.numIndexesInProgress() )
            return StatusWith<CompactStats>( ErrorCodes::BadValue,
                                             "cannot compact when indexes in progress" );

        // this is a big job, so might as well make things tidy before we start just to be nice.
        getDur().commitIfNeeded();

        list<DiskLoc> extents;
        for( DiskLoc extLocation = _details->firstExtent();
             !extLocation.isNull();
             extLocation = getExtentManager()->getExtent( extLocation )->xnext ) {
            extents.push_back( extLocation );
        }
        log() << "compact " << extents.size() << " extents";

        // same data, but might perform a little different after compact?
        _infoCache.reset();

        vector<BSONObj> indexSpecs;
        {
            IndexCatalog::IndexIterator ii( _indexCatalog.getIndexIterator( false ) );
            while ( ii.more() ) {
                IndexDescriptor* descriptor = ii.next();

                const BSONObj spec = _compactAdjustIndexSpec(descriptor->infoObj());
                const BSONObj key = spec.getObjectField("key");
                const Status keyStatus = validateKeyPattern(key);
                if (!keyStatus.isOK()) {
                    return StatusWith<CompactStats>(
                        ErrorCodes::CannotCreateIndex,
                        str::stream() << "Cannot compact collection due to invalid index "
                                      << spec << ": " << keyStatus.reason() << " For more info see"
                                      << " http://dochub.mongodb.org/core/index-validation");
                }
                indexSpecs.push_back(spec);
            }
        }

        log() << "compact orphan deleted lists" << endl;
        _details->orphanDeletedList();

        // Start over from scratch with our extent sizing and growth
        _details->setLastExtentSize( 0 );

        // before dropping indexes, at least make sure we can allocate one extent!
        // this will allocate an extent and add to free list
        // if it cannot, it will throw an exception
        increaseStorageSize( _details->lastExtentSize(), true );

        // note that the drop indexes call also invalidates all clientcursors for the namespace,
        // which is important and wanted here
        log() << "compact dropping indexes" << endl;
        Status status = _indexCatalog.dropAllIndexes( true );
        if ( !status.isOK() ) {
            return StatusWith<CompactStats>( status );
        }

        getDur().commitIfNeeded();
        killCurrentOp.checkForInterrupt();

        CompactStats stats;

        MultiIndexBlock multiIndexBlock( this );
        status = multiIndexBlock.init( indexSpecs );
        if ( !status.isOK() )
            return StatusWith<CompactStats>( status );

        // reset data size and record counts to 0 for this namespace
        // as we're about to tally them up again for each new extent
        _details->setStats( 0, 0 );

        ProgressMeterHolder pm(cc().curop()->setMessage("compact extent",
                                                        "Extent Compacting Progress",
                                                        extents.size()));

        int extentNumber = 0;
        for( list<DiskLoc>::iterator i = extents.begin(); i != extents.end(); i++ ) {
            _compactExtent(*i, extentNumber++, multiIndexBlock, compactOptions, &stats );
            pm.hit();
        }

        invariant( getExtentManager()->getExtent( _details->firstExtent() )->xprev.isNull() );

        // indexes will do their own progress meter
        pm.finished();

        log() << "starting index commits";

        status = multiIndexBlock.commit();
        if ( !status.isOK() )
            return StatusWith<CompactStats>( status );

        return StatusWith<CompactStats>( stats );
    }


}
