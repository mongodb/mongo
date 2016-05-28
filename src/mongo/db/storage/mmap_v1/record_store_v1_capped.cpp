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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/mmap_v1/record_store_v1_capped.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/storage/mmap_v1/mmap.h"
#include "mongo/db/storage/mmap_v1/record.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_capped_iterator.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

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

using std::dec;
using std::endl;
using std::hex;
using std::vector;

CappedRecordStoreV1::CappedRecordStoreV1(OperationContext* txn,
                                         CappedCallback* collection,
                                         StringData ns,
                                         RecordStoreV1MetaData* details,
                                         ExtentManager* em,
                                         bool isSystemIndexes)
    : RecordStoreV1Base(ns, details, em, isSystemIndexes), _cappedCallback(collection) {
    DiskLoc extentLoc = details->firstExtent(txn);
    while (!extentLoc.isNull()) {
        _extentAdvice.push_back(_extentManager->cacheHint(extentLoc, ExtentManager::Sequential));
        Extent* extent = em->getExtent(extentLoc);
        extentLoc = extent->xnext;
    }

    // this is for VERY VERY old versions of capped collections
    cappedCheckMigrate(txn);
}

CappedRecordStoreV1::~CappedRecordStoreV1() {}

StatusWith<DiskLoc> CappedRecordStoreV1::allocRecord(OperationContext* txn,
                                                     int lenToAlloc,
                                                     bool enforceQuota) {
    {
        // align very slightly.
        lenToAlloc = (lenToAlloc + 3) & 0xfffffffc;
    }

    if (lenToAlloc > theCapExtent()->length) {
        // the extent check is a way to try and improve performance
        // since we have to iterate all the extents (for now) to get
        // storage size
        if (lenToAlloc > storageSize(txn)) {
            return StatusWith<DiskLoc>(
                ErrorCodes::DocTooLargeForCapped,
                mongoutils::str::stream() << "document is larger than capped size " << lenToAlloc
                                          << " > "
                                          << storageSize(txn),
                16328);
        }
    }
    DiskLoc loc;
    {  // do allocation

        // signal done allocating new extents.
        if (!cappedLastDelRecLastExtent().isValid())
            setLastDelRecLastExtent(txn, DiskLoc());

        invariant(lenToAlloc < 400000000);
        int passes = 0;

        // delete records until we have room and the max # objects limit achieved.

        /* this fails on a rename -- that is ok but must keep commented out */
        // invariant( theCapExtent()->ns == ns );

        theCapExtent()->assertOk();
        DiskLoc firstEmptyExtent;  // This prevents us from infinite looping.
        while (1) {
            if (_details->numRecords() < _details->maxCappedDocs()) {
                loc = __capAlloc(txn, lenToAlloc);
                if (!loc.isNull())
                    break;
            }

            // If on first iteration through extents, don't delete anything.
            if (!_details->capFirstNewRecord().isValid()) {
                advanceCapExtent(txn, _ns);

                if (_details->capExtent() != _details->firstExtent(txn))
                    _details->setCapFirstNewRecord(txn, DiskLoc().setInvalid());
                // else signal done with first iteration through extents.
                continue;
            }

            if (!_details->capFirstNewRecord().isNull() &&
                theCapExtent()->firstRecord == _details->capFirstNewRecord()) {
                // We've deleted all records that were allocated on the previous
                // iteration through this extent.
                advanceCapExtent(txn, _ns);
                continue;
            }

            if (theCapExtent()->firstRecord.isNull()) {
                if (firstEmptyExtent.isNull())
                    firstEmptyExtent = _details->capExtent();
                advanceCapExtent(txn, _ns);
                if (firstEmptyExtent == _details->capExtent()) {
                    // All records have been deleted but there is still no room for this record.
                    // Nothing we can do but fail.
                    _maybeComplain(txn, lenToAlloc);
                    return StatusWith<DiskLoc>(ErrorCodes::DocTooLargeForCapped,
                                               str::stream()
                                                   << "document doesn't fit in capped collection."
                                                   << " size: "
                                                   << lenToAlloc
                                                   << " storageSize:"
                                                   << storageSize(txn),
                                               28575);
                }
                continue;
            }

            const RecordId fr = theCapExtent()->firstRecord.toRecordId();
            Status status = _cappedCallback->aboutToDeleteCapped(txn, fr, dataFor(txn, fr));
            if (!status.isOK())
                return StatusWith<DiskLoc>(status);
            deleteRecord(txn, fr);

            _compact(txn);
            if ((++passes % 5000) == 0) {
                StringBuilder sb;
                log() << "passes = " << passes << " in CappedRecordStoreV1::allocRecord:"
                      << " ns: " << _ns << ", lenToAlloc: " << lenToAlloc
                      << ", maxCappedDocs: " << _details->maxCappedDocs()
                      << ", nrecords: " << _details->numRecords()
                      << ", datasize: " << _details->dataSize()
                      << ". Continuing to delete old records to make room.";
            }
        }

        // Remember first record allocated on this iteration through capExtent.
        if (_details->capFirstNewRecord().isValid() && _details->capFirstNewRecord().isNull())
            _details->setCapFirstNewRecord(txn, loc);
    }

    invariant(!loc.isNull());

    // possibly slice up if we've allocated too much space

    DeletedRecord* r = drec(loc);

    /* note we want to grab from the front so our next pointers on disk tend
    to go in a forward direction which is important for performance. */
    int regionlen = r->lengthWithHeaders();
    invariant(r->extentOfs() < loc.getOfs());

    int left = regionlen - lenToAlloc;

    /* split off some for further use. */
    txn->recoveryUnit()->writingInt(r->lengthWithHeaders()) = lenToAlloc;
    DiskLoc newDelLoc = loc;
    newDelLoc.inc(lenToAlloc);
    DeletedRecord* newDel = drec(newDelLoc);
    DeletedRecord* newDelW = txn->recoveryUnit()->writing(newDel);
    newDelW->extentOfs() = r->extentOfs();
    newDelW->lengthWithHeaders() = left;
    newDelW->nextDeleted().Null();

    addDeletedRec(txn, newDelLoc);

    return StatusWith<DiskLoc>(loc);
}

Status CappedRecordStoreV1::truncate(OperationContext* txn) {
    setLastDelRecLastExtent(txn, DiskLoc());
    setListOfAllDeletedRecords(txn, DiskLoc());

    // preserve firstExtent/lastExtent
    _details->setCapExtent(txn, _details->firstExtent(txn));
    _details->setStats(txn, 0, 0);
    // preserve lastExtentSize
    // nIndexes preserve 0
    // capped preserve true
    // max preserve
    // paddingFactor is unused
    _details->setCapFirstNewRecord(txn, DiskLoc().setInvalid());
    setLastDelRecLastExtent(txn, DiskLoc().setInvalid());
    // dataFileVersion preserve
    // indexFileVersion preserve

    // Reset all existing extents and recreate the deleted list.
    Extent* ext;
    for (DiskLoc extLoc = _details->firstExtent(txn); !extLoc.isNull(); extLoc = ext->xnext) {
        ext = _extentManager->getExtent(extLoc);

        txn->recoveryUnit()->writing(&ext->firstRecord)->Null();
        txn->recoveryUnit()->writing(&ext->lastRecord)->Null();

        addDeletedRec(txn, _findFirstSpot(txn, extLoc, ext));
    }

    return Status::OK();
}

void CappedRecordStoreV1::temp_cappedTruncateAfter(OperationContext* txn,
                                                   RecordId end,
                                                   bool inclusive) {
    cappedTruncateAfter(txn, _ns.c_str(), DiskLoc::fromRecordId(end), inclusive);
}

/* combine adjacent deleted records *for the current extent* of the capped collection

   this is O(n^2) but we call it for capped tables where typically n==1 or 2!
   (or 3...there will be a little unused sliver at the end of the extent.)
*/
void CappedRecordStoreV1::_compact(OperationContext* txn) {
    DDD("CappedRecordStoreV1::compact enter");

    vector<DiskLoc> drecs;

    // Pull out capExtent's DRs from deletedList
    DiskLoc i = cappedFirstDeletedInCurExtent();
    for (; !i.isNull() && inCapExtent(i); i = deletedRecordFor(i)->nextDeleted()) {
        DDD("\t" << i);
        drecs.push_back(i);
    }

    setFirstDeletedInCurExtent(txn, i);

    std::sort(drecs.begin(), drecs.end());
    DDD("\t drecs.size(): " << drecs.size());

    vector<DiskLoc>::const_iterator j = drecs.begin();
    invariant(j != drecs.end());
    DiskLoc a = *j;
    while (1) {
        j++;
        if (j == drecs.end()) {
            DDD("\t compact adddelrec");
            addDeletedRec(txn, a);
            break;
        }
        DiskLoc b = *j;
        while (a.a() == b.a() && a.getOfs() + drec(a)->lengthWithHeaders() == b.getOfs()) {
            // a & b are adjacent.  merge.
            txn->recoveryUnit()->writingInt(drec(a)->lengthWithHeaders()) +=
                drec(b)->lengthWithHeaders();
            j++;
            if (j == drecs.end()) {
                DDD("\t compact adddelrec2");
                addDeletedRec(txn, a);
                return;
            }
            b = *j;
        }
        DDD("\t compact adddelrec3");
        addDeletedRec(txn, a);
        a = b;
    }
}

DiskLoc CappedRecordStoreV1::cappedFirstDeletedInCurExtent() const {
    if (cappedLastDelRecLastExtent().isNull())
        return cappedListOfAllDeletedRecords();
    else
        return drec(cappedLastDelRecLastExtent())->nextDeleted();
}

void CappedRecordStoreV1::setFirstDeletedInCurExtent(OperationContext* txn, const DiskLoc& loc) {
    if (cappedLastDelRecLastExtent().isNull())
        setListOfAllDeletedRecords(txn, loc);
    else
        *txn->recoveryUnit()->writing(&drec(cappedLastDelRecLastExtent())->nextDeleted()) = loc;
}

void CappedRecordStoreV1::cappedCheckMigrate(OperationContext* txn) {
    // migrate old RecordStoreV1MetaData format
    if (_details->capExtent().a() == 0 && _details->capExtent().getOfs() == 0) {
        WriteUnitOfWork wunit(txn);
        _details->setCapFirstNewRecord(txn, DiskLoc().setInvalid());
        // put all the DeletedRecords in cappedListOfAllDeletedRecords()
        for (int i = 1; i < Buckets; ++i) {
            DiskLoc first = _details->deletedListEntry(i);
            if (first.isNull())
                continue;
            DiskLoc last = first;
            for (; !drec(last)->nextDeleted().isNull(); last = drec(last)->nextDeleted())
                ;
            *txn->recoveryUnit()->writing(&drec(last)->nextDeleted()) =
                cappedListOfAllDeletedRecords();
            setListOfAllDeletedRecords(txn, first);
            _details->setDeletedListEntry(txn, i, DiskLoc());
        }
        // NOTE cappedLastDelRecLastExtent() set to DiskLoc() in above

        // Last, in case we're killed before getting here
        _details->setCapExtent(txn, _details->firstExtent(txn));
        wunit.commit();
    }
}

bool CappedRecordStoreV1::inCapExtent(const DiskLoc& dl) const {
    invariant(!dl.isNull());

    if (dl.a() != _details->capExtent().a())
        return false;

    if (dl.getOfs() < _details->capExtent().getOfs())
        return false;

    const Extent* e = theCapExtent();
    int end = _details->capExtent().getOfs() + e->length;
    return dl.getOfs() <= end;
}

bool CappedRecordStoreV1::nextIsInCapExtent(const DiskLoc& dl) const {
    invariant(!dl.isNull());
    DiskLoc next = drec(dl)->nextDeleted();
    if (next.isNull())
        return false;
    return inCapExtent(next);
}

void CappedRecordStoreV1::advanceCapExtent(OperationContext* txn, StringData ns) {
    // We want cappedLastDelRecLastExtent() to be the last DeletedRecord of the prev cap extent
    // (or DiskLoc() if new capExtent == firstExtent)
    if (_details->capExtent() == _details->lastExtent(txn))
        setLastDelRecLastExtent(txn, DiskLoc());
    else {
        DiskLoc i = cappedFirstDeletedInCurExtent();
        for (; !i.isNull() && nextIsInCapExtent(i); i = drec(i)->nextDeleted())
            ;
        setLastDelRecLastExtent(txn, i);
    }

    _details->setCapExtent(
        txn, theCapExtent()->xnext.isNull() ? _details->firstExtent(txn) : theCapExtent()->xnext);

    /* this isn't true if a collection has been renamed...that is ok just used for diagnostics */
    // dassert( theCapExtent()->ns == ns );

    theCapExtent()->assertOk();
    _details->setCapFirstNewRecord(txn, DiskLoc());
}

DiskLoc CappedRecordStoreV1::__capAlloc(OperationContext* txn, int len) {
    DiskLoc prev = cappedLastDelRecLastExtent();
    DiskLoc i = cappedFirstDeletedInCurExtent();
    DiskLoc ret;
    for (; !i.isNull() && inCapExtent(i); prev = i, i = drec(i)->nextDeleted()) {
        // We need to keep at least one DR per extent in cappedListOfAllDeletedRecords(),
        // so make sure there's space to create a DR at the end.
        if (drec(i)->lengthWithHeaders() >= len + 24) {
            ret = i;
            break;
        }
    }

    /* unlink ourself from the deleted list */
    if (!ret.isNull()) {
        if (prev.isNull())
            setListOfAllDeletedRecords(txn, drec(ret)->nextDeleted());
        else
            *txn->recoveryUnit()->writing(&drec(prev)->nextDeleted()) = drec(ret)->nextDeleted();
        *txn->recoveryUnit()->writing(&drec(ret)->nextDeleted()) =
            DiskLoc().setInvalid();  // defensive.
        invariant(drec(ret)->extentOfs() < ret.getOfs());
    }

    return ret;
}

void CappedRecordStoreV1::cappedTruncateLastDelUpdate(OperationContext* txn) {
    if (_details->capExtent() == _details->firstExtent(txn)) {
        // Only one extent of the collection is in use, so there
        // is no deleted record in a previous extent, so nullify
        // cappedLastDelRecLastExtent().
        setLastDelRecLastExtent(txn, DiskLoc());
    } else {
        // Scan through all deleted records in the collection
        // until the last deleted record for the extent prior
        // to the new capExtent is found.  Then set
        // cappedLastDelRecLastExtent() to that deleted record.
        DiskLoc i = cappedListOfAllDeletedRecords();
        for (; !drec(i)->nextDeleted().isNull() && !inCapExtent(drec(i)->nextDeleted());
             i = drec(i)->nextDeleted())
            ;
        // In our capped storage model, every extent must have at least one
        // deleted record.  Here we check that 'i' is not the last deleted
        // record.  (We expect that there will be deleted records in the new
        // capExtent as well.)
        invariant(!drec(i)->nextDeleted().isNull());
        setLastDelRecLastExtent(txn, i);
    }
}

void CappedRecordStoreV1::cappedTruncateAfter(OperationContext* txn,
                                              const char* ns,
                                              DiskLoc end,
                                              bool inclusive) {
    invariant(cappedLastDelRecLastExtent().isValid());

    // We iteratively remove the newest document until the newest document
    // is 'end', then we remove 'end' if requested.
    bool foundLast = false;
    while (1) {
        if (foundLast) {
            // 'end' has been found and removed, so break.
            break;
        }
        // 'curr' will point to the newest document in the collection.
        const DiskLoc curr = theCapExtent()->lastRecord;
        const RecordId currId = curr.toRecordId();
        invariant(!curr.isNull());
        if (curr == end) {
            if (inclusive) {
                // 'end' has been found, so break next iteration.
                foundLast = true;
            } else {
                // 'end' has been found, so break.
                break;
            }
        }

        // TODO The algorithm used in this function cannot generate an
        // empty collection, but we could call emptyCappedCollection() in
        // this case instead of asserting.
        uassert(13415, "emptying the collection is not allowed", _details->numRecords() > 1);

        WriteUnitOfWork wunit(txn);
        // Delete the newest record, and coalesce the new deleted
        // record with existing deleted records.
        Status status = _cappedCallback->aboutToDeleteCapped(txn, currId, dataFor(txn, currId));
        uassertStatusOK(status);
        deleteRecord(txn, currId);
        _compact(txn);

        // This is the case where we have not yet had to remove any
        // documents to make room for other documents, and we are allocating
        // documents from free space in fresh extents instead of reusing
        // space from familiar extents.
        if (!_details->capLooped()) {
            // We just removed the last record from the 'capExtent', and
            // the 'capExtent' can't be empty, so we set 'capExtent' to
            // capExtent's prev extent.
            if (theCapExtent()->lastRecord.isNull()) {
                invariant(!theCapExtent()->xprev.isNull());
                // NOTE Because we didn't delete the last document, and
                // capLooped() is false, capExtent is not the first extent
                // so xprev will be nonnull.
                _details->setCapExtent(txn, theCapExtent()->xprev);
                theCapExtent()->assertOk();

                // update cappedLastDelRecLastExtent()
                cappedTruncateLastDelUpdate(txn);
            }
            wunit.commit();
            continue;
        }

        // This is the case where capLooped() is true, and we just deleted
        // from capExtent, and we just deleted capFirstNewRecord, which was
        // the last record on the fresh side of capExtent.
        // NOTE In this comparison, curr and potentially capFirstNewRecord
        // may point to invalid data, but we can still compare the
        // references themselves.
        if (curr == _details->capFirstNewRecord()) {
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
                newCapExtent = (newCapExtent == _details->firstExtent(txn))
                    ? _details->lastExtent(txn)
                    : _extentManager->getExtent(newCapExtent)->xprev;
                _extentManager->getExtent(newCapExtent)->assertOk();
            } while (_extentManager->getExtent(newCapExtent)->firstRecord.isNull());
            _details->setCapExtent(txn, newCapExtent);

            // Place all documents in the new capExtent on the fresh side
            // of the capExtent by setting capFirstNewRecord to the first
            // document in the new capExtent.
            _details->setCapFirstNewRecord(txn, theCapExtent()->firstRecord);

            // update cappedLastDelRecLastExtent()
            cappedTruncateLastDelUpdate(txn);
        }

        wunit.commit();
    }
}

DiskLoc CappedRecordStoreV1::cappedListOfAllDeletedRecords() const {
    return _details->deletedListEntry(0);
}

void CappedRecordStoreV1::setListOfAllDeletedRecords(OperationContext* txn, const DiskLoc& loc) {
    return _details->setDeletedListEntry(txn, 0, loc);
}

DiskLoc CappedRecordStoreV1::cappedLastDelRecLastExtent() const {
    return _details->deletedListEntry(1);
}

void CappedRecordStoreV1::setLastDelRecLastExtent(OperationContext* txn, const DiskLoc& loc) {
    return _details->setDeletedListEntry(txn, 1, loc);
}

Extent* CappedRecordStoreV1::theCapExtent() const {
    return _extentManager->getExtent(_details->capExtent());
}

void CappedRecordStoreV1::addDeletedRec(OperationContext* txn, const DiskLoc& dloc) {
    DeletedRecord* d = txn->recoveryUnit()->writing(drec(dloc));

    if (!cappedLastDelRecLastExtent().isValid()) {
        // Initial extent allocation.  Insert at end.
        d->nextDeleted() = DiskLoc();
        if (cappedListOfAllDeletedRecords().isNull())
            setListOfAllDeletedRecords(txn, dloc);
        else {
            DiskLoc i = cappedListOfAllDeletedRecords();
            for (; !drec(i)->nextDeleted().isNull(); i = drec(i)->nextDeleted())
                ;
            *txn->recoveryUnit()->writing(&drec(i)->nextDeleted()) = dloc;
        }
    } else {
        d->nextDeleted() = cappedFirstDeletedInCurExtent();
        setFirstDeletedInCurExtent(txn, dloc);
        // always _compact() after this so order doesn't matter
    }
}

std::unique_ptr<SeekableRecordCursor> CappedRecordStoreV1::getCursor(OperationContext* txn,
                                                                     bool forward) const {
    return stdx::make_unique<CappedRecordStoreV1Iterator>(txn, this, forward);
}

vector<std::unique_ptr<RecordCursor>> CappedRecordStoreV1::getManyCursors(
    OperationContext* txn) const {
    vector<std::unique_ptr<RecordCursor>> cursors;

    if (!_details->capLooped()) {
        // if we haven't looped yet, just spit out all extents (same as non-capped impl)
        const Extent* ext;
        for (DiskLoc extLoc = details()->firstExtent(txn); !extLoc.isNull(); extLoc = ext->xnext) {
            ext = _getExtent(txn, extLoc);
            if (ext->firstRecord.isNull())
                continue;

            cursors.push_back(stdx::make_unique<RecordStoreV1Base::IntraExtentIterator>(
                txn, ext->firstRecord, this));
        }
    } else {
        // if we've looped we need to iterate the extents, starting and ending with the
        // capExtent
        const DiskLoc capExtent = details()->capExtent();
        invariant(!capExtent.isNull());
        invariant(capExtent.isValid());

        // First do the "old" portion of capExtent if there is any
        DiskLoc extLoc = capExtent;
        {
            const Extent* ext = _getExtent(txn, extLoc);
            if (ext->firstRecord != details()->capFirstNewRecord()) {
                // this means there is old data in capExtent
                cursors.push_back(stdx::make_unique<RecordStoreV1Base::IntraExtentIterator>(
                    txn, ext->firstRecord, this));
            }

            extLoc = ext->xnext.isNull() ? details()->firstExtent(txn) : ext->xnext;
        }

        // Next handle all the other extents
        while (extLoc != capExtent) {
            const Extent* ext = _getExtent(txn, extLoc);
            cursors.push_back(stdx::make_unique<RecordStoreV1Base::IntraExtentIterator>(
                txn, ext->firstRecord, this));

            extLoc = ext->xnext.isNull() ? details()->firstExtent(txn) : ext->xnext;
        }

        // Finally handle the "new" data in the capExtent
        cursors.push_back(stdx::make_unique<RecordStoreV1Base::IntraExtentIterator>(
            txn, details()->capFirstNewRecord(), this));
    }

    return cursors;
}

void CappedRecordStoreV1::_maybeComplain(OperationContext* txn, int len) const {
    RARELY {
        std::stringstream buf;
        buf << "couldn't make room for record len: " << len << " in capped ns " << _ns << '\n';
        buf << "numRecords: " << numRecords(txn) << '\n';
        int i = 0;
        for (DiskLoc e = _details->firstExtent(txn); !e.isNull();
             e = _extentManager->getExtent(e)->xnext, ++i) {
            buf << "  Extent " << i;
            if (e == _details->capExtent())
                buf << " (capExtent)";
            buf << ' ' << e;
            buf << '\n';

            buf << "    magic: " << hex << _extentManager->getExtent(e)->magic << dec
                << " extent->ns: " << _extentManager->getExtent(e)->nsDiagnostic.toString() << '\n';
            buf << "    fr: " << _extentManager->getExtent(e)->firstRecord.toString()
                << " lr: " << _extentManager->getExtent(e)->lastRecord.toString()
                << " extent->len: " << _extentManager->getExtent(e)->length << '\n';
        }

        warning() << buf.str();

        // assume it is unusually large record; if not, something is broken
        fassert(17438, len * 5 > _details->lastExtentSize(txn));
    }
}

DiskLoc CappedRecordStoreV1::firstRecord(OperationContext* txn, const DiskLoc& startExtent) const {
    for (DiskLoc i = startExtent.isNull() ? _details->firstExtent(txn) : startExtent; !i.isNull();
         i = _extentManager->getExtent(i)->xnext) {
        Extent* e = _extentManager->getExtent(i);

        if (!e->firstRecord.isNull())
            return e->firstRecord;
    }
    return DiskLoc();
}

DiskLoc CappedRecordStoreV1::lastRecord(OperationContext* txn, const DiskLoc& startExtent) const {
    for (DiskLoc i = startExtent.isNull() ? _details->lastExtent(txn) : startExtent; !i.isNull();
         i = _extentManager->getExtent(i)->xprev) {
        Extent* e = _extentManager->getExtent(i);
        if (!e->lastRecord.isNull())
            return e->lastRecord;
    }
    return DiskLoc();
}
}
