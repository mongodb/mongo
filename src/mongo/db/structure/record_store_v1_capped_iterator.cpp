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

#include "mongo/db/structure/record_store_v1_capped_iterator.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/structure/record_store_v1_capped.h"

namespace mongo {


    //
    // Capped collection traversal
    //
    CappedRecordStoreV1Iterator::CappedRecordStoreV1Iterator( const CappedRecordStoreV1* collection,
                                                              const DiskLoc& start, bool tailable,
                                                              const CollectionScanParams::Direction& dir)
        : _recordStore(collection), _curr(start), _tailable(tailable),
          _direction(dir), _killedByInvalidate(false) {

        if (_curr.isNull()) {

            const NamespaceDetails* nsd = _recordStore->details();
            const ExtentManager* em = _recordStore->_extentManager;

            // If a start position isn't specified, we fill one out from the start of the
            // collection.
            if (CollectionScanParams::FORWARD == _direction) {
                // Going forwards.
                if (!nsd->capLooped()) {
                    // If our capped collection doesn't loop around, the first record is easy.
                    _curr = nsd->firstRecord();
                }
                else {
                    // Our capped collection has "looped' around.
                    // Copied verbatim from ForwardCappedCursor::init.
                    // TODO ELABORATE
                    _curr = em->getExtent( nsd->capExtent() )->firstRecord;
                    if (!_curr.isNull() && _curr == nsd->capFirstNewRecord()) {
                        _curr = em->getExtent( nsd->capExtent() )->lastRecord;
                        _curr = nextLoop(nsd,em,_curr);
                    }
                }
            }
            else {
                // Going backwards
                if (!nsd->capLooped()) {
                    // Start at the end.
                    _curr = nsd->lastRecord();
                }
                else {
                    _curr = em->getExtent( nsd->capExtent() )->lastRecord;
                }
            }
        }
    }

    bool CappedRecordStoreV1Iterator::isEOF() { return _curr.isNull(); }

    DiskLoc CappedRecordStoreV1Iterator::curr() { return _curr; }

    DiskLoc CappedRecordStoreV1Iterator::getNext() {
        DiskLoc ret = _curr;

        const NamespaceDetails* nsd = _recordStore->details();
        const ExtentManager* em = _recordStore->_extentManager;

        // Move to the next thing.
        if (!isEOF()) {
            _prev = _curr;
            _curr = getNextCapped(nsd, em, _curr, _direction);
        }
        else if (_tailable && !_prev.isNull()) {
            // If we're tailable, there COULD have been something inserted even though we were
            // previously EOF.  Look at the next thing from 'prev' and see.
            DiskLoc newCurr = getNextCapped(nsd, em, _prev, _direction);

            if (!newCurr.isNull()) {
                // There's something new to return.  _curr always points to the next thing to
                // return.  Update it, and move _prev to the thing we just returned.
                _prev = ret = newCurr;
                _curr = getNextCapped(nsd, em, _prev, _direction);
            }
        }

        return ret;
    }

    void CappedRecordStoreV1Iterator::invalidate(const DiskLoc& dl) {
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

    void CappedRecordStoreV1Iterator::prepareToYield() {
    }

    bool CappedRecordStoreV1Iterator::recoverFromYield() {
        // If invalidate invalidated the DiskLoc we relied on, give up now.
        if (_killedByInvalidate) {
            _recordStore = NULL;
            return false;
        }

        return true;
    }

    DiskLoc CappedRecordStoreV1Iterator::getNextCapped(const NamespaceDetails* nsd, const ExtentManager* em,
                                          const DiskLoc& dl,
                                          CollectionScanParams::Direction direction ) {
        verify(!dl.isNull());

        if (CollectionScanParams::FORWARD == direction) {
            // If it's not looped, it's easy.
            if (!nsd->capLooped()) { return em->getNextRecord( dl ); }

            // TODO ELABORATE
            // EOF.
            if (dl == em->getExtent( nsd->capExtent() )->lastRecord) { return DiskLoc(); }

            DiskLoc ret = nextLoop(nsd,em,dl);

            // If we become capFirstNewRecord from same extent, advance to next extent.
            if (ret == nsd->capFirstNewRecord() && ret != em->getExtent( nsd->capExtent() )->firstRecord) {
                ret = nextLoop(nsd,em,em->getExtent( nsd->capExtent() )->lastRecord);
            }

            // If we have just gotten to beginning of capExtent, skip to capFirstNewRecord
            if (ret == em->getExtent( nsd->capExtent() )->firstRecord) { ret = nsd->capFirstNewRecord(); }

            return ret;
        }
        else {
            if (!nsd->capLooped()) { return em->getPrevRecord( dl ); }

            // TODO ELABORATE
            // Last record
            if (nsd->capFirstNewRecord() == em->getExtent( nsd->capExtent() )->firstRecord) {
                if (dl == nextLoop(nsd,em,em->getExtent( nsd->capExtent() )->lastRecord)) {
                    return DiskLoc();
                }
            }
            else {
                if (dl == em->getExtent( nsd->capExtent() )->firstRecord) { return DiskLoc(); }
            }

            DiskLoc ret;
            // If we are capFirstNewRecord, advance to prev extent, otherwise just get prev.
            if (dl == nsd->capFirstNewRecord()) {
                ret = prevLoop(nsd, em, em->getExtent( nsd->capExtent() )->firstRecord);
            }
            else {
                ret = prevLoop(nsd, em, dl);
            }

            // If we just became last in cap extent, advance past capFirstNewRecord
            // (We know ext(capExtent)->firstRecord != capFirstNewRecord, since would
            // have returned DiskLoc() earlier otherwise.)
            if (ret == em->getExtent( nsd->capExtent() )->lastRecord) {
                ret = em->getPrevRecord( nsd->capFirstNewRecord() );
            }

            return ret;
        }
    }

    DiskLoc CappedRecordStoreV1Iterator::nextLoop(const NamespaceDetails* nsd, const ExtentManager* em,
                                     const DiskLoc& prev) {
        // TODO ELABORATE
        verify(nsd->capLooped());
        DiskLoc next = em->getNextRecord( prev );
        if (!next.isNull()) { return next; }
        return nsd->firstRecord();
    }

    DiskLoc CappedRecordStoreV1Iterator::prevLoop(const NamespaceDetails* nsd, const ExtentManager* em,
                                     const DiskLoc& curr) {
        // TODO ELABORATE
        verify(nsd->capLooped());
        DiskLoc prev = em->getPrevRecord( curr );
        if (!prev.isNull()) { return prev; }
        return nsd->lastRecord();
    }

    const Record* CappedRecordStoreV1Iterator::recordFor( const DiskLoc& loc ) const {
        return _recordStore->recordFor( loc );
    }


}  // namespace mongo
