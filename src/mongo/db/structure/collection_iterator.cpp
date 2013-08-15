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
 */

#include "mongo/db/structure/collection_iterator.h"

#include "mongo/db/namespace_details.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/structure/collection.h"

#include "mongo/db/pdfile.h" // XXX-ERH

namespace mongo {

    //
    // Regular / non-capped collection traversal
    //

    FlatIterator::FlatIterator(const CollectionTemp* collection,
                               const DiskLoc& start,
                               const CollectionScanParams::Direction& dir)
        : _curr(start), _collection(collection), _direction(dir) {

        if (_curr.isNull()) {

            const ExtentManager* em = _collection->getExtentManager();

            if (CollectionScanParams::FORWARD == _direction) {

                // Find a non-empty extent and start with the first record in it.
                Extent* e = em->getExtent( _collection->_details->firstExtent() );

                while (e->firstRecord.isNull() && !e->xnext.isNull()) {
                    e = em->getNextExtent( e );
                }

                // _curr may be set to DiskLoc() here if e->lastRecord isNull but there is no
                // valid e->xnext
                _curr = e->firstRecord;
            }
            else {
                // Walk backwards, skipping empty extents, and use the last record in the first
                // non-empty extent we see.
                Extent* e = em->getExtent( _collection->_details->lastExtent() );

                // TODO ELABORATE
                // Does one of e->lastRecord.isNull(), e.firstRecord.isNull() imply the other?
                while (e->lastRecord.isNull() && !e->xprev.isNull()) {
                    e = em->getPrevExtent( e );
                }

                // _curr may be set to DiskLoc() here if e->lastRecord isNull but there is no
                // valid e->xprev
                _curr = e->lastRecord;
            }
        }
    }

    bool FlatIterator::isEOF() {
        return _curr.isNull();
    }

    DiskLoc FlatIterator::getNext() {
        DiskLoc ret = _curr;

        // Move to the next thing.
        if (!isEOF()) {
            if (CollectionScanParams::FORWARD == _direction) {
                _curr = _collection->getExtentManager()->getNextRecord( _curr );
            }
            else {
                _curr = _collection->getExtentManager()->getPrevRecord( _curr );
            }
        }

        return ret;
    }

    void FlatIterator::invalidate(const DiskLoc& dl) {
        verify( _collection->ok() );

        // Just move past the thing being deleted.
        if (dl == _curr) {
            // We don't care about the return of getNext so much as the side effect of moving _curr
            // to the 'next' thing.
            getNext();
        }
    }

    void FlatIterator::prepareToYield() {
    }

    bool FlatIterator::recoverFromYield() {
        // if the collection is dropped, then the cursor should be destroyed
        // this check is just a sanity check that the Collection instance we're about to use
        // has need been destroyed
        verify( _collection->ok() );

        return true;
    }

    //
    // Capped collection traversal
    //

    CappedIterator::CappedIterator(const string& ns, const DiskLoc& start, bool tailable,
                                   const CollectionScanParams::Direction& dir)
        : _ns(ns), _curr(start), _tailable(tailable), _direction(dir), _killedByInvalidate(false) {

        _details = nsdetails(ns);
        verify(NULL != _details);

        if (_curr.isNull()) {
            // If a start position isn't specified, we fill one out from the start of the
            // collection.
            if (CollectionScanParams::FORWARD == _direction) {
                // Going forwards.
                if (!_details->capLooped()) {
                    // If our capped collection doesn't loop around, the first record is easy.
                    _curr = _details->firstRecord();
                }
                else {
                    // Our capped collection has "looped' around.
                    // Copied verbatim from ForwardCappedCursor::init.
                    // TODO ELABORATE
                    _curr = _details->capExtent().ext()->firstRecord;
                    if (!_curr.isNull() && _curr == _details->capFirstNewRecord()) {
                        _curr = _details->capExtent().ext()->lastRecord;
                        _curr = nextLoop(_details, _curr);
                    }
                }
            }
            else {
                // Going backwards
                if (!_details->capLooped()) {
                    // Start at the end.
                    _curr = _details->lastRecord();
                }
                else {
                    _curr = _details->capExtent().ext()->lastRecord;
                }
            }
        }
    }

    bool CappedIterator::isEOF() { return _curr.isNull(); }

    DiskLoc CappedIterator::getNext() {
        DiskLoc ret = _curr;

        // Move to the next thing.
        if (!isEOF()) {
            _curr = getNextCapped(_curr, _direction, _details);
        }
        else if (_tailable && !_prev.isNull()) {
            // If we're tailable, there COULD have been something inserted even though we were
            // previously EOF.  Look at the next thing from 'prev' and see.
            DiskLoc newCurr = getNextCapped(_prev, _direction, _details);

            if (!newCurr.isNull()) {
                // There's something new to return.  _curr always points to the next thing to
                // return.  Update it, and move _prev to the thing we just returned.
                ret = _prev = newCurr;
                _curr = getNextCapped(_prev, _direction, _details);
            }
        }

        return ret;
    }

    void CappedIterator::invalidate(const DiskLoc& dl) {
        if ((_tailable && _curr.isNull() && dl == _prev) || (dl == _curr)) {
            // In the _tailable case, we're about to kill the DiskLoc that we're tailing.  Nothing
            // that we can possibly do to survive that.
            //
            // In the _curr case, we *could* move to the next thing, since there is actually a next
            // thing, but according to clientcursor.cpp:
            // "note we cannot advance here. if this condition occurs, writes to the oplog
            //  have "caught" the reader.  skipping ahead, the reader would miss postentially
            //  important data."
            _curr = _prev = DiskLoc();
            _killedByInvalidate = true;
        }
    }

    void CappedIterator::prepareToYield() {
        _details = NULL;
    }

    bool CappedIterator::recoverFromYield() {
        // If invalidate invalidated the DiskLoc we relied on, give up now.
        if (_killedByInvalidate) {
            return false;
        }

        _details = nsdetails(_ns);

        // If the collection was deleted from under us, stop.
        if (NULL == _details) {
            // For paranoia's sake force EOF.
            _curr = _prev = DiskLoc();
            return false;
        }

        return true;
    }

    DiskLoc CappedIterator::getNextCapped(const DiskLoc& dl,
                                          CollectionScanParams::Direction direction,
                                          NamespaceDetails* nsd) {
        verify(!dl.isNull());

        if (CollectionScanParams::FORWARD == direction) {
            // If it's not looped, it's easy.
            if (!nsd->capLooped()) { return dl.rec()->getNext(dl); }

            // TODO ELABORATE
            // EOF.
            if (dl == nsd->capExtent().ext()->lastRecord) { return DiskLoc(); }

            DiskLoc ret = nextLoop(nsd, dl);

            // If we become capFirstNewRecord from same extent, advance to next extent.
            if (ret == nsd->capFirstNewRecord() && ret != nsd->capExtent().ext()->firstRecord) {
                ret = nextLoop(nsd, nsd->capExtent().ext()->lastRecord);
            }

            // If we have just gotten to beginning of capExtent, skip to capFirstNewRecord
            if (ret == nsd->capExtent().ext()->firstRecord) { ret = nsd->capFirstNewRecord(); }

            return ret;
        }
        else {
            if (!nsd->capLooped()) { return dl.rec()->getPrev(dl); }

            // TODO ELABORATE
            // Last record
            if (nsd->capFirstNewRecord() == nsd->capExtent().ext()->firstRecord) {
                if (dl == nextLoop(nsd, nsd->capExtent().ext()->lastRecord)) {
                    return DiskLoc();
                }
            }
            else {
                if (dl == nsd->capExtent().ext()->firstRecord) { return DiskLoc(); }
            }

            DiskLoc ret;
            // If we are capFirstNewRecord, advance to prev extent, otherwise just get prev.
            if (dl == nsd->capFirstNewRecord()) {
                ret = prevLoop(nsd, nsd->capExtent().ext()->firstRecord);
            }
            else {
                ret = prevLoop(nsd, dl);
            }

            // If we just became last in cap extent, advance past capFirstNewRecord
            // (We know capExtent.ext()->firstRecord != capFirstNewRecord, since would
            // have returned DiskLoc() earlier otherwise.)
            if (ret == nsd->capExtent().ext()->lastRecord) {
                ret = nsd->capFirstNewRecord().rec()->getPrev(nsd->capFirstNewRecord());
            }

            return ret;
        }
    }

    DiskLoc CappedIterator::nextLoop(NamespaceDetails* nsd, const DiskLoc& prev) {
        // TODO ELABORATE
        verify(nsd->capLooped());
        DiskLoc next = prev.rec()->getNext(prev);
        if (!next.isNull()) { return next; }
        return nsd->firstRecord();
    }

    DiskLoc CappedIterator::prevLoop(NamespaceDetails* nsd, const DiskLoc& curr) {
        // TODO ELABORATE
        verify(nsd->capLooped());
        DiskLoc prev = curr.rec()->getPrev(curr);
        if (!prev.isNull()) { return prev; }
        return nsd->lastRecord();
    }

}  // namespace mongo
