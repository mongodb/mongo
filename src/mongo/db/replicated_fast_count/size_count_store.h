// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <ostream>
#include <span>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo::replicated_fast_count {
using namespace std::literals::string_view_literals;

// BSON field names used as the on-disk encoding of fast count records. These are shared between
// the `SizeCountStore`, `SizeCountTimestampStore`, and `ReplicatedFastCountManager`.
inline constexpr std::string_view kMetadataKey = "meta"sv;
inline constexpr std::string_view kSizeKey = "sz"sv;
inline constexpr std::string_view kCountKey = "ct"sv;
inline constexpr std::string_view kValidAsOfKey = "valid-as-of"sv;

/**
 * Acquires the replicated fast count collection for read access.
 * Returns boost::none if the collection does not exist.
 */
boost::optional<CollectionOrViewAcquisition> acquireFastCountCollectionForRead(
    OperationContext* opCtx);

/**
 * Acquire the fastcount collection that underpins this class with write intent.
 * Returns boost::none if it doesn't exist.
 */
boost::optional<CollectionOrViewAcquisition> acquireFastCountCollectionForWrite(
    OperationContext* opCtx);

/**
 * Abstract interface for read/write access to the persisted size and count metadata. Two
 * implementations exist: `CollectionSizeCountStore` (collection-backed) and
 * `ContainerSizeCountStore` (container-backed).
 *
 * Locking: the container-backed implementation reads and writes the underlying container and does
 * not acquire any locks of its own. Callers must therefore hold the global lock for the duration
 * of the call:
 *   MODE_IS - read(), readAndIncrementSizeCounts()
 *   MODE_IX - write(), insert(), remove()
 *
 * The collection-backed implementation acquires the collection (and its locks) internally, but
 * callers should hold the same locks so the two implementations are interchangeable.
 */
class SizeCountStore {
public:
    /**
     * In-memory representation of the persisted `size` and `count` for a collection. The
     * `timestamp` indicates when the `size` and `count` values were last flushed.
     */
    struct Entry {
        Timestamp timestamp{0, 0};
        int64_t size{0};
        int64_t count{0};
        bool operator==(const Entry&) const = default;
    };

    SizeCountStore() = default;
    virtual ~SizeCountStore() = default;

    SizeCountStore(SizeCountStore&&) = default;
    SizeCountStore& operator=(SizeCountStore&&) = default;
    SizeCountStore(const SizeCountStore&) = delete;
    SizeCountStore& operator=(const SizeCountStore&) = delete;

    /**
     * Decodes a container value produced by a previous write into an Entry.
     */
    static Entry parseContainerValue(std::span<const char> value);

    /**
     * Returns the persisted size, count, and timestamp for the collection with `uuid`.
     *
     * If no entry exists for `uuid`, read() returns boost::none.
     */
    [[nodiscard]] virtual boost::optional<Entry> read(OperationContext* opCtx, UUID uuid) const = 0;

    /**
     * Upserts `entry` into the store. `entry` will overwrite any pre-existing document for `uuid`.
     */
    virtual void write(OperationContext* opCtx, UUID uuid, const Entry& entry) = 0;

    /**
     * Inserts `entry` into the store. If an entry for `uuid` already exists, this operation will
     * throw a DBException.
     */
    virtual void insert(OperationContext* opCtx, UUID uuid, const Entry& entry) = 0;

    /**
     * Removes the entry for `uuid` from the store if one exists, otherwise does nothing.
     *
     * Returns the number of entries removed, either 0 or 1.
     */
    virtual size_t remove(OperationContext* opCtx, UUID uuid) = 0;

    /**
     * For each entry in `deltas`, looks up the persisted size and count for that UUID in the
     * on-disk fast count store and adds the persisted values to the entry's size and count
     * in place. If a UUID has no on-disk entry, its delta is left unchanged.
     *
     * Implementations are expected to acquire any underlying storage handles once and reuse them
     * across all UUIDs in `deltas`, since this is the checkpoint hot path.
     */
    virtual void readAndIncrementSizeCounts(OperationContext* opCtx,
                                            SizeCountDeltas& deltas) const = 0;

    virtual bool usesContainers() const = 0;
};

/**
 * Collection-backed implementation of `SizeCountStore`. Reads and writes target the
 * `config.fast_count_metadata_store` collection.
 */
class CollectionSizeCountStore final : public SizeCountStore {
public:
    CollectionSizeCountStore() = default;

    boost::optional<Entry> read(OperationContext* opCtx, UUID uuid) const override;
    void write(OperationContext* opCtx, UUID uuid, const Entry& entry) override;
    void insert(OperationContext* opCtx, UUID uuid, const Entry& entry) override;
    size_t remove(OperationContext* opCtx, UUID uuid) override;
    void readAndIncrementSizeCounts(OperationContext* opCtx,
                                    SizeCountDeltas& deltas) const override;

    bool usesContainers() const override {
        return false;
    }
};

/**
 * Container-backed implementation of `SizeCountStore`. Owns the RecordStore that backs the
 * underlying StringKeyedContainer.
 */
class ContainerSizeCountStore final : public SizeCountStore {
public:
    explicit ContainerSizeCountStore(std::unique_ptr<RecordStore> recordStore)
        : _recordStore(std::move(recordStore)) {
        invariant(_recordStore, "ContainerSizeCountStore requires a non-null RecordStore");
    }

    boost::optional<Entry> read(OperationContext* opCtx, UUID uuid) const override;
    void write(OperationContext* opCtx, UUID uuid, const Entry& entry) override;
    void insert(OperationContext* opCtx, UUID uuid, const Entry& entry) override;
    size_t remove(OperationContext* opCtx, UUID uuid) override;
    void readAndIncrementSizeCounts(OperationContext* opCtx,
                                    SizeCountDeltas& deltas) const override;

    bool usesContainers() const override {
        return true;
    }

    /**
     * Encodes `uuid` as the container key. The returned span views into `uuid` and is valid only
     * for the lifetime of the UUID argument.
     */
    static std::span<const char> uuidToContainerKey(const UUID& uuid);

    RecordStore* rs_ForTest() const;

private:
    StringKeyedContainer& _getStringKeyedContainer() const;

    std::unique_ptr<RecordStore> _recordStore;
};

inline std::ostream& operator<<(std::ostream& os, const SizeCountStore::Entry& e) {
    return os << "{ timestamp: " << e.timestamp.toString() << ", size: " << e.size
              << ", count: " << e.count << " }";
}
}  // namespace mongo::replicated_fast_count
