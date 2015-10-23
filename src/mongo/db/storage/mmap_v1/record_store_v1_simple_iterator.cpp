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

#include "mongo/db/storage/mmap_v1/record_store_v1_simple_iterator.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_simple.h"

namespace mongo {

//
// Regular / non-capped collection traversal
//

SimpleRecordStoreV1Iterator::SimpleRecordStoreV1Iterator(OperationContext* txn,
                                                         const SimpleRecordStoreV1* collection,
                                                         bool forward)
    : _txn(txn), _recordStore(collection), _forward(forward) {
    // Eagerly seek to first Record on creation since it is cheap.
    const ExtentManager* em = _recordStore->_extentManager;
    if (_recordStore->details()->firstExtent(txn).isNull()) {
        // nothing in the collection
        verify(_recordStore->details()->lastExtent(txn).isNull());
    } else if (_forward) {
        // Find a non-empty extent and start with the first record in it.
        Extent* e = em->getExtent(_recordStore->details()->firstExtent(txn));

        while (e->firstRecord.isNull() && !e->xnext.isNull()) {
            e = em->getExtent(e->xnext);
        }

        // _curr may be set to DiskLoc() here if e->lastRecord isNull but there is no
        // valid e->xnext
        _curr = e->firstRecord;
    } else {
        // Walk backwards, skipping empty extents, and use the last record in the first
        // non-empty extent we see.
        Extent* e = em->getExtent(_recordStore->details()->lastExtent(txn));

        // TODO ELABORATE
        // Does one of e->lastRecord.isNull(), e.firstRecord.isNull() imply the other?
        while (e->lastRecord.isNull() && !e->xprev.isNull()) {
            e = em->getExtent(e->xprev);
        }

        // _curr may be set to DiskLoc() here if e->lastRecord isNull but there is no
        // valid e->xprev
        _curr = e->lastRecord;
    }
}

boost::optional<Record> SimpleRecordStoreV1Iterator::next() {
    if (isEOF())
        return {};
    auto toReturn = _curr.toRecordId();
    advance();
    return {{toReturn, _recordStore->RecordStore::dataFor(_txn, toReturn)}};
}

boost::optional<Record> SimpleRecordStoreV1Iterator::seekExact(const RecordId& id) {
    _curr = DiskLoc::fromRecordId(id);
    advance();
    return {{id, _recordStore->RecordStore::dataFor(_txn, id)}};
}

void SimpleRecordStoreV1Iterator::advance() {
    // Move to the next thing.
    if (!isEOF()) {
        if (_forward) {
            _curr = _recordStore->getNextRecord(_txn, _curr);
        } else {
            _curr = _recordStore->getPrevRecord(_txn, _curr);
        }
    }
}

void SimpleRecordStoreV1Iterator::invalidate(OperationContext* txn, const RecordId& dl) {
    // Just move past the thing being deleted.
    if (dl == _curr.toRecordId()) {
        const DiskLoc origLoc = _curr;

        // Undo the advance on rollback, as the deletion that forced it "never happened".
        txn->recoveryUnit()->onRollback([this, origLoc]() { this->_curr = origLoc; });
        advance();
    }
}

void SimpleRecordStoreV1Iterator::save() {}

bool SimpleRecordStoreV1Iterator::restore() {
    // if the collection is dropped, then the cursor should be destroyed
    return true;
}

std::unique_ptr<RecordFetcher> SimpleRecordStoreV1Iterator::fetcherForNext() const {
    return _recordStore->_extentManager->recordNeedsFetch(_curr);
}

std::unique_ptr<RecordFetcher> SimpleRecordStoreV1Iterator::fetcherForId(const RecordId& id) const {
    return _recordStore->_extentManager->recordNeedsFetch(DiskLoc::fromRecordId(id));
}
}
