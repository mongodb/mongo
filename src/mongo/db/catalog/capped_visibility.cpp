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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/db/catalog/capped_visibility.h"

#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"

namespace mongo {

namespace {
const RecoveryUnit::Snapshot::Decoration<CappedWriter> getCappedWriters =
    RecoveryUnit::Snapshot::declareDecoration<CappedWriter>();
}

CappedWriter& CappedWriter::get(RecoveryUnit* ru) {
    return getCappedWriters(ru->getSnapshot());
}

CappedWriter& CappedWriter::get(OperationContext* opCtx) {
    return getCappedWriters(opCtx->recoveryUnit()->getSnapshot());
}

CappedWriter::~CappedWriter() {
    try {
        // Signal that any uncommitted writes are either committed or aborted. Destruction of this
        // CappedWriter is a direct result of the snapshot of the RecoveryUnit being destructed.
        for (auto&& [_, uncommitted] : _identToUncommittedRecords) {
            uncommitted->committedOrAborted();
        }
    } catch (...) {
        LOGV2_FATAL(6628300,
                    "Caught exception destructing CappedWriter",
                    "exception"_attr = exceptionToStatus());
    }
}

UncommittedRecords* CappedWriter::getUncommitedRecordsFor(const std::string& ident) {
    auto& uncommitted = _identToUncommittedRecords[ident];
    if (!uncommitted) {
        uncommitted = std::make_unique<UncommittedRecords>();
    }
    return uncommitted.get();
}

void UncommittedRecords::registerRecordIds(const RecordId& min, const RecordId& max) {
    // The only circumstances where we can correctly handle capped visibility are with integer
    // RecordIds, since with clustered collections (string RecordIds) we don't control RecordId
    // assignment.
    invariant(min.isLong());
    invariant(max.isLong());
    invariant(max >= min);
    // We can register multiple recordids on the same writer, but we update the min and max.
    auto myMin = _min.load();
    if (!myMin || min.getLong() < myMin) {
        _min.store(min.getLong());
    }
    if (!_max || max.getLong() > _max) {
        _max = max.getLong();
    }
}

UncommittedRecords* CappedVisibilityObserver::registerWriter(
    RecoveryUnit* recoveryUnit, UncommittedRecords::OnCommitOrAbortFn&& onCommitOrAbort) {
    auto& writer = CappedWriter::get(recoveryUnit);
    auto uncommitted = writer.getUncommitedRecordsFor(_ident);

    // We have already been registered, so we should not insert a new entry.
    if (uncommitted->getIterator().has_value()) {
        return uncommitted;
    }

    uncommitted->onCommitOrAbort(std::move(onCommitOrAbort));

    // Register ourselves as a writer with uncommitted records. We allocate a new single-element
    // list outside the mutex, obtain an iterator, and then splice into the existing list under the
    // mutex. The iterator remains valid even in the new list, and we can erase in constant time
    // when the writer commits.
    std::list<UncommittedRecords*> tmp;
    auto it = tmp.insert(tmp.begin(), uncommitted);
    uncommitted->setIterator(std::move(it));
    {
        stdx::unique_lock<Mutex> lk(_mutex);
        _uncommittedRecords.splice(_uncommittedRecords.end(), tmp);
    }

    // Because CappedWriters is decorated on the RecoveryUnit and tied to its lifetime, we can
    // capture the writer without risk of it dangling.
    class CappedVisibilityChange : public RecoveryUnit::Change {
    public:
        CappedVisibilityChange(CappedVisibilityObserver* observer, CappedWriter* writer)
            : _observer(observer), _writer(writer) {}
        void commit(OperationContext* opCtx, boost::optional<Timestamp> commitTime) final {
            _observer->_onWriterCommittedOrAborted(_writer, true /* commit */);
        }
        void rollback(OperationContext* opCtx) final {
            _observer->_onWriterCommittedOrAborted(_writer, false /* commit */);
        }

    private:
        CappedVisibilityObserver* _observer;
        CappedWriter* _writer;
    };

    recoveryUnit->registerChange(std::make_unique<CappedVisibilityChange>(this, &writer));
    return uncommitted;
}

void CappedVisibilityObserver::_onWriterCommittedOrAborted(CappedWriter* writer, bool committed) {
    auto uncommitted = writer->getUncommitedRecordsFor(_ident);
    invariant(uncommitted->getIterator());
    auto min = uncommitted->getMinRecord();
    auto max = uncommitted->getMaxRecord();

    // Create a temporary list that we use to splice out the removed element and can deallocate
    // outside of the mutex.
    std::list<UncommittedRecords*> tmp;
    {
        stdx::unique_lock<Mutex> lk(_mutex);
        // We only want to advance the highest record when a transaction commits.
        if (committed) {
            if (max > _highestRecord) {
                _highestRecord = max;
            }
        }
        tmp.splice(tmp.end(), _uncommittedRecords, *uncommitted->getIterator());
    }
}

void CappedVisibilityObserver::setRecordImmediatelyVisible(const RecordId& rid) {
    stdx::unique_lock<Mutex> lk(_mutex);
    _highestRecord = rid;
}

CappedVisibilitySnapshot CappedVisibilityObserver::makeSnapshot() const {
    stdx::unique_lock<Mutex> lk(_mutex);
    return _makeSnapshot(lk);
}

CappedVisibilitySnapshot CappedVisibilityObserver::_makeSnapshot(WithLock wl) const {
    RecordId lowestUncommitted;
    for (auto&& uncommitted : _uncommittedRecords) {
        auto min = uncommitted->getMinRecord();
        if (lowestUncommitted.isNull() || (!min.isNull() && min < lowestUncommitted)) {
            lowestUncommitted = min;
        }
    }
    return {_highestRecord, lowestUncommitted};
}
}  // namespace mongo
