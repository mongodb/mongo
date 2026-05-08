/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <ostream>
#include <span>

#include <boost/optional/optional.hpp>

namespace mongo::replicated_fast_count {

// BSON field names used as the on-disk encoding of fast count records. These are shared between
// the `SizeCountStore`, `SizeCountTimestampStore`, and `ReplicatedFastCountManager`.
inline constexpr StringData kMetadataKey = "meta"_sd;
inline constexpr StringData kSizeKey = "sz"_sd;
inline constexpr StringData kCountKey = "ct"_sd;
inline constexpr StringData kValidAsOfKey = "valid-as-of"_sd;

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
     */
    virtual void remove(OperationContext* opCtx, UUID uuid) = 0;
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
    void remove(OperationContext* opCtx, UUID uuid) override;
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
    void remove(OperationContext* opCtx, UUID uuid) override;

    /**
     * Encodes `uuid` as the container key. The returned span views into `uuid` and is valid only
     * for the lifetime of the UUID argument.
     */
    static std::span<const char> uuidToContainerKey(const UUID& uuid);

private:
    StringKeyedContainer& _getStringKeyedContainer() const;

    std::unique_ptr<RecordStore> _recordStore;
};

inline std::ostream& operator<<(std::ostream& os, const SizeCountStore::Entry& e) {
    return os << "{ timestamp: " << e.timestamp.toString() << ", size: " << e.size
              << ", count: " << e.count << " }";
}
}  // namespace mongo::replicated_fast_count
