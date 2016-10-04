/**
 *    Copyright (C) 2014 10gen Inc.
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

#include "mongo/db/storage/mmap_v1/record_store_v1_repair_iterator.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_simple.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;

RecordStoreV1RepairCursor::RecordStoreV1RepairCursor(OperationContext* txn,
                                                     const RecordStoreV1Base* recordStore)
    : _txn(txn), _recordStore(recordStore), _stage(FORWARD_SCAN) {
    // Position the iterator at the first record
    //
    advance();
}

boost::optional<Record> RecordStoreV1RepairCursor::next() {
    if (_currRecord.isNull())
        return {};
    auto out = _currRecord.toRecordId();
    advance();
    return {{out, _recordStore->dataFor(_txn, out)}};
}

void RecordStoreV1RepairCursor::advance() {
    const ExtentManager* em = _recordStore->_extentManager;

    while (true) {
        if (_currRecord.isNull()) {
            if (!_advanceToNextValidExtent()) {
                return;
            }

            _seenInCurrentExtent.clear();

            // Otherwise _advanceToNextValidExtent would have returned false
            //
            invariant(!_currExtent.isNull());

            const Extent* e = em->getExtent(_currExtent, false);
            _currRecord = (FORWARD_SCAN == _stage ? e->firstRecord : e->lastRecord);
        } else {
            switch (_stage) {
                case FORWARD_SCAN:
                    _currRecord = _recordStore->getNextRecordInExtent(_txn, _currRecord);
                    break;
                case BACKWARD_SCAN:
                    _currRecord = _recordStore->getPrevRecordInExtent(_txn, _currRecord);
                    break;
                default:
                    invariant(!"This should never be reached.");
                    break;
            }
        }

        if (_currRecord.isNull()) {
            continue;
        }

        // Validate the contents of the record's disk location and deduplicate
        //
        if (!_seenInCurrentExtent.insert(_currRecord).second) {
            error() << "infinite loop in extent, seen: " << _currRecord << " before" << endl;
            _currRecord = DiskLoc();
            continue;
        }

        if (_currRecord.getOfs() <= 0) {
            error() << "offset is 0 for record which should be impossible" << endl;
            _currRecord = DiskLoc();
            continue;
        }

        return;
    }
}

bool RecordStoreV1RepairCursor::_advanceToNextValidExtent() {
    const ExtentManager* em = _recordStore->_extentManager;

    while (true) {
        if (_currExtent.isNull()) {
            switch (_stage) {
                case FORWARD_SCAN:
                    _currExtent = _recordStore->details()->firstExtent(_txn);
                    break;
                case BACKWARD_SCAN:
                    _currExtent = _recordStore->details()->lastExtent(_txn);
                    break;
                default:
                    invariant(DONE == _stage);
                    return false;
            }
        } else {
            // If _currExtent is not NULL, then it must point to a valid extent, so no extra
            // checks here.
            //
            const Extent* e = em->getExtent(_currExtent, false);
            _currExtent = (FORWARD_SCAN == _stage ? e->xnext : e->xprev);
        }

        bool hasNextExtent = !_currExtent.isNull();

        // Sanity checks for the extent's disk location
        //
        if (hasNextExtent && (!_currExtent.isValid() || (_currExtent.getOfs() < 0))) {
            error() << "Invalid extent location: " << _currExtent << endl;

            // Switch the direction of scan
            //
            hasNextExtent = false;
        }

        if (hasNextExtent) {
            break;
        }

        // Swap the direction of scan and loop again
        //
        switch (_stage) {
            case FORWARD_SCAN:
                _stage = BACKWARD_SCAN;
                break;
            case BACKWARD_SCAN:
                _stage = DONE;
                break;
            default:
                invariant(!"This should never be reached.");
                break;
        }

        _currExtent = DiskLoc();
    }


    // Check _currExtent's contents for validity, but do not count is as failure if they
    // don't check out.
    //
    const Extent* e = em->getExtent(_currExtent, false);
    if (!e->isOk()) {
        warning() << "Extent not ok magic: " << e->magic << " going to try to continue" << endl;
    }

    log() << (FORWARD_SCAN == _stage ? "FORWARD" : "BACKWARD") << "  Extent loc: " << _currExtent
          << ", length: " << e->length << endl;

    return true;
}

void RecordStoreV1RepairCursor::invalidate(OperationContext* txn, const RecordId& id) {
    // If we see this record again it probably means it was reinserted rather than an infinite
    // loop. If we do loop, we should quickly hit another seen record that hasn't been
    // invalidated.
    DiskLoc dl = DiskLoc::fromRecordId(id);
    _seenInCurrentExtent.erase(dl);

    if (_currRecord == dl) {
        // The DiskLoc being invalidated is also the one pointed at by this iterator. We
        // advance the iterator so it's not pointing at invalid data.
        // We don't worry about undoing invalidations on rollback here, as we shouldn't have
        // concurrent writes that can rollback to a database we're trying to recover.
        advance();

        if (_currRecord == dl) {
            // Even after advancing the iterator, we're still pointing at the DiskLoc being
            // invalidated. This is expected when 'dl' is the last DiskLoc in the FORWARD scan,
            // and the initial call to getNext() moves the iterator to the first loc in the
            // BACKWARDS scan.
            advance();
        }

        invariant(_currRecord != dl);
    }
}

}  // namespace mongo
