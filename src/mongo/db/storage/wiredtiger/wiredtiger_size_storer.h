/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#include <cstdint>
#include <memory>
#include <string>

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
        AtomicWord<long long> numRecords;
        AtomicWord<long long> dataSize;

    private:
        friend WiredTigerSizeStorer;
        AtomicWord<bool> _dirty;
    };

    WiredTigerSizeStorer(WiredTigerConnection* conn, const std::string& storageUri);
    ~WiredTigerSizeStorer() = default;

    /**
     * Ensure that the shared SizeInfo will be stored by the next call to flush.
     * Values stored are no older than the values at time of this call, but may be newer.
     */
    void store(StringData uri, std::shared_ptr<SizeInfo> sizeInfo);

    /**
     * Returns the size info for the given URI. Creates a default-initialized SizeInfo if there is
     * no existing size info for the given URI. Never returns nullptr.
     */
    std::shared_ptr<SizeInfo> load(WiredTigerSession& session, StringData uri) const;

    /**
     * Informs the size storer that the size information about the given ident should be removed
     * upon the next flush.
     */
    void remove(StringData uri);

    /**
     * Writes all changes to the underlying table.
     */
    void flush(bool syncToDisk);

private:
    WiredTigerConnection* _conn;
    const std::string _storageUri;
    const uint64_t _tableId;  // Not persisted

    // Serializes flushes to disk.
    stdx::mutex _flushMutex;

    using Buffer = StringMap<std::shared_ptr<SizeInfo>>;

    mutable stdx::mutex _bufferMutex;  // Guards _buffer
    Buffer _buffer;
};
}  // namespace mongo
