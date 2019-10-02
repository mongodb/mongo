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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/throttle_cursor.h"

#include "mongo/db/catalog/max_validate_mb_per_sec_gen.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/operation_context.h"

namespace mongo {

// Used to change the 'dataSize' passed into DataThrottle::awaitIfNeeded() to be a fixed size of
// 512KB.
MONGO_FAIL_POINT_DEFINE(fixedCursorDataSizeOf512KBForDataThrottle);

SeekableRecordThrottleCursor::SeekableRecordThrottleCursor(OperationContext* opCtx,
                                                           const RecordStore* rs,
                                                           DataThrottle* dataThrottle) {
    _cursor = rs->getCursor(opCtx, /*forward=*/true);
    _dataThrottle = dataThrottle;
}

boost::optional<Record> SeekableRecordThrottleCursor::seekExact(OperationContext* opCtx,
                                                                const RecordId& id) {
    boost::optional<Record> record = _cursor->seekExact(id);
    if (record) {
        const int64_t dataSize = record->data.size() + sizeof(record->id.repr());
        _dataThrottle->awaitIfNeeded(opCtx, dataSize);
    }

    return record;
}

boost::optional<Record> SeekableRecordThrottleCursor::next(OperationContext* opCtx) {
    boost::optional<Record> record = _cursor->next();
    if (record) {
        const int64_t dataSize = record->data.size() + sizeof(record->id.repr());
        _dataThrottle->awaitIfNeeded(opCtx, dataSize);
    }

    return record;
}

SortedDataInterfaceThrottleCursor::SortedDataInterfaceThrottleCursor(OperationContext* opCtx,
                                                                     const IndexAccessMethod* iam,
                                                                     DataThrottle* dataThrottle) {
    _cursor = iam->newCursor(opCtx, /*forward=*/true);
    _dataThrottle = dataThrottle;
}

boost::optional<IndexKeyEntry> SortedDataInterfaceThrottleCursor::seek(
    OperationContext* opCtx, const KeyString::Value& key) {
    boost::optional<IndexKeyEntry> entry = _cursor->seek(key);
    if (entry) {
        const int64_t dataSize = entry->key.objsize() + sizeof(entry->loc.repr());
        _dataThrottle->awaitIfNeeded(opCtx, dataSize);
    }

    return entry;
}

boost::optional<KeyStringEntry> SortedDataInterfaceThrottleCursor::seekForKeyString(
    OperationContext* opCtx, const KeyString::Value& key) {
    boost::optional<KeyStringEntry> entry = _cursor->seekForKeyString(key);
    if (entry) {
        const int64_t dataSize = entry->keyString.getSize() + sizeof(entry->loc.repr());
        _dataThrottle->awaitIfNeeded(opCtx, dataSize);
    }

    return entry;
}

boost::optional<IndexKeyEntry> SortedDataInterfaceThrottleCursor::next(OperationContext* opCtx) {
    boost::optional<IndexKeyEntry> entry = _cursor->next();
    if (entry) {
        const int64_t dataSize = entry->key.objsize() + sizeof(entry->loc.repr());
        _dataThrottle->awaitIfNeeded(opCtx, dataSize);
    }

    return entry;
}

boost::optional<KeyStringEntry> SortedDataInterfaceThrottleCursor::nextKeyString(
    OperationContext* opCtx) {
    boost::optional<KeyStringEntry> entry = _cursor->nextKeyString();
    if (entry) {
        const int64_t dataSize = entry->keyString.getSize() + sizeof(entry->loc.repr());
        _dataThrottle->awaitIfNeeded(opCtx, dataSize);
    }

    return entry;
}

void DataThrottle::awaitIfNeeded(OperationContext* opCtx, const int64_t dataSize) {
    if (_shouldNotThrottle) {
        return;
    }

    // No throttling should take place if 'gMaxValidateMBperSec' is zero.
    uint64_t maxValidateBytesPerSec = gMaxValidateMBperSec.loadRelaxed() * 1024 * 1024;
    if (maxValidateBytesPerSec == 0) {
        return;
    }

    int64_t currentMillis =
        opCtx->getServiceContext()->getFastClockSource()->now().toMillisSinceEpoch();

    // Reset the tracked information as the second has rolled over the starting point.
    if (currentMillis >= _startMillis + 1000) {
        _startMillis = currentMillis;
        _bytesProcessed = 0;
    }

    _bytesProcessed += MONGO_unlikely(fixedCursorDataSizeOf512KBForDataThrottle.shouldFail())
        ? /*512KB*/ 1 * 1024 * 512
        : dataSize;

    if (_bytesProcessed < maxValidateBytesPerSec) {
        return;
    }

    // Sleep until the second rolls over the starting point.
    do {
        int64_t millisToSleep = 1000 - (currentMillis - _startMillis);
        invariant(millisToSleep >= 0);

        opCtx->sleepFor(Milliseconds(millisToSleep));
        currentMillis =
            opCtx->getServiceContext()->getFastClockSource()->now().toMillisSinceEpoch();
    } while (currentMillis < _startMillis + 1000);
}

}  // namespace mongo
