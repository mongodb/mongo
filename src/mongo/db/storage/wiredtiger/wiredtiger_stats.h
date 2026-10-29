// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/storage/storage_stats.h"
#include "mongo/util/modules.h"

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

    void appendToBsonObjBuilder(BSONObjBuilder& builder) const final;

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
    Microseconds _storageEngineTime{0};
};

inline WiredTigerStats operator-(WiredTigerStats lhs, const WiredTigerStats& rhs) {
    lhs -= rhs;
    return lhs;
}

}  // namespace mongo
