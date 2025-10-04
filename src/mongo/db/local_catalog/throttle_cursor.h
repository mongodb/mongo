/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {

class DataThrottle;
class IndexAccessMethod;
class OperationContext;
class SortedDataInterface;

/**
 * Cursor wrapper class that creates a cursor internally and throttles record fetching according to
 * the DataThrottle instance passed into its constructor.
 */
class SeekableRecordThrottleCursor {
public:
    SeekableRecordThrottleCursor(OperationContext* opCtx,
                                 const RecordStore* rs,
                                 DataThrottle* dataThrottle);

    boost::optional<Record> seekExact(OperationContext* opCtx, const RecordId& id);

    boost::optional<Record> next(OperationContext* opCtx);

    void save() {
        _cursor->save();
    }

    bool restore(RecoveryUnit& ru) {
        return _cursor->restore(ru);
    }

    void detachFromOperationContext() {
        _cursor->detachFromOperationContext();
    }

    void reattachToOperationContext(OperationContext* opCtx) {
        _cursor->reattachToOperationContext(opCtx);
    }

private:
    std::unique_ptr<SeekableRecordCursor> _cursor;
    DataThrottle* _dataThrottle;
};

/**
 * Cursor wrapper class that creates a cursor internally and throttles record fetching according to
 * the DataThrottle instance passed into its constructor.
 */
class SortedDataInterfaceThrottleCursor {
public:
    SortedDataInterfaceThrottleCursor(OperationContext* opCtx,
                                      const SortedDataIndexAccessMethod* iam,
                                      DataThrottle* dataThrottle);

    boost::optional<IndexKeyEntry> seek(OperationContext* opCtx, std::span<const char> key);
    boost::optional<KeyStringEntry> seekForKeyString(OperationContext* opCtx,
                                                     std::span<const char> key);

    boost::optional<IndexKeyEntry> next(OperationContext* opCtx);
    boost::optional<KeyStringEntry> nextKeyString(OperationContext* opCtx);

    void save() {
        _cursor->save();
    }

    void restore(RecoveryUnit& ru) {
        _cursor->restore(ru);
    }

    void detachFromOperationContext() {
        _cursor->detachFromOperationContext();
    }

    void reattachToOperationContext(OperationContext* opCtx) {
        _cursor->reattachToOperationContext(opCtx);
    }

    bool isRecordIdAtEndOfKeyString() const {
        return _cursor->isRecordIdAtEndOfKeyString();
    }

    void setEndPosition(const BSONObj& key, bool inclusive) {
        _cursor->setEndPosition(key, inclusive);
    }

private:
    std::unique_ptr<SortedDataInterface::Cursor> _cursor;
    DataThrottle* _dataThrottle;
};

/**
 * Throttles the amount of data processed within a unit of time. Puts the thread to sleep via an
 * opCtx -- so it is interruptible -- whenever the data limit set by the 'maxValidateMBperSec'
 * server parameter is exceeded before the time unit is done.
 */
class DataThrottle {
public:
    DataThrottle(OperationContext* opCtx, std::function<int()> maxMBperSec)
        : _startMillis(opCtx->fastClockSource().now().toMillisSinceEpoch()),
          _bytesProcessed(0),
          _totalElapsedTimeSec(0),
          _totalMBProcessed(0),
          _shouldNotThrottle(false),
          _maxMBperSec(maxMBperSec) {}

    /**
     * If throttling is not enabled by calling turnThrottlingOff(), or if
     * 'maxValidateMBperSec' == 0, then this is a no-op.
     *
     * When the accumulated number of bytes processed in each second reaches or exceeds the limit
     * set by the 'maxValidateMBperSec' server parameter, the throttle mechanism gets engaged to
     * wait for the remainder of that second by putting the thread to sleep.
     *
     * In addition to throttling, while the thread is waiting, its operation context remains
     * interruptible.
     */
    void awaitIfNeeded(OperationContext* opCtx, int64_t dataSize);

    void turnThrottlingOff() {
        _shouldNotThrottle = true;
    }

private:
    // Point-in-time (milliseconds) when tracking for the current second has started.
    int64_t _startMillis;

    // Number of bytes processed in the current second being tracked by '_startMillis'. This will
    // contain the number of bytes processed between '_startMillis' and '_startMillis + 999'.
    uint64_t _bytesProcessed;

    float _totalElapsedTimeSec;
    float _totalMBProcessed;

    // Whether the throttle should be active.
    bool _shouldNotThrottle;

    // Will return the rate to throttle, 0 means turn off throttling.
    std::function<int()> _maxMBperSec;
};

}  // namespace mongo
