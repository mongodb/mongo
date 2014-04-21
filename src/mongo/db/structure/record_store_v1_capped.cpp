// record_store_v1_capped.cpp

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

#include "mongo/db/structure/record_store_v1_capped.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/structure/record_store_v1_capped_iterator.h"

/*
 capped collection layout

 d's below won't exist if things align perfectly:

 extent1             -> extent2                 -> extent3
 -------------------    -----------------------    ---------------------
 d r r r r r r r r d    d r r r r d r r r r r d    d r r r r r r r r r d
                                ^   ^
                           oldest   newest

                        ^cappedFirstDeletedInCurExtent()
                   ^cappedLastDelRecLastExtent()
 ^cappedListOfAllDeletedRecords()
*/

#define DDD(x)

namespace mongo {

    CappedRecordStoreV1::CappedRecordStoreV1( Collection* collection,
                                              const StringData& ns,
                                              RecordStoreV1MetaData* details,
                                              ExtentManager* em,
                                              bool isSystemIndexes )
        : RecordStoreV1Base( ns, details, em, isSystemIndexes ),
          _collection( collection ) {

        DiskLoc extentLoc = details->firstExtent();
        while ( !extentLoc.isNull() ) {
            Extent* extent = em->getExtent( extentLoc );
            extentLoc = extent->xnext;
            MAdvise* m( new MAdvise( reinterpret_cast<void*>( extent ),
                                     extent->length,
                                     MAdvise::Sequential ) );
            _extentAdvice.mutableVector().push_back( m );
        }

        // this is for VERY VERY old versions of capped collections
        cappedCheckMigrate();
    }

    CappedRecordStoreV1::~CappedRecordStoreV1() {
    }

    StatusWith<DiskLoc> CappedRecordStoreV1::allocRecord( int lenToAlloc, int quotaMax ) {
        {
            // align very slightly.
            lenToAlloc = (lenToAlloc + 3) & 0xfffffffc;
        }

        if ( lenToAlloc > theCapExtent()->length ) {
            // the extent check is a way to try and improve performance
            // since we have to iterate all the extents (for now) to get
            // storage size
            if ( lenToAlloc > _collection->storageSize() ) {
                return StatusWith<DiskLoc>( ErrorCodes::BadValue,
                                            str::stream() << "document is larger than capped size "
                                            << lenToAlloc << " > " << _collection->storageSize(),
                                            16328 );
            }

        }
        DiskLoc loc;
        { // do allocation

            // signal done allocating new extents.
            if ( !cappedLastDelRecLastExtent().isValid() )
                setLastDelRecLastExtent( DiskLoc() );

            invariant( lenToAlloc < 400000000 );
            int passes = 0;
            int maxPasses = ( lenToAlloc / 30 ) + 2; // 30 is about the smallest entry that could go in the oplog
            if ( maxPasses < 5000 ) {
                // this is for bacwards safety since 5000 was the old value
                maxPasses = 5000;
            }

            // delete records until we have room and the max # objects limit achieved.

            /* this fails on a rename -- that is ok but must keep commented out */
            //invariant( theCapExtent()->ns == ns );

            theCapExtent()->assertOk();
            DiskLoc firstEmptyExtent;
            while ( 1 ) {
                if ( _details->numRecords() < _details->maxCappedDocs() ) {
                    loc = __capAlloc( lenToAlloc );
                    if ( !loc.isNull() )
                        break;
                }

                // If on first iteration through extents, don't delete anything.
                if ( !_details->capFirstNewRecord().isValid() ) {
                    advanceCapExtent( _ns );

                    if ( _details->capExtent() != _details->firstExtent() )
                        _details->setCapFirstNewRecord( DiskLoc().setInvalid() );
                    // else signal done with first iteration through extents.
                    continue;
                }

                if ( !_details->capFirstNewRecord().isNull() &&
                     theCapExtent()->firstRecord == _details->capFirstNewRecord() ) {
                    // We've deleted all records that were allocated on the previous
                    // iteration through this extent.
                    advanceCapExtent( _ns );
                    continue;
                }

                if ( theCapExtent()->firstRecord.isNull() ) {
                    if ( firstEmptyExtent.isNull() )
                        firstEmptyExtent = _details->capExtent();
                    advanceCapExtent( _ns );
                    if ( firstEmptyExtent == _details->capExtent() ) {
                        _maybeComplain( lenToAlloc );
                        return StatusWith<DiskLoc>( ErrorCodes::InternalError,
                                                    "no space in capped collection" );
                    }
                    continue;
                }

                DiskLoc fr = theCapExtent()->firstRecord;
                _collection->deleteDocument( fr, true );
                compact();
                if( ++passes > maxPasses ) {
                    StringBuilder sb;
                    sb << "passes >= maxPasses in CappedRecordStoreV1::cappedAlloc: ns: " << _ns
                       << ", lenToAlloc: " << lenToAlloc
                       << ", maxPasses: " << maxPasses
                       << ", _maxDocsInCapped: " << _details->maxCappedDocs()
                       << ", nrecords: " << _details->numRecords()
                       << ", datasize: " << _details->dataSize();

                    return StatusWith<DiskLoc>( ErrorCodes::InternalError, sb.str() );
                }
            }

            // Remember first record allocated on this iteration through capExtent.
            if ( _details->capFirstNewRecord().isValid() && _details->capFirstNewRecord().isNull() )
                _details->setCapFirstNewRecord( loc );
        }

        invariant( !loc.isNull() );

        // possibly slice up if we've allocated too much space

        DeletedRecord *r = drec( loc );

        /* note we want to grab from the front so our next pointers on disk tend
        to go in a forward direction which is important for performance. */
        int regionlen = r->lengthWithHeaders();
        invariant( r->extentOfs() < loc.getOfs() );

        int left = regionlen - lenToAlloc;

        /* split off some for further use. */
        getDur().writingInt(r->lengthWithHeaders()) = lenToAlloc;
        DiskLoc newDelLoc = loc;
        newDelLoc.inc(lenToAlloc);
        DeletedRecord* newDel = drec( newDelLoc );
        DeletedRecord* newDelW = getDur().writing(newDel);
        newDelW->extentOfs() = r->extentOfs();
        newDelW->lengthWithHeaders() = left;
        newDelW->nextDeleted().Null();

        addDeletedRec(newDelLoc);

        return StatusWith<DiskLoc>( loc );
    }

    Status CappedRecordStoreV1::truncate() {
        setLastDelRecLastExtent( DiskLoc() );
        setListOfAllDeletedRecords( DiskLoc() );

        // preserve firstExtent/lastExtent
        _details->setCapExtent( _details->firstExtent() );
        _details->setStats( 0, 0 );
        // preserve lastExtentSize
        // nIndexes preserve 0
        // capped preserve true
        // max preserve
        _details->setPaddingFactor( 1.0 );
        _details->clearSystemFlags();
        _details->setCapFirstNewRecord( DiskLoc().setInvalid() );
        setLastDelRecLastExtent( DiskLoc().setInvalid() );
        // dataFileVersion preserve
        // indexFileVersion preserve

        // Reset all existing extents and recreate the deleted list.
        for( DiskLoc ext = _details->firstExtent();
             !ext.isNull();
             ext = _extentManager->getExtent(ext)->xnext ) {

            DiskLoc prev = _extentManager->getExtent(ext)->xprev;
            DiskLoc next = _extentManager->getExtent(ext)->xnext;
            DiskLoc empty = _extentManager->getExtent(ext)->reuse( _ns, true );
            getDur().writingDiskLoc(_extentManager->getExtent(ext)->xprev) = prev;
            getDur().writingDiskLoc(_extentManager->getExtent(ext)->xnext) = next;
            addDeletedRec( empty );
        }

        return Status::OK();
    }

    void CappedRecordStoreV1::temp_cappedTruncateAfter( DiskLoc end, bool inclusive ) {
        cappedTruncateAfter( _ns.c_str(), end, inclusive );
    }

    /* combine adjacent deleted records *for the current extent* of the capped collection

       this is O(n^2) but we call it for capped tables where typically n==1 or 2!
       (or 3...there will be a little unused sliver at the end of the extent.)
    */
    void CappedRecordStoreV1::compact() {
        DDD( "CappedRecordStoreV1::compact enter" );

        vector<DiskLoc> drecs;

        // Pull out capExtent's DRs from deletedList
        DiskLoc i = cappedFirstDeletedInCurExtent();
        for (; !i.isNull() && inCapExtent( i ); i = deletedRecordFor( i )->nextDeleted() ) {
            DDD( "\t" << i );
            drecs.push_back( i );
        }

        setFirstDeletedInCurExtent( i );

        std::sort( drecs.begin(), drecs.end() );
        DDD( "\t drecs.size(): " << drecs.size() );

        vector<DiskLoc>::const_iterator j = drecs.begin();
        invariant( j != drecs.end() );
        DiskLoc a = *j;
        while ( 1 ) {
            j++;
            if ( j == drecs.end() ) {
                DDD( "\t compact adddelrec" );
                addDeletedRec( a);
                break;
            }
            DiskLoc b = *j;
            while ( a.a() == b.a() &&
                    a.getOfs() + drec( a )->lengthWithHeaders() == b.getOfs() ) {

                // a & b are adjacent.  merge.
                getDur().writingInt( drec(a)->lengthWithHeaders() ) += drec(b)->lengthWithHeaders();
                j++;
                if ( j == drecs.end() ) {
                    DDD( "\t compact adddelrec2" );
                    addDeletedRec(a);
                    return;
                }
                b = *j;
            }
            DDD( "\t compact adddelrec3" );
            addDeletedRec(a);
            a = b;
        }

    }

    const DiskLoc &CappedRecordStoreV1::cappedFirstDeletedInCurExtent() const {
        if ( cappedLastDelRecLastExtent().isNull() )
            return cappedListOfAllDeletedRecords();
        else
            return drec(cappedLastDelRecLastExtent())->nextDeleted();
    }

    void CappedRecordStoreV1::setFirstDeletedInCurExtent( const DiskLoc& loc ) const {
        if ( cappedLastDelRecLastExtent().isNull() )
            setListOfAllDeletedRecords( loc );
        else
            getDur().writingDiskLoc( drec(cappedLastDelRecLastExtent())->nextDeleted() ) = loc;
    }

    void CappedRecordStoreV1::cappedCheckMigrate() {
        // migrate old RecordStoreV1MetaData format
        if ( _details->capExtent().a() == 0 && _details->capExtent().getOfs() == 0 ) {
            _details->setCapFirstNewRecord( DiskLoc().setInvalid() );
            // put all the DeletedRecords in cappedListOfAllDeletedRecords()
            for ( int i = 1; i < Buckets; ++i ) {
                DiskLoc first = _details->deletedListEntry( i );
                if ( first.isNull() )
                    continue;
                DiskLoc last = first;
                for (; !drec(last)->nextDeleted().isNull(); last = drec(last)->nextDeleted() );
                getDur().writingDiskLoc(drec(last)->nextDeleted()) = cappedListOfAllDeletedRecords();
                setListOfAllDeletedRecords( first );
                _details->setDeletedListEntry(i,DiskLoc());
            }
            // NOTE cappedLastDelRecLastExtent() set to DiskLoc() in above

            // Last, in case we're killed before getting here
            _details->setCapExtent( _details->firstExtent() );
        }
    }

    bool CappedRecordStoreV1::inCapExtent( const DiskLoc &dl ) const {
        invariant( !dl.isNull() );

        if ( dl.a() != _details->capExtent().a() )
            return false;

        if ( dl.getOfs() < _details->capExtent().getOfs() )
            return false;

        const Extent* e = theCapExtent();
        int end = _details->capExtent().getOfs() + e->length;
        return dl.getOfs() <= end;
    }

    bool CappedRecordStoreV1::nextIsInCapExtent( const DiskLoc &dl ) const {
        invariant( !dl.isNull() );
        DiskLoc next = drec(dl)->nextDeleted();
        if ( next.isNull() )
            return false;
        return inCapExtent( next );
    }

    void CappedRecordStoreV1::advanceCapExtent( const StringData& ns ) {
        // We want cappedLastDelRecLastExtent() to be the last DeletedRecord of the prev cap extent
        // (or DiskLoc() if new capExtent == firstExtent)
        if ( _details->capExtent() == _details->lastExtent() )
            setLastDelRecLastExtent( DiskLoc() );
        else {
            DiskLoc i = cappedFirstDeletedInCurExtent();
            for (; !i.isNull() && nextIsInCapExtent( i ); i = drec(i)->nextDeleted() );
            setLastDelRecLastExtent( i );
        }

        _details->setCapExtent( theCapExtent()->xnext.isNull() ?
                                _details->firstExtent() : theCapExtent()->xnext );

        /* this isn't true if a collection has been renamed...that is ok just used for diagnostics */
        //dassert( theCapExtent()->ns == ns );

        theCapExtent()->assertOk();
        _details->setCapFirstNewRecord( DiskLoc() );
    }

    DiskLoc CappedRecordStoreV1::__capAlloc( int len ) {
        DiskLoc prev = cappedLastDelRecLastExtent();
        DiskLoc i = cappedFirstDeletedInCurExtent();
        DiskLoc ret;
        for (; !i.isNull() && inCapExtent( i ); prev = i, i = drec(i)->nextDeleted() ) {
            // We need to keep at least one DR per extent in cappedListOfAllDeletedRecords(),
            // so make sure there's space to create a DR at the end.
            if ( drec(i)->lengthWithHeaders() >= len + 24 ) {
                ret = i;
                break;
            }
        }

        /* unlink ourself from the deleted list */
        if ( !ret.isNull() ) {
            if ( prev.isNull() )
                setListOfAllDeletedRecords( drec(ret)->nextDeleted() );
            else
                getDur().writingDiskLoc(drec(prev)->nextDeleted()) = drec(ret)->nextDeleted();
            getDur().writingDiskLoc(drec(ret)->nextDeleted()).setInvalid(); // defensive.
            invariant( drec(ret)->extentOfs() < ret.getOfs() );
        }

        return ret;
    }

    void CappedRecordStoreV1::cappedTruncateLastDelUpdate() {
        if ( _details->capExtent() == _details->firstExtent() ) {
            // Only one extent of the collection is in use, so there
            // is no deleted record in a previous extent, so nullify
            // cappedLastDelRecLastExtent().
            setLastDelRecLastExtent( DiskLoc() );
        }
        else {
            // Scan through all deleted records in the collection
            // until the last deleted record for the extent prior
            // to the new capExtent is found.  Then set
            // cappedLastDelRecLastExtent() to that deleted record.
            DiskLoc i = cappedListOfAllDeletedRecords();
            for( ;
                 !drec(i)->nextDeleted().isNull() &&
                     !inCapExtent( drec(i)->nextDeleted() );
                 i = drec(i)->nextDeleted() );
            // In our capped storage model, every extent must have at least one
            // deleted record.  Here we check that 'i' is not the last deleted
            // record.  (We expect that there will be deleted records in the new
            // capExtent as well.)
            invariant( !drec(i)->nextDeleted().isNull() );
            setLastDelRecLastExtent( i );
        }
    }

    void CappedRecordStoreV1::cappedTruncateAfter(const char *ns, DiskLoc end, bool inclusive) {
        invariant( cappedLastDelRecLastExtent().isValid() );

        // We iteratively remove the newest document until the newest document
        // is 'end', then we remove 'end' if requested.
        bool foundLast = false;
        while( 1 ) {
            if ( foundLast ) {
                // 'end' has been found and removed, so break.
                break;
            }
            getDur().commitIfNeeded();
            // 'curr' will point to the newest document in the collection.
            DiskLoc curr = theCapExtent()->lastRecord;
            invariant( !curr.isNull() );
            if ( curr == end ) {
                if ( inclusive ) {
                    // 'end' has been found, so break next iteration.
                    foundLast = true;
                }
                else {
                    // 'end' has been found, so break.
                    break;
                }
            }

            // TODO The algorithm used in this function cannot generate an
            // empty collection, but we could call emptyCappedCollection() in
            // this case instead of asserting.
            uassert( 13415, "emptying the collection is not allowed", _details->numRecords() > 1 );

            // Delete the newest record, and coalesce the new deleted
            // record with existing deleted records.
            _collection->deleteDocument( curr, true );
            compact();

            // This is the case where we have not yet had to remove any
            // documents to make room for other documents, and we are allocating
            // documents from free space in fresh extents instead of reusing
            // space from familiar extents.
            if ( !capLooped() ) {

                // We just removed the last record from the 'capExtent', and
                // the 'capExtent' can't be empty, so we set 'capExtent' to
                // capExtent's prev extent.
                if ( theCapExtent()->lastRecord.isNull() ) {
                    invariant( !theCapExtent()->xprev.isNull() );
                    // NOTE Because we didn't delete the last document, and
                    // capLooped() is false, capExtent is not the first extent
                    // so xprev will be nonnull.
                    _details->setCapExtent( theCapExtent()->xprev );
                    theCapExtent()->assertOk();

                    // update cappedLastDelRecLastExtent()
                    cappedTruncateLastDelUpdate();
                }
                continue;
            }

            // This is the case where capLooped() is true, and we just deleted
            // from capExtent, and we just deleted capFirstNewRecord, which was
            // the last record on the fresh side of capExtent.
            // NOTE In this comparison, curr and potentially capFirstNewRecord
            // may point to invalid data, but we can still compare the
            // references themselves.
            if ( curr == _details->capFirstNewRecord() ) {

                // Set 'capExtent' to the first nonempty extent prior to the
                // initial capExtent.  There must be such an extent because we
                // have not deleted the last document in the collection.  It is
                // possible that all extents other than the capExtent are empty.
                // In this case we will keep the initial capExtent and specify
                // that all records contained within are on the fresh rather than
                // stale side of the extent.
                DiskLoc newCapExtent = _details->capExtent();
                do {
                    // Find the previous extent, looping if necessary.
                    newCapExtent = ( newCapExtent == _details->firstExtent() ) ?
                        _details->lastExtent() :
                        _extentManager->getExtent(newCapExtent)->xprev;
                    _extentManager->getExtent(newCapExtent)->assertOk();
                }
                while ( _extentManager->getExtent(newCapExtent)->firstRecord.isNull() );
                _details->setCapExtent( newCapExtent );

                // Place all documents in the new capExtent on the fresh side
                // of the capExtent by setting capFirstNewRecord to the first
                // document in the new capExtent.
                _details->setCapFirstNewRecord( theCapExtent()->firstRecord );

                // update cappedLastDelRecLastExtent()
                cappedTruncateLastDelUpdate();
            }
        }
    }

    const DiskLoc& CappedRecordStoreV1::cappedListOfAllDeletedRecords() const {
        return _details->deletedListEntry(0);
    }

    void CappedRecordStoreV1::setListOfAllDeletedRecords( const DiskLoc& loc ) const {
        return _details->setDeletedListEntry(0,loc);
    }

    const DiskLoc& CappedRecordStoreV1::cappedLastDelRecLastExtent() const {
        return _details->deletedListEntry(1);
    }

    void CappedRecordStoreV1::setLastDelRecLastExtent( const DiskLoc& loc ) const {
        return _details->setDeletedListEntry(1,loc);
    }

    bool CappedRecordStoreV1::capLooped() const {
        return _details->capFirstNewRecord().isValid();
    }

    Extent* CappedRecordStoreV1::theCapExtent() const {
        return _extentManager->getExtent(_details->capExtent());
    }

    void CappedRecordStoreV1::addDeletedRec( const DiskLoc& dloc ) {
        DeletedRecord* d = drec( dloc );

        {
            Record *r = (Record *) getDur().writingPtr(d, sizeof(Record));
            d = &r->asDeleted();
            // defensive code: try to make us notice if we reference a deleted record
            reinterpret_cast<unsigned*>( r->data() )[0] = 0xeeeeeeee;
        }
        DEBUGGING log() << "TEMP: add deleted rec " << dloc.toString() << ' ' << hex << d->extentOfs() << endl;
        if ( !cappedLastDelRecLastExtent().isValid() ) {
            // Initial extent allocation.  Insert at end.
            d->nextDeleted() = DiskLoc();
            if ( cappedListOfAllDeletedRecords().isNull() )
                setListOfAllDeletedRecords( dloc );
            else {
                DiskLoc i = cappedListOfAllDeletedRecords();
                for (; !drec(i)->nextDeleted().isNull(); i = drec(i)->nextDeleted() )
                    ;
                getDur().writingDiskLoc(drec(i)->nextDeleted()) = dloc;
            }
        }
        else {
            d->nextDeleted() = cappedFirstDeletedInCurExtent();
            setFirstDeletedInCurExtent( dloc );
            // always compact() after this so order doesn't matter
        }
    }

    RecordIterator* CappedRecordStoreV1::getIterator( const DiskLoc& start, bool tailable,
                                                      const CollectionScanParams::Direction& dir) const {
        return new CappedRecordStoreV1Iterator( this, start, tailable, dir );
    }

    Status CappedRecordStoreV1::compact( RecordStoreCompactAdaptor* adaptor,
                                         const CompactOptions* options,
                                         CompactStats* stats ) {
        invariant(false);
    }

    void CappedRecordStoreV1::_maybeComplain( int len ) const {
        RARELY {
            std::stringstream buf;
            buf << "couldn't make room for record len: " << len << " in capped ns " << _ns << '\n';
            int i = 0;
            for ( DiskLoc e = _details->firstExtent();
                  !e.isNull();
                  e = _extentManager->getExtent( e )->xnext, ++i ) {
                buf << "  Extent " << i;
                if ( e == _details->capExtent() )
                    buf << " (capExtent)";
                buf << '\n';

                buf << "    magic: " << hex << _extentManager->getExtent( e )->magic << dec
                    << " extent->ns: " << _extentManager->getExtent( e )->nsDiagnostic.toString()
                    << '\n';
                buf << "    fr: " << _extentManager->getExtent( e )->firstRecord.toString()
                    << " lr: " << _extentManager->getExtent( e )->lastRecord.toString()
                    << " extent->len: " << _extentManager->getExtent( e )->length << '\n';
            }

            warning() << buf.str();

            // assume it is unusually large record; if not, something is broken
            fassert( 17438, len * 5 > _details->lastExtentSize() );
        }
    }

    DiskLoc CappedRecordStoreV1::firstRecord( const DiskLoc &startExtent ) const {
        for (DiskLoc i = startExtent.isNull() ? _details->firstExtent() : startExtent;
                !i.isNull();
             i = _extentManager->getExtent( i )->xnext ) {

            Extent* e = _extentManager->getExtent( i );

            if ( !e->firstRecord.isNull() )
                return e->firstRecord;
        }
        return DiskLoc();
    }

    DiskLoc CappedRecordStoreV1::lastRecord( const DiskLoc &startExtent ) const {
        for (DiskLoc i = startExtent.isNull() ? _details->lastExtent() : startExtent;
                !i.isNull();
             i = _extentManager->getExtent( i )->xprev ) {

            Extent* e = _extentManager->getExtent( i );
            if ( !e->lastRecord.isNull() )
                return e->lastRecord;
        }
        return DiskLoc();
    }

}
