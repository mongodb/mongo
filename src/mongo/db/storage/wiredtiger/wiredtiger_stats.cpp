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

#include "mongo/db/storage/wiredtiger/wiredtiger_stats.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/assert_util.h"

#include <cstdint>

#include <wiredtiger.h>

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

void appendIfNonZero(StringData fieldName, int64_t value, BSONObjBuilder* builder) {
    if (value != 0) {
        builder->append(fieldName, value);
    }
}

}  // namespace

WiredTigerStats::WiredTigerStats(WiredTigerSession& session) {
    WT_CURSOR* c = nullptr;
    uassert(ErrorCodes::CursorNotFound,
            "Unable to open statistics cursor",
            !session.open_cursor("statistics:session", nullptr, "statistics=(fast)", &c));

    ScopeGuard guard{[c] {
        c->close(c);
    }};

    // Get all the stats
    while (c->next(c) == 0) {
        int32_t key;
        invariant(c->get_key(c, &key) == 0);

        uint64_t value;
        fassert(51035, c->get_value(c, nullptr, nullptr, &value) == 0);

        updateCounter(key, WiredTigerUtil::castStatisticsValue<long long>(value));
    }

    _storageExecutionTime = session.getStorageExecutionTime();
}

void WiredTigerStats::updateCounter(int32_t key_id, uint64_t value) {
    switch (key_id) {
        case WT_STAT_SESSION_BYTES_READ:
            _bytesRead = value;
            break;
        case WT_STAT_SESSION_BYTES_WRITE:
            _bytesWrite = value;
            break;
        case WT_STAT_SESSION_LOCK_DHANDLE_WAIT:
            _lockDhandleWait = value;
            break;
        case WT_STAT_SESSION_TXN_BYTES_DIRTY:
            _txnBytesDirty = value;
            break;
        case WT_STAT_SESSION_TXN_UPDATES:
            _txnNumUpdates = value;
            break;
        case WT_STAT_SESSION_READ_TIME:
            _readTime = Microseconds((int64_t)value);
            break;
        case WT_STAT_SESSION_WRITE_TIME:
            _writeTime = Microseconds((int64_t)value);
            break;
        case WT_STAT_SESSION_LOCK_SCHEMA_WAIT:
            _lockSchemaWait = value;
            break;
        case WT_STAT_SESSION_CACHE_TIME:
            _cacheTime = Microseconds((int64_t)value);
            break;
        case WT_STAT_SESSION_CACHE_TIME_INTERRUPTIBLE:
            _cacheTimeInterruptible = Microseconds((int64_t)value);
            break;
        case WT_STAT_SESSION_CACHE_TIME_MANDATORY:
            _cacheTimeMandatory = Microseconds((int64_t)value);
            break;
        default:
            // Ignore unknown counters that WT may add.
            break;
    }
}

BSONObj WiredTigerStats::toBSON() const {

    BSONObjBuilder builder;

    // Only output metrics for non-zero values
    if (_bytesRead != 0 || _bytesWrite != 0 || _readTime.count() != 0 || _writeTime.count() != 0 ||
        _txnBytesDirty != 0 || _txnNumUpdates != 0) {
        BSONObjBuilder dataSection(builder.subobjStart("data"));
        appendIfNonZero("bytesRead", _bytesRead, &dataSection);
        appendIfNonZero("bytesWritten", _bytesWrite, &dataSection);
        appendIfNonZero("timeReadingMicros", durationCount<Microseconds>(_readTime), &dataSection);
        appendIfNonZero("timeWritingMicros", durationCount<Microseconds>(_writeTime), &dataSection);
        appendIfNonZero("txnBytesDirty", _txnBytesDirty, &dataSection);
        appendIfNonZero("txnNumUpdates", _txnNumUpdates, &dataSection);
    }

    if (_lockDhandleWait != 0 || _lockSchemaWait != 0 || _cacheTime.count() != 0 ||
        _cacheTimeInterruptible.count() != 0 || _cacheTimeMandatory.count() != 0 ||
        _storageExecutionTime.count() != 0) {
        BSONObjBuilder waitingSection(builder.subobjStart("timeWaitingMicros"));
        appendIfNonZero("handleLock", _lockDhandleWait, &waitingSection);
        appendIfNonZero("schemaLock", _lockSchemaWait, &waitingSection);
        appendIfNonZero("cacheMicros", durationCount<Microseconds>(_cacheTime), &waitingSection);
        appendIfNonZero("cacheInterruptibleMicros",
                        durationCount<Microseconds>(_cacheTimeInterruptible),
                        &waitingSection);
        appendIfNonZero("cacheMandatoryMicros",
                        durationCount<Microseconds>(_cacheTimeMandatory),
                        &waitingSection);
        appendIfNonZero("storageExecutionMicros",
                        durationCount<Microseconds>(_storageExecutionTime),
                        &waitingSection);
    }

    return builder.obj();
}

uint64_t WiredTigerStats::bytesRead() const {
    return _bytesRead;
}

Microseconds WiredTigerStats::readingTime() const {
    return Microseconds(_readTime);
}

int64_t WiredTigerStats::txnBytesDirty() const {
    return _txnBytesDirty;
}

std::unique_ptr<StorageStats> WiredTigerStats::clone() const {
    return std::make_unique<WiredTigerStats>(*this);
}

WiredTigerStats& WiredTigerStats::operator+=(const WiredTigerStats& other) {
    _bytesRead += other._bytesRead;
    _bytesWrite += other._bytesWrite;
    _lockDhandleWait += other._lockDhandleWait;
    _txnBytesDirty += other._txnBytesDirty;
    _txnNumUpdates += other._txnNumUpdates;
    _readTime += other._readTime;
    _writeTime += other._writeTime;
    _lockSchemaWait += other._lockSchemaWait;
    _cacheTime += other._cacheTime;
    _cacheTimeInterruptible += other._cacheTimeInterruptible;
    _cacheTimeMandatory += other._cacheTimeMandatory;
    _storageExecutionTime += other._storageExecutionTime;

    return *this;
}

StorageStats& WiredTigerStats::operator+=(const StorageStats& other) {
    return *this += checked_cast<const WiredTigerStats&>(other);
}

WiredTigerStats& WiredTigerStats::operator-=(const WiredTigerStats& other) {
    _bytesRead -= other._bytesRead;
    _bytesWrite -= other._bytesWrite;
    _lockDhandleWait -= other._lockDhandleWait;
    _txnBytesDirty -= other._txnBytesDirty;
    _txnNumUpdates -= other._txnNumUpdates;
    _readTime -= other._readTime;
    _writeTime -= other._writeTime;
    _lockSchemaWait -= other._lockSchemaWait;
    _cacheTime -= other._cacheTime;
    _cacheTimeInterruptible -= other._cacheTimeInterruptible;
    _cacheTimeMandatory -= other._cacheTimeMandatory;
    _storageExecutionTime -= other._storageExecutionTime;

    return (*this);
}

StorageStats& WiredTigerStats::operator-=(const StorageStats& other) {
    *this -= checked_cast<const WiredTigerStats&>(other);
    return (*this);
}

}  // namespace mongo
