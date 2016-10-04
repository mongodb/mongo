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

#pragma once

#include <set>

#include "mongo/db/storage/mmap_v1/record_store_v1_base.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

/**
 * This iterator will go over the collection twice - once going forward (first extent -> last
 * extent) and once backwards in an attempt to salvage potentially corrupted or unreachable
 * records. It is used by the mongodump --repair option.
 */
class RecordStoreV1RepairCursor final : public RecordCursor {
public:
    RecordStoreV1RepairCursor(OperationContext* txn, const RecordStoreV1Base* recordStore);

    boost::optional<Record> next() final;
    void invalidate(OperationContext* txn, const RecordId& dl);
    void save() final {}
    bool restore() final {
        return true;
    }
    void detachFromOperationContext() final {
        _txn = nullptr;
    }
    void reattachToOperationContext(OperationContext* txn) final {
        _txn = txn;
    }

    // Explicitly not supporting fetcherForNext(). The expected use case for this class is a
    // special offline operation where there are no concurrent operations, so it would be better
    // to take the pagefault inline with the operation.

private:
    void advance();

    /**
     * Based on the direction of scan, finds the next valid (un-corrupted) extent in the chain
     * and sets _currExtent to point to that.
     *
     * @return true if valid extent was found (_currExtent will not be null)
     *         false otherwise and _currExtent will be null
     */
    bool _advanceToNextValidExtent();

    // transactional context for read locks. Not owned by us
    OperationContext* _txn;

    // Reference to the owning RecordStore. The store must not be deleted while there are
    // active iterators on it.
    //
    const RecordStoreV1Base* _recordStore;

    DiskLoc _currExtent;
    DiskLoc _currRecord;

    enum Stage { FORWARD_SCAN = 0, BACKWARD_SCAN = 1, DONE = 2 };

    Stage _stage;

    // Used to find cycles within an extent. Cleared after each extent has been processed.
    //
    std::set<DiskLoc> _seenInCurrentExtent;
};

}  // namespace mongo
