// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <cstdint>

#include <absl/container/flat_hash_map.h>

namespace mongo {

/**
 * Stores the last committed size and count values for a collection.
 */
struct [[MONGO_MOD_PUBLIC]] CollectionSizeCount {
    int64_t size{0};
    int64_t count{0};

    bool operator==(const CollectionSizeCount& other) const {
        return size == other.size && count == other.count;
    }
    CollectionSizeCount operator+(const CollectionSizeCount& other) const {
        return CollectionSizeCount{size + other.size, count + other.count};
    }
    CollectionSizeCount operator-(const CollectionSizeCount& other) const {
        return CollectionSizeCount{size - other.size, count - other.count};
    }

    std::string toString() const {
        return str::stream() << "size: " << size << ", count: " << count;
    }
};

inline std::ostream& operator<<(std::ostream& s, const CollectionSizeCount& collectionSizeCount) {
    return (s << collectionSizeCount.toString());
}

/**
 * Indicates whether a collection had been created or dropped since the last checkpoint.
 */
enum class DDLState {
    /**
     * Indicates the collection has been created for the first time.
     */
    kCreated,

    /**
     * Indicates the collection has been dropped.
     */
    kDropped,

    /**
     * Indicates the collection has been dropped and then created again with the same UUID.
     *
     */
    kDroppedAndRecreated,

    /**
     * Indicates the collection has neither been dropped nor created, meaning that the operation
     * with this state is an insert or update.
     */
    kNone
};

/**
 * Stores the size and count values for a collection along with state indicating whether the
 * collection had been created or dropped.
 */
struct SizeCountDelta {
    CollectionSizeCount sizeCount{0, 0};
    DDLState state{DDLState::kNone};

    bool operator==(const SizeCountDelta&) const = default;

    std::string toString() const {
        auto stateStr = [&] {
            switch (state) {
                case DDLState::kCreated:
                    return "created";
                case DDLState::kDropped:
                    return "dropped";
                case DDLState::kDroppedAndRecreated:
                    return "droppedAndRecreated";
                case DDLState::kNone:
                    return "none";
            }
            MONGO_UNREACHABLE;
        }();
        return fmt::format("sizeCount: {}, state: {}", sizeCount.toString(), stateStr);
    }
};

inline std::ostream& operator<<(std::ostream& s, const SizeCountDelta& delta) {
    return (s << delta.toString());
}

namespace replicated_fast_count {

/**
 * Data structure mapping collection UUIDs to their size and count deltas.
 *
 * Useful for tracking changes to collections' size and count while scanning the oplog during
 * both checkpoints and size/count lookups.
 */
using SizeCountDeltas = absl::flat_hash_map<UUID, SizeCountDelta>;

}  // namespace replicated_fast_count

}  // namespace mongo
