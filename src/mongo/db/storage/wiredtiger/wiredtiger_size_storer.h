// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace mongo {

class WiredTigerConnection;

/**
 * The WiredTigerSizeStorer class serves as a write buffer to durably store size information for
 * MongoDB collections. The size storer uses a separate WiredTiger table as key-value store, where
 * the URI serves as key and the value is a BSON document with `numRecords` and `dataSize` fields.
 * This buffering is necessary to allow concurrent updates of size information without causing
 * write conflicts. The dirty size information is periodically stored written back to the table,
 * including on clean shutdown and/or catalog reload. Crashes or replica-set fail-overs may result
 * in size updates to be lost, so size information is only approximate. Reads use the buffer for
 * pending stores, or otherwise read directly from the WiredTiger table using a dedicated session
 * and cursor.
 */
class WiredTigerSizeStorer {
public:
    /**
     * SizeInfo is a thread-safe buffer for keeping track of the number of documents in a collection
     * and their data size. Storing a SizeInfo in the WiredTigerSizeStorer results in shared
     * ownership. The SizeInfo may still be updated after it is stored in the SizeStorer.
     * The 'dirty' field is used by the size storer to cheaply merge duplicate stores of the same
     * SizeInfo.
     */
    struct SizeInfo {
        SizeInfo() = default;
        SizeInfo(long long records, long long size) : numRecords(records), dataSize(size) {}

        ~SizeInfo() {
            invariant(!_dirty.load());
        }
        Atomic<long long> numRecords;
        Atomic<long long> dataSize;

    private:
        friend WiredTigerSizeStorer;
        Atomic<bool> _dirty;
    };

    WiredTigerSizeStorer(WiredTigerConnection* conn, const std::string& storageUri);
    ~WiredTigerSizeStorer() = default;

    /**
     * Ensure that the shared SizeInfo will be stored by the next call to flush.
     * Values stored are no older than the values at time of this call, but may be newer.
     */
    void store(std::string_view uri, std::shared_ptr<SizeInfo> sizeInfo);

    /**
     * Returns the size info for the given URI. Creates a default-initialized SizeInfo if there is
     * no existing size info for the given URI. Never returns nullptr.
     */
    std::shared_ptr<SizeInfo> load(WiredTigerSession& session, std::string_view uri) const;

    /**
     * Informs the size storer that the size information about the given ident should be removed
     * upon the next flush.
     */
    void remove(std::string_view uri);

    /**
     * Writes all changes to the underlying table.
     */
    void flush(bool syncToDisk);

    std::string_view getStorageUri() {
        return _storageUri;
    }

private:
    WiredTigerConnection* _conn;
    const std::string _storageUri;
    const uint64_t _tableId;  // Not persisted

    // Serializes flushes to disk.
    std::mutex _flushMutex;

    using Buffer = StringMap<std::shared_ptr<SizeInfo>>;

    mutable std::mutex _bufferMutex;  // Guards _buffer
    Buffer _buffer;
};
}  // namespace mongo
