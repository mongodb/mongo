// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <cstdint>
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
inline constexpr std::string_view kHashKey = "h"sv;
inline constexpr std::string_view kValidAsOfKey = "valid-as-of"sv;

/**
 * The `SizeCountStore` provides read and write access to the persisted metadata of each collection.
 * Owns the RecordStore that backs the underlying StringKeyedContainer.
 *
 * Locking: this class reads and writes the underlying container and does not acquire any locks of
 * its own. Callers must therefore hold the global lock for the duration of the call:
 *     MODE_IS - read(), readAndIncrementReplicatedMetadata()
 *     MODE_IX - write(), insert(), remove()
 */
class SizeCountStore {
public:
    /**
     * In-memory representation of the persisted `size` and `count` for a collection. The
     * `timestamp` indicates when the `size` and `count` values were last flushed. `hash`
     * is boost::none for records written before the field existed or by nodes that do not yet emit
     * it.
     */
    struct Entry {
        Timestamp timestamp{0, 0};
        int64_t size{0};
        int64_t count{0};
        boost::optional<int64_t> hash;
        bool operator==(const Entry&) const = default;
    };

    explicit SizeCountStore(std::unique_ptr<RecordStore> recordStore)
        : _recordStore(std::move(recordStore)) {
        invariant(_recordStore, "SizeCountStore requires a non-null RecordStore");
    }

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
    [[nodiscard]] boost::optional<Entry> read(OperationContext* opCtx, UUID uuid) const;

    /**
     * Upserts `entry` into the store. `entry` will overwrite any pre-existing document for `uuid`.
     *
     * If `entry.hash` is `boost::none`, the hash and its key, `kHashKey`, are not written.
     */
    void write(OperationContext* opCtx, UUID uuid, const Entry& entry);

    /**
     * Inserts `entry` into the store. If an entry for `uuid` already exists, this operation will
     * throw a DBException.
     *
     * If `entry.hash` is `boost::none`, the hash and its key, `kHashKey`, are not written.
     */
    void insert(OperationContext* opCtx, UUID uuid, const Entry& entry);

    /**
     * Removes the entry for `uuid` from the store if one exists, otherwise does nothing.
     *
     * Returns the number of entries removed, either 0 or 1.
     */
    size_t remove(OperationContext* opCtx, UUID uuid);

    /**
     * For each entry in `deltas`, looks up the persisted size and count for that UUID in the
     * on-disk fast count store and adds the persisted values to the entry's size and count
     * in place. If a UUID has no on-disk entry, its delta is left unchanged.
     */
    void readAndIncrementReplicatedMetadata(OperationContext* opCtx,
                                            ReplicatedMetadataDeltas& deltas) const;

    /**
     * Performs a single write of `entry` for `uuid` directly to the underlying physical table.
     * Unlike write(), this does not log to the oplog: it skips invoking op observers and bypasses
     * the `canAcceptWritesFor` primary check, so the write is not replicated.
     */
    void writeToTable(OperationContext* opCtx, UUID uuid, const Entry& entry);

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
    os << "{ timestamp: " << e.timestamp.toString() << ", size: " << e.size
       << ", count: " << e.count << ", hash: ";
    if (e.hash) {
        os << *e.hash;
    } else {
        os << "none";
    }
    return os << " }";
}
}  // namespace mongo::replicated_fast_count
