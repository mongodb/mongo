// record_store_v1_base.cpp

/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/structure/record_store_v1_base.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/storage/transaction.h"
#include "mongo/db/structure/record_store_v1_repair_iterator.h"
#include "mongo/util/timer.h"
#include "mongo/util/touch_pages.h"

namespace mongo {

    const int RecordStoreV1Base::Buckets = 19;
    const int RecordStoreV1Base::MaxBucket = 18;

    /* Deleted list buckets are used to quickly locate free space based on size.  Each bucket
       contains records up to that size.  All records >= 4mb are placed into the 16mb bucket.
    */
    const int RecordStoreV1Base::bucketSizes[] = {
        0x20,     0x40,     0x80,     0x100,      // 32,   64,   128,  256
        0x200,    0x400,    0x800,    0x1000,     // 512,  1K,   2K,   4K
        0x2000,   0x4000,   0x8000,   0x10000,    // 8K,   16K,  32K,  64K
        0x20000,  0x40000,  0x80000,  0x100000,   // 128K, 256K, 512K, 1M
        0x200000, 0x400000, 0x1000000,            // 2M,   4M,   16M (see above)
     };


    RecordStoreV1Base::RecordStoreV1Base( const StringData& ns,
                                          RecordStoreV1MetaData* details,
                                          ExtentManager* em,
                                          bool isSystemIndexes )
        : RecordStore( ns ),
          _details( details ),
          _extentManager( em ),
          _isSystemIndexes( isSystemIndexes ) {
    }

    RecordStoreV1Base::~RecordStoreV1Base() {
    }


    int64_t RecordStoreV1Base::storageSize( BSONObjBuilder* extraInfo, int level ) const {
        BSONArrayBuilder extentInfo;

        int64_t total = 0;
        int n = 0;

        DiskLoc cur = _details->firstExtent();
        while ( !cur.isNull() ) {
            Extent* e = _extentManager->getExtent( cur );

            total += e->length;
            n++;

            if ( extraInfo && level > 0 ) {
                extentInfo.append( BSON( "len" << e->length << "loc: " << e->myLoc.toBSONObj() ) );
            }

            cur = e->xnext;
        }

        if ( extraInfo ) {
            extraInfo->append( "numExtents", n );
            if ( level > 0 )
                extraInfo->append( "extents", extentInfo.arr() );
        }

        return total;
    }

    Record* RecordStoreV1Base::recordFor( const DiskLoc& loc ) const {
        return _extentManager->recordForV1( loc );
    }

    const DeletedRecord* RecordStoreV1Base::deletedRecordFor( const DiskLoc& loc ) const {
        invariant( loc.a() != -1 );
        return reinterpret_cast<const DeletedRecord*>( recordFor( loc ) );
    }

    DeletedRecord* RecordStoreV1Base::drec( const DiskLoc& loc ) const {
        invariant( loc.a() != -1 );
        return reinterpret_cast<DeletedRecord*>( recordFor( loc ) );
    }

    Extent* RecordStoreV1Base::_getExtent( const DiskLoc& loc ) const {
        return _extentManager->getExtent( loc );
    }

    DiskLoc RecordStoreV1Base::_getExtentLocForRecord( const DiskLoc& loc ) const {
        return _extentManager->extentLocForV1( loc );
    }


    DiskLoc RecordStoreV1Base::getNextRecord( const DiskLoc& loc ) const {
        DiskLoc next = getNextRecordInExtent( loc );
        if ( !next.isNull() )
            return next;

        // now traverse extents

        Extent* e = _getExtent( _getExtentLocForRecord(loc) );
        while ( 1 ) {
            if ( e->xnext.isNull() )
                return DiskLoc(); // end of collection
            e = _getExtent( e->xnext );
            if ( !e->firstRecord.isNull() )
                break;
            // entire extent could be empty, keep looking
        }
        return e->firstRecord;
    }

    DiskLoc RecordStoreV1Base::getPrevRecord( const DiskLoc& loc ) const {
        DiskLoc prev = getPrevRecordInExtent( loc );
        if ( !prev.isNull() )
            return prev;

        // now traverse extents

        Extent *e = _getExtent(_getExtentLocForRecord(loc));
        while ( 1 ) {
            if ( e->xprev.isNull() )
                return DiskLoc(); // end of collection
            e = _getExtent( e->xprev );
            if ( !e->firstRecord.isNull() )
                break;
            // entire extent could be empty, keep looking
        }
        return e->lastRecord;

    }

    DiskLoc RecordStoreV1Base::_findFirstSpot( TransactionExperiment* txn,
                                               const DiskLoc& extDiskLoc, Extent* e ) {
        DiskLoc emptyLoc = extDiskLoc;
        emptyLoc.inc( Extent::HeaderSize() );
        int delRecLength = e->length - Extent::HeaderSize();
        if ( delRecLength >= 32*1024 && _ns.find('$') != string::npos && !isCapped() ) {
            // probably an index. so skip forward to keep its records page aligned
            int& ofs = emptyLoc.GETOFS();
            int newOfs = (ofs + 0xfff) & ~0xfff;
            delRecLength -= (newOfs-ofs);
            dassert( delRecLength > 0 );
            ofs = newOfs;
        }

        DeletedRecord* empty = txn->writing(drec(emptyLoc));
        empty->lengthWithHeaders() = delRecLength;
        empty->extentOfs() = e->myLoc.getOfs();
        empty->nextDeleted().Null();
        return emptyLoc;

    }

    DiskLoc RecordStoreV1Base::getNextRecordInExtent( const DiskLoc& loc ) const {
        int nextOffset = recordFor( loc )->nextOfs();

        if ( nextOffset == DiskLoc::NullOfs )
            return DiskLoc();

        fassert( 17441, abs(nextOffset) >= 8 ); // defensive
        return DiskLoc( loc.a(), nextOffset );
    }

    DiskLoc RecordStoreV1Base::getPrevRecordInExtent( const DiskLoc& loc ) const {
        int prevOffset = recordFor( loc )->prevOfs();

        if ( prevOffset == DiskLoc::NullOfs )
            return DiskLoc();

        fassert( 17442, abs(prevOffset) >= 8 ); // defensive
        return DiskLoc( loc.a(), prevOffset );

    }


    StatusWith<DiskLoc> RecordStoreV1Base::insertRecord( TransactionExperiment* txn,
                                                         const DocWriter* doc,
                                                         int quotaMax ) {
        int lenWHdr = doc->documentSize() + Record::HeaderSize;
        if ( doc->addPadding() )
            lenWHdr = getRecordAllocationSize( lenWHdr );

        StatusWith<DiskLoc> loc = allocRecord( txn, lenWHdr, quotaMax );
        if ( !loc.isOK() )
            return loc;

        Record *r = recordFor( loc.getValue() );
        fassert( 17319, r->lengthWithHeaders() >= lenWHdr );

        r = reinterpret_cast<Record*>( txn->writingPtr(r, lenWHdr) );
        doc->writeDocument( r->data() );

        _addRecordToRecListInExtent(txn, r, loc.getValue());

        _details->incrementStats( txn, r->netLength(), 1 );

        return loc;
    }


    StatusWith<DiskLoc> RecordStoreV1Base::insertRecord( TransactionExperiment* txn,
                                                         const char* data,
                                                         int len,
                                                         int quotaMax ) {
        int lenWHdr = getRecordAllocationSize( len + Record::HeaderSize );
        fassert( 17208, lenWHdr >= ( len + Record::HeaderSize ) );

        StatusWith<DiskLoc> loc = allocRecord( txn, lenWHdr, quotaMax );
        if ( !loc.isOK() )
            return loc;

        Record *r = recordFor( loc.getValue() );
        fassert( 17210, r->lengthWithHeaders() >= lenWHdr );

        // copy the data
        r = reinterpret_cast<Record*>( txn->writingPtr(r, lenWHdr) );
        memcpy( r->data(), data, len );

        _addRecordToRecListInExtent(txn, r, loc.getValue());

        _details->incrementStats( txn, r->netLength(), 1 );

        return loc;
    }

    void RecordStoreV1Base::deleteRecord( TransactionExperiment* txn, const DiskLoc& dl ) {

        Record* todelete = recordFor( dl );

        /* remove ourself from the record next/prev chain */
        {
            if ( todelete->prevOfs() != DiskLoc::NullOfs ) {
                DiskLoc prev = getPrevRecordInExtent( dl );
                Record* prevRecord = recordFor( prev );
                txn->writingInt( prevRecord->nextOfs() ) = todelete->nextOfs();
            }

            if ( todelete->nextOfs() != DiskLoc::NullOfs ) {
                DiskLoc next = getNextRecord( dl );
                Record* nextRecord = recordFor( next );
                txn->writingInt( nextRecord->prevOfs() ) = todelete->prevOfs();
            }
        }

        /* remove ourself from extent pointers */
        {
            Extent *e = txn->writing( _getExtent( _getExtentLocForRecord( dl ) ) );
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
            _details->incrementStats( txn, -1 * todelete->netLength(), -1 );

            if ( _isSystemIndexes ) {
                /* temp: if in system.indexes, don't reuse, and zero out: we want to be
                   careful until validated more, as IndexDetails has pointers
                   to this disk location.  so an incorrectly done remove would cause
                   a lot of problems.
                */
                memset( txn->writingPtr(todelete, todelete->lengthWithHeaders() ),
                        0, todelete->lengthWithHeaders() );
            }
            else {
                DEV {
                    unsigned long long *p = reinterpret_cast<unsigned long long *>( todelete->data() );
                    *txn->writing(p) = 0;
                }
                addDeletedRec(txn, dl);
            }
        }

    }

    RecordIterator* RecordStoreV1Base::getIteratorForRepair() const {
        return new RecordStoreV1RepairIterator(this);
    }

    void RecordStoreV1Base::_addRecordToRecListInExtent(TransactionExperiment* txn,
                                                        Record *r,
                                                        DiskLoc loc) {
        dassert( recordFor(loc) == r );
        Extent *e = _getExtent( _getExtentLocForRecord( loc ) );
        if ( e->lastRecord.isNull() ) {
            *txn->writing(&e->firstRecord) = loc;
            *txn->writing(&e->lastRecord) = loc;
            r->prevOfs() = r->nextOfs() = DiskLoc::NullOfs;
        }
        else {
            Record *oldlast = recordFor(e->lastRecord);
            r->prevOfs() = e->lastRecord.getOfs();
            r->nextOfs() = DiskLoc::NullOfs;
            txn->writingInt(oldlast->nextOfs()) = loc.getOfs();
            *txn->writing(&e->lastRecord) = loc;
        }
    }

    void RecordStoreV1Base::increaseStorageSize( TransactionExperiment* txn,
                                                 int size,
                                                 int quotaMax ) {
        DiskLoc eloc = _extentManager->allocateExtent( txn,
                                                       isCapped(),
                                                       size,
                                                       quotaMax );

        Extent *e = _extentManager->getExtent( eloc );
        invariant( e );

        *txn->writing( &e->nsDiagnostic ) = _ns;

        txn->writing( &e->xnext )->Null();
        txn->writing( &e->xprev )->Null();
        txn->writing( &e->firstRecord )->Null();
        txn->writing( &e->lastRecord )->Null();

        DiskLoc emptyLoc = _findFirstSpot( txn, eloc, e );

        if ( _details->lastExtent().isNull() ) {
            verify( _details->firstExtent().isNull() );
            _details->setFirstExtent( txn, eloc );
            _details->setLastExtent( txn, eloc );
            _details->setCapExtent( txn, eloc );
            verify( e->xprev.isNull() );
            verify( e->xnext.isNull() );
        }
        else {
            verify( !_details->firstExtent().isNull() );
            *txn->writing(&e->xprev) = _details->lastExtent();
            *txn->writing(&_extentManager->getExtent(_details->lastExtent())->xnext) = eloc;
            _details->setLastExtent( txn, eloc );
        }

        _details->setLastExtentSize( txn, e->length );

        addDeletedRec(txn, emptyLoc);
    }

    Status RecordStoreV1Base::validate( TransactionExperiment* txn,
                                        bool full, bool scanData,
                                        ValidateAdaptor* adaptor,
                                        ValidateResults* results, BSONObjBuilder* output ) const {

        // 1) basic status that require no iteration
        // 2) extent level info
        // 3) check extent start and end
        // 4) check each non-deleted record
        // 5) check deleted list

        // -------------

        // 1111111111111111111
        if ( isCapped() ){
            output->appendBool("capped", true);
            output->appendNumber("max", _details->maxCappedDocs());
        }

        output->appendNumber("datasize", _details->dataSize());
        output->appendNumber("nrecords", _details->numRecords());
        output->appendNumber("lastExtentSize", _details->lastExtentSize());
        output->appendNumber("padding", _details->paddingFactor());

        if ( _details->firstExtent().isNull() )
            output->append( "firstExtent", "null" );
        else
            output->append( "firstExtent",
                            str::stream() << _details->firstExtent().toString()
                            << " ns:"
                            << _getExtent( _details->firstExtent() )->nsDiagnostic.toString());
        if ( _details->lastExtent().isNull() )
            output->append( "lastExtent", "null" );
        else
            output->append( "lastExtent", str::stream() << _details->lastExtent().toString()
                            << " ns:"
                            << _getExtent( _details->lastExtent() )->nsDiagnostic.toString());

        // 22222222222222222222222222
        { // validate extent basics
            BSONArrayBuilder extentData;
            int extentCount = 0;
            try {
                if ( !_details->firstExtent().isNull() ) {
                    _getExtent( _details->firstExtent() )->assertOk();
                    _getExtent( _details->lastExtent() )->assertOk();
                }

                DiskLoc extentDiskLoc = _details->firstExtent();
                while (!extentDiskLoc.isNull()) {
                    Extent* thisExtent = _getExtent( extentDiskLoc );
                    if (full) {
                        extentData << thisExtent->dump();
                    }
                    if (!thisExtent->validates(extentDiskLoc, &results->errors)) {
                        results->valid = false;
                    }
                    DiskLoc nextDiskLoc = thisExtent->xnext;
                    if (extentCount > 0 && !nextDiskLoc.isNull()
                        &&  _getExtent( nextDiskLoc )->xprev != extentDiskLoc) {
                        StringBuilder sb;
                        sb << "'xprev' pointer " << _getExtent( nextDiskLoc )->xprev.toString()
                           << " in extent " << nextDiskLoc.toString()
                           << " does not point to extent " << extentDiskLoc.toString();
                        results->errors.push_back( sb.str() );
                        results->valid = false;
                    }
                    if (nextDiskLoc.isNull() && extentDiskLoc != _details->lastExtent()) {
                        StringBuilder sb;
                        sb << "'lastExtent' pointer " << _details->lastExtent().toString()
                           << " does not point to last extent in list " << extentDiskLoc.toString();
                        results->errors.push_back( sb.str() );
                        results->valid = false;
                    }
                    extentDiskLoc = nextDiskLoc;
                    extentCount++;
                    txn->checkForInterrupt();
                }
            }
            catch (const DBException& e) {
                StringBuilder sb;
                sb << "exception validating extent " << extentCount
                   << ": " << e.what();
                results->errors.push_back( sb.str() );
                results->valid = false;
                return Status::OK();
            }
            output->append("extentCount", extentCount);

            if ( full )
                output->appendArray( "extents" , extentData.arr() );

        }

        try {
            // 333333333333333333333333333
            bool testingLastExtent = false;
            try {
                if (_details->firstExtent().isNull()) {
                    // this is ok
                }
                else {
                    output->append("firstExtentDetails", _getExtent(_details->firstExtent())->dump());
                    if (!_getExtent(_details->firstExtent())->xprev.isNull()) {
                        StringBuilder sb;
                        sb << "'xprev' pointer in 'firstExtent' " << _details->firstExtent().toString()
                           << " is " << _getExtent(_details->firstExtent())->xprev.toString()
                           << ", should be null";
                        results->errors.push_back( sb.str() );
                        results->valid = false;
                    }
                }
                testingLastExtent = true;
                if (_details->lastExtent().isNull()) {
                    // this is ok
                }
                else {
                    if (_details->firstExtent() != _details->lastExtent()) {
                        output->append("lastExtentDetails", _getExtent(_details->lastExtent())->dump());
                        if (!_getExtent(_details->lastExtent())->xnext.isNull()) {
                            StringBuilder sb;
                            sb << "'xnext' pointer in 'lastExtent' " << _details->lastExtent().toString()
                               << " is " << _getExtent(_details->lastExtent())->xnext.toString()
                               << ", should be null";
                            results->errors.push_back( sb.str() );
                            results->valid = false;
                        }
                    }
                }
            }
            catch (const DBException& e) {
                StringBuilder sb;
                sb << "exception processing '"
                   << (testingLastExtent ? "lastExtent" : "firstExtent")
                   << "': " << e.what();
                results->errors.push_back( sb.str() );
                results->valid = false;
            }

            // 4444444444444444444444444

            set<DiskLoc> recs;
            if( scanData ) {
                int n = 0;
                int nInvalid = 0;
                long long nQuantizedSize = 0;
                long long nPowerOf2QuantizedSize = 0;
                long long len = 0;
                long long nlen = 0;
                long long bsonLen = 0;
                int outOfOrder = 0;
                DiskLoc cl_last;

                scoped_ptr<RecordIterator> iterator( getIterator( DiskLoc(),
                                                                  false,
                                                                  CollectionScanParams::FORWARD ) );
                DiskLoc cl;
                while ( !( cl = iterator->getNext() ).isNull() ) {
                    n++;

                    if ( n < 1000000 )
                        recs.insert(cl);
                    if ( isCapped() ) {
                        if ( cl < cl_last )
                            outOfOrder++;
                        cl_last = cl;
                    }

                    Record *r = recordFor(cl);
                    len += r->lengthWithHeaders();
                    nlen += r->netLength();

                    if ( r->lengthWithHeaders() ==
                         quantizeAllocationSpace( r->lengthWithHeaders() ) ) {
                        // Count the number of records having a size consistent with
                        // the quantizeAllocationSpace quantization implementation.
                        ++nQuantizedSize;
                    }

                    if ( r->lengthWithHeaders() ==
                         quantizePowerOf2AllocationSpace( r->lengthWithHeaders() - 1 ) ) {
                        // Count the number of records having a size consistent with the
                        // quantizePowerOf2AllocationSpace quantization implementation.
                        // Because of SERVER-8311, power of 2 quantization is not idempotent and
                        // r->lengthWithHeaders() - 1 must be checked instead of the record
                        // length itself.
                        ++nPowerOf2QuantizedSize;
                    }

                    if (full){
                        size_t dataSize = 0;
                        const Status status = adaptor->validate( r, &dataSize );
                        if (!status.isOK()) {
                            results->valid = false;
                            if (nInvalid == 0) // only log once;
                                results->errors.push_back( "invalid object detected (see logs)" );

                            nInvalid++;
                            log() << "Invalid object detected in " << _ns
                                  << ": " << status.reason();
                        }
                        else {
                            bsonLen += dataSize;
                        }
                    }
                }

                if ( isCapped() && !_details->capLooped() ) {
                    output->append("cappedOutOfOrder", outOfOrder);
                    if ( outOfOrder > 1 ) {
                        results->valid = false;
                        results->errors.push_back( "too many out of order records" );
                    }
                }
                output->append("objectsFound", n);

                if (full) {
                    output->append("invalidObjects", nInvalid);
                }

                output->appendNumber("nQuantizedSize", nQuantizedSize);
                output->appendNumber("nPowerOf2QuantizedSize", nPowerOf2QuantizedSize);
                output->appendNumber("bytesWithHeaders", len);
                output->appendNumber("bytesWithoutHeaders", nlen);

                if (full) {
                    output->appendNumber("bytesBson", bsonLen);
                }
            } // end scanData

            // 55555555555555555555555555
            BSONArrayBuilder deletedListArray;
            for ( int i = 0; i < Buckets; i++ ) {
                deletedListArray << _details->deletedListEntry(i).isNull();
            }

            int ndel = 0;
            long long delSize = 0;
            BSONArrayBuilder delBucketSizes;
            int incorrect = 0;
            for ( int i = 0; i < Buckets; i++ ) {
                DiskLoc loc = _details->deletedListEntry(i);
                try {
                    int k = 0;
                    while ( !loc.isNull() ) {
                        if ( recs.count(loc) )
                            incorrect++;
                        ndel++;

                        if ( loc.questionable() ) {
                            if( isCapped() && !loc.isValid() && i == 1 ) {
                                /* the constructor for NamespaceDetails intentionally sets deletedList[1] to invalid
                                   see comments in namespace.h
                                */
                                break;
                            }

                            string err( str::stream() << "bad pointer in deleted record list: "
                                        << loc.toString()
                                        << " bucket: " << i
                                        << " k: " << k );
                            results->errors.push_back( err );
                            results->valid = false;
                            break;
                        }

                        const DeletedRecord* d = deletedRecordFor(loc);
                        delSize += d->lengthWithHeaders();
                        loc = d->nextDeleted();
                        k++;
                        txn->checkForInterrupt();
                    }
                    delBucketSizes << k;
                }
                catch (...) {
                    results->errors.push_back( (string)"exception in deleted chain for bucket " +
                                               BSONObjBuilder::numStr(i) );
                    results->valid = false;
                }
            }
            output->appendNumber("deletedCount", ndel);
            output->appendNumber("deletedSize", delSize);
            if ( full ) {
                output->append( "delBucketSizes", delBucketSizes.arr() );
            }

            if ( incorrect ) {
                results->errors.push_back( BSONObjBuilder::numStr(incorrect) +
                                           " records from datafile are in deleted list" );
                results->valid = false;
            }

        }
        catch (AssertionException) {
            results->errors.push_back( "exception during validate" );
            results->valid = false;
        }

        return Status::OK();
    }

    namespace {
        struct touch_location {
            const char* root;
            size_t length;
        };
    }

    Status RecordStoreV1Base::touch( TransactionExperiment* txn, BSONObjBuilder* output ) const {
        Timer t;

        // Note: when this class has document level locking, we'll need a lock to get extents
        // and then ideally only hold the collection lock from above while doing actual touching.

        std::vector<touch_location> ranges;
        {
            Extent* ext = _getExtent( _details->firstExtent() );
            while ( ext ) {
                touch_location tl;
                tl.root = reinterpret_cast<const char*>(ext);
                tl.length = ext->length;
                ranges.push_back(tl);
                if ( ext->xnext.isNull() )
                    ext = NULL;
                else
                    ext = _getExtent( ext->xnext );
            }
        }

        /* TODO(ERH)
        std::string progress_msg = "touch " + ns + " extents";
        ProgressMeterHolder pm(cc().curop()->setMessage(progress_msg.c_str(),
                                                        "Touch Progress",
                                                        ranges.size()));
        */

        for ( std::vector<touch_location>::iterator it = ranges.begin(); it != ranges.end(); ++it ) {
            touch_pages( it->root, it->length );
            //pm.hit();
            txn->checkForInterrupt();
        }
        //pm.finished();

        if ( output ) {
            output->append( "numRanges", static_cast<int>( ranges.size() ) );
            output->append( "millis", t.millis() );
        }

        return Status::OK();
    }

    int RecordStoreV1Base::getRecordAllocationSize( int minRecordSize ) const {

        if ( isCapped() )
            return minRecordSize;

        invariant( _details->paddingFactor() >= 1 );

        if ( _details->isUserFlagSet( Flag_UsePowerOf2Sizes ) ) {
            // quantize to the nearest bucketSize (or nearest 1mb boundary for large sizes).
            return quantizePowerOf2AllocationSpace(minRecordSize);
        }

        // adjust for padding factor
        return static_cast<int>(minRecordSize * _details->paddingFactor());
    }

    DiskLoc RecordStoreV1Base::IntraExtentIterator::getNext() {
        if (_curr.isNull())
            return DiskLoc();

        const DiskLoc out = _curr; // we always return where we were, not where we will be.
        const Record* rec = recordFor(_curr);
        const int nextOfs = _forward ? rec->nextOfs() : rec->prevOfs();
        _curr = (nextOfs == DiskLoc::NullOfs ? DiskLoc() : DiskLoc(_curr.a(), nextOfs));
        return out;
    }

    void RecordStoreV1Base::IntraExtentIterator::invalidate(const DiskLoc& dl) {
        if (dl == _curr)
            getNext();
    }

    /* @return the size for an allocated record quantized to 1/16th of the BucketSize
       @param allocSize    requested size to allocate
    */
    int RecordStoreV1Base::quantizeAllocationSpace(int allocSize) {
        const int bucketIdx = bucket(allocSize);
        int bucketSize = bucketSizes[bucketIdx];
        int quantizeUnit = bucketSize / 16;
        if (allocSize >= (1 << 22)) // 4mb
            // all allocatons >= 4mb result in 4mb/16 quantization units, even if >= 8mb.  idea is
            // to reduce quantization overhead of large records at the cost of increasing the
            // DeletedRecord size distribution in the largest bucket by factor of 4.
            quantizeUnit = (1 << 18); // 256k
        if (allocSize % quantizeUnit == 0)
            // size is already quantized
            return allocSize;
        const int quantizedSpace = (allocSize | (quantizeUnit - 1)) + 1;
        fassert(16484, quantizedSpace >= allocSize);
        return quantizedSpace;
    }

    int RecordStoreV1Base::quantizePowerOf2AllocationSpace(int allocSize) {
        int allocationSize = bucketSizes[ bucket( allocSize ) ];
        if ( allocationSize == bucketSizes[MaxBucket] ) {
            // if we get here, it means we're allocating more than 4mb, so round
            // to the nearest megabyte
            allocationSize = 1 + ( allocSize | ( ( 1 << 20 ) - 1 ) );
        }
        return allocationSize;
    }

    int RecordStoreV1Base::bucket(int size) {
        for ( int i = 0; i < Buckets; i++ ) {
            if ( bucketSizes[i] > size ) {
                // Return the first bucket sized _larger_ than the requested size.
                return i;
            }
        }
        return MaxBucket;
    }

}
