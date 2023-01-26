/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/platform/basic.h"

#include "mongo/db/record_id.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/decorable.h"

namespace mongo {
class OperationContext;
class RecoveryUnit;
class ServiceContext;

/**
 * A CappedVisibilitySnapshot represents a snapshot of the Records that should and should not be
 * visible for a capped collection.
 */
class CappedVisibilitySnapshot {
public:
    CappedVisibilitySnapshot() = default;

    CappedVisibilitySnapshot(const RecordId& highestRecord, const RecordId& lowestUncommitted)
        : _highestRecord(highestRecord), _lowestUncommittedRecord(lowestUncommitted) {
        invariant(_lowestUncommittedRecord.isNull() || _lowestUncommittedRecord.isLong());
        invariant(_highestRecord.isNull() || _highestRecord.isLong());
    }

    /**
     * Returns true if this RecordId is safely visible in our snapshot.
     */
    bool isRecordVisible(const RecordId& rid) const {
        if (_lowestUncommittedRecord.isNull()) {
            if (_highestRecord.isNull()) {
                return true;
            } else {
                return rid <= _highestRecord;
            }
        }
        return rid < _lowestUncommittedRecord;
    }

    /**
     * Returns the highest RecordId that should be visible in our snapshot. May not represent a
     * RecordId that exists.
     */
    RecordId getHighestVisible() const {
        if (_lowestUncommittedRecord.isNull()) {
            return _highestRecord;
        }
        return RecordId(_lowestUncommittedRecord.getLong() - 1);
    }

    bool hasUncommittedRecords() {
        return !_lowestUncommittedRecord.isNull();
    }

private:
    RecordId _highestRecord;
    RecordId _lowestUncommittedRecord;
};

/**
 * UncommittedRecords hold RecordIds for uncommitted inserts into a capped collection by a single
 * operation. Only valid for the duration of a storage snapshot on a single collection.
 */
class UncommittedRecords {
public:
    UncommittedRecords() = default;

    /**
     * Register a range of RecordIds as allocated and may be committed by this writer in the future.
     * RecordIds must be of the long type. When registering a single RecordId, min and max must be
     * the same.
     */
    void registerRecordIds(const RecordId& min, const RecordId& max);
    void registerRecordId(const RecordId& id) {
        registerRecordIds(id, id);
    }

    /**
     * Returns the lowest uncommitted RecordId of this writer. This is thread safe.
     */
    RecordId getMinRecord() const {
        return RecordId(_min.load());
    }

    /**
     * Returns the highest uncommitted RecordId of this writer. This is not thread safe.
     */
    RecordId getMaxRecord() const {
        return RecordId(_max);
    }

    using Iterator = boost::optional<std::list<UncommittedRecords*>::iterator>;

    /**
     * Returns an iterator to this writer's position in a list owned by the
     * CappedVisibilityObserver.
     */
    Iterator& getIterator() {
        return _it;
    }

    /**
     * Set the iterator to this writer's position in a list.
     */
    void setIterator(Iterator&& it) {
        _it = std::move(it);
    }

    /**
     * Sets an optional function to be called when the uncommitted writes are either committed or
     * aborted. The callback function must not throw.
     */
    using OnCommitOrAbortFn = std::function<void()>;
    void onCommitOrAbort(OnCommitOrAbortFn&& fn) {
        _onCommitOrAbort = std::move(fn);
    }

    void committedOrAborted() noexcept {
        if (_onCommitOrAbort) {
            _onCommitOrAbort();
        }
    }

private:
    /**
     * This iterator is not thread safe and may only be modified by the writer itself. Points to
     * this writer's position in the CappedVisibilityObserver's list of active writers.
     */
    boost::optional<std::list<UncommittedRecords*>::iterator> _it;

    // Since a CappedVisibilitySnapshot is only concerned with the minimum uncommitted RecordId for
    // a given writer, we use an atomic on the minimum. We can use a non-atomic for the maximum,
    // which is never observed by another thread.
    AtomicWord<std::int64_t> _min{0};

    // For consistency with _min, we'll use an int64 type as well.
    std::int64_t _max{0};

    // An optional notification function that should be called when these uncommitted records are
    // either committed or aborted.
    OnCommitOrAbortFn _onCommitOrAbort;
};

/**
 * Container that holds UncommittedRecords for different capped collections. This allows an
 * operation to write to multiple capped collections at once, if necessary. A CappedWriter is only
 * valid for the lifetime of a RecoveryUnit Snapshot, and may only be accessed by a single thread.
 */
class CappedWriter {
public:
    ~CappedWriter();
    static CappedWriter& get(RecoveryUnit*);
    static CappedWriter& get(OperationContext*);

    /**
     * Returns a pointer to the uncommitted writes for the given ident. The pointer is only valid
     * for the duration of this storage snapshot.
     */
    UncommittedRecords* getUncommitedRecordsFor(const std::string& ident);

private:
    // This maps ident names to the uncommitted records for that collection.
    StringMap<std::unique_ptr<UncommittedRecords>> _identToUncommittedRecords;
};

/**
 * A CappedVisibilityObserver tracks the "visibility point" of a capped collection. For capped
 * collections that accept concurrent writes which may not commit in RecordId order, the visibility
 * point is the highest RecordId that is safe to read for a forward scanning cursor to guarantee
 * that it doesn't miss "holes" for uncommitted records.
 */
class CappedVisibilityObserver {
public:
    CappedVisibilityObserver(StringData ident) : _ident(ident) {}

    /**
     * Register a writer for an uncommitted insert operation. The writer must follow-up by
     * registering its allocated RecordIds with registerRecordIds() on the UncommittedRecords.
     */
    UncommittedRecords* registerWriter(
        RecoveryUnit* recoveryUnit,
        UncommittedRecords::OnCommitOrAbortFn&& onCommitOrAbort = nullptr);

    /**
     * Set a RecordId as committed and should be visible immediately. This bypasses any visibility
     * tracking for uncommitted records so should only be used in cases where concurrent writes are
     * not possible.
     */
    void setRecordImmediatelyVisible(const RecordId& rid);

    /**
     * Obtain a consistent view of the capped visibility point. This can be used by callers to
     * determine whether records should be visible or not.
     *
     * It is critical that callers create a capped visibility snapshot before opening a storage
     * engine snapshot unless the caller can guarantee there are no concurrent writes.
     */
    CappedVisibilitySnapshot makeSnapshot() const;

private:
    /**
     * Notify that a previously-allocated RecordId for an uncommitted insert operation has either
     * been committed or rolled-back.
     */
    void _onWriterCommittedOrAborted(CappedWriter* writer, bool committed);

    CappedVisibilitySnapshot _makeSnapshot(WithLock) const;

    const std::string _ident;

    // This mutex protects all variables below.
    mutable Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "CappedVisibilityObserver::_mutex");

    // The set of uncommitted writes to this capped collection. We use a std::list so that we can
    // use splice() for constant time insertion and deletion. This relies on the ability to maintain
    // an iterator that is valid even after modifications to the container.
    std::list<UncommittedRecords*> _uncommittedRecords;

    // This is the highest RecordId ever committed to this collection.
    RecordId _highestRecord;
};
}  // namespace mongo
