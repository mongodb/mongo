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

#include "mongo/db/storage/mmap_v1/record_store_v1_capped_iterator.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_capped.h"

namespace mongo {


//
// Capped collection traversal
//
CappedRecordStoreV1Iterator::CappedRecordStoreV1Iterator(OperationContext* txn,
                                                         const CappedRecordStoreV1* collection,
                                                         bool forward)
    : _txn(txn), _recordStore(collection), _forward(forward) {
    const RecordStoreV1MetaData* nsd = _recordStore->details();

    // If a start position isn't specified, we fill one out from the start of the
    // collection.
    if (_forward) {
        // Going forwards.
        if (!nsd->capLooped()) {
            // If our capped collection doesn't loop around, the first record is easy.
            _curr = collection->firstRecord(_txn);
        } else {
            // Our capped collection has "looped' around.
            // Copied verbatim from ForwardCappedCursor::init.
            // TODO ELABORATE
            _curr = _getExtent(nsd->capExtent())->firstRecord;
            if (!_curr.isNull() && _curr == nsd->capFirstNewRecord()) {
                _curr = _getExtent(nsd->capExtent())->lastRecord;
                _curr = nextLoop(_curr);
            }
        }
    } else {
        // Going backwards
        if (!nsd->capLooped()) {
            // Start at the end.
            _curr = collection->lastRecord(_txn);
        } else {
            _curr = _getExtent(nsd->capExtent())->lastRecord;
        }
    }
}

boost::optional<Record> CappedRecordStoreV1Iterator::next() {
    if (isEOF())
        return {};
    auto toReturn = _curr.toRecordId();
    _curr = getNextCapped(_curr);
    return {{toReturn, _recordStore->RecordStore::dataFor(_txn, toReturn)}};
}

boost::optional<Record> CappedRecordStoreV1Iterator::seekExact(const RecordId& id) {
    _curr = getNextCapped(DiskLoc::fromRecordId(id));
    return {{id, _recordStore->RecordStore::dataFor(_txn, id)}};
}

void CappedRecordStoreV1Iterator::invalidate(OperationContext* txn, const RecordId& id) {
    const DiskLoc dl = DiskLoc::fromRecordId(id);
    if (dl == _curr) {
        // We *could* move to the next thing, since there is actually a next
        // thing, but according to clientcursor.cpp:
        // "note we cannot advance here. if this condition occurs, writes to the oplog
        //  have "caught" the reader.  skipping ahead, the reader would miss potentially
        //  important data."
        // We don't really need to worry about rollback here, as the very next write would
        // invalidate the cursor anyway.
        _curr = DiskLoc();
        _killedByInvalidate = true;
    }
}

void CappedRecordStoreV1Iterator::save() {}

bool CappedRecordStoreV1Iterator::restore() {
    return !_killedByInvalidate;
}

DiskLoc CappedRecordStoreV1Iterator::getNextCapped(const DiskLoc& dl) {
    invariant(!dl.isNull());
    const RecordStoreV1MetaData* details = _recordStore->details();

    if (_forward) {
        // If it's not looped, it's easy.
        if (!_recordStore->details()->capLooped()) {
            return _getNextRecord(dl);
        }

        // TODO ELABORATE
        // EOF.
        if (dl == _getExtent(details->capExtent())->lastRecord) {
            return DiskLoc();
        }

        DiskLoc ret = nextLoop(dl);

        // If we become capFirstNewRecord from same extent, advance to next extent.
        if (ret == details->capFirstNewRecord() &&
            ret != _getExtent(details->capExtent())->firstRecord) {
            ret = nextLoop(_getExtent(details->capExtent())->lastRecord);
        }

        // If we have just gotten to beginning of capExtent, skip to capFirstNewRecord
        if (ret == _getExtent(details->capExtent())->firstRecord) {
            ret = details->capFirstNewRecord();
        }

        return ret;
    } else {
        if (!details->capLooped()) {
            return _getPrevRecord(dl);
        }

        // TODO ELABORATE
        // Last record
        if (details->capFirstNewRecord() == _getExtent(details->capExtent())->firstRecord) {
            if (dl == nextLoop(_getExtent(details->capExtent())->lastRecord)) {
                return DiskLoc();
            }
        } else {
            if (dl == _getExtent(details->capExtent())->firstRecord) {
                return DiskLoc();
            }
        }

        DiskLoc ret;
        // If we are capFirstNewRecord, advance to prev extent, otherwise just get prev.
        if (dl == details->capFirstNewRecord()) {
            ret = prevLoop(_getExtent(details->capExtent())->firstRecord);
        } else {
            ret = prevLoop(dl);
        }

        // If we just became last in cap extent, advance past capFirstNewRecord
        // (We know ext(capExtent)->firstRecord != capFirstNewRecord, since would
        // have returned DiskLoc() earlier otherwise.)
        if (ret == _getExtent(details->capExtent())->lastRecord) {
            ret = _getPrevRecord(details->capFirstNewRecord());
        }

        return ret;
    }
}

DiskLoc CappedRecordStoreV1Iterator::nextLoop(const DiskLoc& prev) {
    // TODO ELABORATE
    DiskLoc next = _getNextRecord(prev);
    if (!next.isNull()) {
        return next;
    }
    return _recordStore->firstRecord(_txn);
}

DiskLoc CappedRecordStoreV1Iterator::prevLoop(const DiskLoc& curr) {
    // TODO ELABORATE
    DiskLoc prev = _getPrevRecord(curr);
    if (!prev.isNull()) {
        return prev;
    }
    return _recordStore->lastRecord(_txn);
}


Extent* CappedRecordStoreV1Iterator::_getExtent(const DiskLoc& loc) {
    return _recordStore->_extentManager->getExtent(loc);
}

DiskLoc CappedRecordStoreV1Iterator::_getNextRecord(const DiskLoc& loc) {
    return _recordStore->getNextRecord(_txn, loc);
}

DiskLoc CappedRecordStoreV1Iterator::_getPrevRecord(const DiskLoc& loc) {
    return _recordStore->getPrevRecord(_txn, loc);
}

std::unique_ptr<RecordFetcher> CappedRecordStoreV1Iterator::fetcherForNext() const {
    return _recordStore->_extentManager->recordNeedsFetch(_curr);
}

std::unique_ptr<RecordFetcher> CappedRecordStoreV1Iterator::fetcherForId(const RecordId& id) const {
    return _recordStore->_extentManager->recordNeedsFetch(DiskLoc::fromRecordId(id));
}

}  // namespace mongo
