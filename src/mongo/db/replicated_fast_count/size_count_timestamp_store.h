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
 *
 * This class is useful for tracking when persisted size and count metadata were last known to be
 * accurate and thus the timestamp after which the oplog is needed for correctness.
 *
 * Two implementations exist: `CollectionSizeCountTimestampStore` (collection-backed) and
 * `ContainerSizeCountTimestampStore` (container-backed).
 *
 * Locking: the container-backed implementation reads and writes the underlying container and does
 * not acquire any locks of its own. Callers must therefore hold the global lock for the duration
 * of the call:
 *   MODE_IS - read()
 *   MODE_IX - write()
 *
 * The collection-backed implementation acquires the collection (and its locks) internally, but
 * callers should hold the same locks so the two implementations are interchangeable.
 */
class SizeCountTimestampStore {
public:
    SizeCountTimestampStore() = default;
    virtual ~SizeCountTimestampStore() = default;

    SizeCountTimestampStore(SizeCountTimestampStore&&) = default;
    SizeCountTimestampStore& operator=(SizeCountTimestampStore&&) = default;
    SizeCountTimestampStore(const SizeCountTimestampStore&) = delete;
    SizeCountTimestampStore& operator=(const SizeCountTimestampStore&) = delete;

    /**
     * Returns the last written timestamp.
     *
     * If no timestamp exists, read() returns boost::none.
     */
    [[nodiscard]] virtual boost::optional<Timestamp> read(OperationContext* opCtx) const = 0;

    /**
     * Upserts `timestamp` into the store. If a timestamp already exists, it will be replaced.
     *
     * write() must be called within a WriteUnitOfWork. Otherwise, the function raises an assertion
     * error.
     */
    virtual void write(OperationContext* opCtx, Timestamp timestamp) = 0;
};

/**
 * Collection-backed implementation of `SizeCountTimestampStore`. Reads and writes target the
 * `config.fast_count_metadata_timestamp_store` collection.
 */
class CollectionSizeCountTimestampStore final : public SizeCountTimestampStore {
public:
    CollectionSizeCountTimestampStore() = default;

    boost::optional<Timestamp> read(OperationContext* opCtx) const override;
    void write(OperationContext* opCtx, Timestamp timestamp) override;
};

/**
 * Container-backed implementation of `SizeCountTimestampStore`. Owns the RecordStore that
 * backs the underlying IntegerKeyedContainer.
 */
class ContainerSizeCountTimestampStore final : public SizeCountTimestampStore {
public:
    explicit ContainerSizeCountTimestampStore(std::unique_ptr<RecordStore> recordStore)
        : _recordStore(std::move(recordStore)) {
        invariant(_recordStore, "ContainerSizeCountTimestampStore requires a non-null RecordStore");
    }

    boost::optional<Timestamp> read(OperationContext* opCtx) const override;
    void write(OperationContext* opCtx, Timestamp timestamp) override;

    RecordStore* rs_ForTest() const;

private:
    std::unique_ptr<RecordStore> _recordStore;
};
}  // namespace mongo::replicated_fast_count
