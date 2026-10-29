// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/assert_util.h"

#include <boost/optional/optional.hpp>

namespace mongo::replicated_fast_count {

/**
 * The `SizeCountTimestampStore` provides read and write access to a single, persisted timestamp.
 * Owns the RecordStore that backs the underlying IntegerKeyedContainer.
 *
 * This class is useful for tracking when persisted size and count metadata were last known to be
 * accurate and thus the timestamp after which the oplog is needed for correctness.
 *
 * Locking: this class reads and writes the underlying container and does not acquire any locks of
 * its own. Callers must therefore hold the global lock for the duration of the call:
 *   MODE_IS - read()
 *   MODE_IX - write()
 */
class SizeCountTimestampStore {
public:
    explicit SizeCountTimestampStore(std::unique_ptr<RecordStore> recordStore)
        : _recordStore(std::move(recordStore)) {
        invariant(_recordStore, "SizeCountTimestampStore requires a non-null RecordStore");
    }

    SizeCountTimestampStore(SizeCountTimestampStore&&) = default;
    SizeCountTimestampStore& operator=(SizeCountTimestampStore&&) = default;
    SizeCountTimestampStore(const SizeCountTimestampStore&) = delete;
    SizeCountTimestampStore& operator=(const SizeCountTimestampStore&) = delete;

    /**
     * Returns the last written timestamp.
     *
     * If no timestamp exists, read() returns boost::none.
     */
    [[nodiscard]] boost::optional<Timestamp> read(OperationContext* opCtx) const;

    /**
     * Upserts `timestamp` into the store. If a timestamp already exists, it will be replaced.
     *
     * write() must be called within a WriteUnitOfWork. Otherwise, the function raises an assertion
     * error.
     */
    void write(OperationContext* opCtx, Timestamp timestamp);

    /**
     * Performs a single write of `timestamp` directly to the underlying physical table. Unlike
     * write(), this does not log to the oplog: it skips invoking op observers and bypasses the
     * `canAcceptWritesFor` primary check, so the write is not replicated.
     */
    void writeToTable(OperationContext* opCtx, Timestamp timestamp);

    RecordStore* rs_ForTest() const;

private:
    std::unique_ptr<RecordStore> _recordStore;
};
}  // namespace mongo::replicated_fast_count
