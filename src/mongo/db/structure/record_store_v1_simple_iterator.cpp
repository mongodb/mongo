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

#include "mongo/db/structure/record_store_v1_simple_iterator.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/structure/record_store_v1_simple.h"

namespace mongo {

    //
    // Regular / non-capped collection traversal
    //

    SimpleRecordStoreV1Iterator::SimpleRecordStoreV1Iterator(const SimpleRecordStoreV1* collection,
                                                             const DiskLoc& start,
                                                             const CollectionScanParams::Direction& dir)
        : _curr(start), _recordStore(collection), _direction(dir) {

        if (_curr.isNull()) {

            const ExtentManager* em = _recordStore->_extentManager;

            if ( _recordStore->details()->firstExtent().isNull() ) {
                // nothing in the collection
                verify( _recordStore->details()->lastExtent().isNull() );
            }
            else if (CollectionScanParams::FORWARD == _direction) {

                // Find a non-empty extent and start with the first record in it.
                Extent* e = em->getExtent( _recordStore->details()->firstExtent() );

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
                Extent* e = em->getExtent( _recordStore->details()->lastExtent() );

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

    bool SimpleRecordStoreV1Iterator::isEOF() {
        return _curr.isNull();
    }

    DiskLoc SimpleRecordStoreV1Iterator::curr() { return _curr; }

    DiskLoc SimpleRecordStoreV1Iterator::getNext() {
        DiskLoc ret = _curr;

        // Move to the next thing.
        if (!isEOF()) {
            if (CollectionScanParams::FORWARD == _direction) {
                _curr = _recordStore->_extentManager->getNextRecord( _curr );
            }
            else {
                _curr = _recordStore->_extentManager->getPrevRecord( _curr );
            }
        }

        return ret;
    }

    void SimpleRecordStoreV1Iterator::invalidate(const DiskLoc& dl) {
        // Just move past the thing being deleted.
        if (dl == _curr) {
            // We don't care about the return of getNext so much as the side effect of moving _curr
            // to the 'next' thing.
            getNext();
        }
    }

    void SimpleRecordStoreV1Iterator::prepareToYield() {
    }

    bool SimpleRecordStoreV1Iterator::recoverFromYield() {
        // if the collection is dropped, then the cursor should be destroyed
        return true;
    }

    const Record* SimpleRecordStoreV1Iterator::recordFor( const DiskLoc& loc ) const {
        return _recordStore->recordFor( loc );
    }

}
