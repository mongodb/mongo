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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/storage/storage_stats.h"

#include <memory>

#include <wiredtiger.h>

namespace mongo {

class WiredTigerSession;

class WiredTigerStats final : public StorageStats {
public:
    /**
     * Construct a new WiredTigerStats object with the statistics of the specified session.
     */
    WiredTigerStats(WiredTigerSession& session);

    WiredTigerStats() = default;

    BSONObj toBSON() const final;

    uint64_t bytesRead() const final;
    Microseconds readingTime() const final;

    int64_t txnBytesDirty() const;

    std::unique_ptr<StorageStats> clone() const final;

    StorageStats& operator+=(const StorageStats&) final;

    WiredTigerStats& operator+=(const WiredTigerStats&);

    StorageStats& operator-=(const StorageStats&) final;

    WiredTigerStats& operator-=(const WiredTigerStats&);

private:
    void updateCounter(int32_t key_id, uint64_t value);

private:
    // See src/third_party/wiredtiger/src/include/stat.h
    // which is derived from src/third_party/wiredtiger/dist/stat_data.py
    int64_t _bytesRead{0};
    int64_t _bytesWrite{0};
    int64_t _lockDhandleWait{0};
    int64_t _txnBytesDirty{0};
    int64_t _txnNumUpdates{0};
    Microseconds _readTime{0};
    Microseconds _writeTime{0};
    int64_t _lockSchemaWait{0};
    Microseconds _cacheTime{0};
    // The latency for WiredTiger to interrupt cache eviction.
    Microseconds _cacheTimeInterruptible{0};
    // The time spent performing mandatory cache eviction.
    Microseconds _cacheTimeMandatory{0};
    // The duration of the last WT_SESSION API call.
    Microseconds _storageExecutionTime{0};
};

inline WiredTigerStats operator-(WiredTigerStats lhs, const WiredTigerStats& rhs) {
    lhs -= rhs;
    return lhs;
}

}  // namespace mongo
