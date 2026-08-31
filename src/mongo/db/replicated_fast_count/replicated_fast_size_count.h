// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <cstdint>

#include <absl/container/flat_hash_map.h>
#include <boost/container/flat_map.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

inline constexpr int64_t kEmptyCollectionValidationHash = 0;

/**
 * Combines two validation hashes with XOR, which is its own inverse: folding a contribution in and
 * back out again restores the original value, and the order of the contributions does not matter.
 *
 * An absent operand makes the result absent, since a value that is missing one of its contributions
 * is indistinguishable from a wrong one.
 */
inline boost::optional<int64_t> combineValidationHashes(const boost::optional<int64_t>& lhs,
                                                        const boost::optional<int64_t>& rhs) {
    if (!lhs || !rhs) {
        return boost::none;
    }
    return *lhs ^ *rhs;
}

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
 * Stores a collection's size and count along with the validation hash accumulated over the same
 * operations.
 */
struct CollectionReplicatedMetadata {
    CollectionSizeCount sizeCount;

    boost::optional<int64_t> hash;

    bool operator==(const CollectionReplicatedMetadata&) const = default;

    CollectionReplicatedMetadata operator+(const CollectionReplicatedMetadata& other) const {
        return CollectionReplicatedMetadata{sizeCount + other.sizeCount,
                                            combineValidationHashes(hash, other.hash)};
    }
    CollectionReplicatedMetadata operator-(const CollectionReplicatedMetadata& other) const {
        // XOR is its own inverse, so removing a hash contribution is the same operation as adding
        // it.
        return CollectionReplicatedMetadata{sizeCount - other.sizeCount,
                                            combineValidationHashes(hash, other.hash)};
    }

    std::string toString() const {
        str::stream s;
        s << sizeCount.toString();
        if (hash) {
            s << ", hash: " << *hash;
        }
        return s;
    }
};

inline std::ostream& operator<<(std::ostream& s, const CollectionReplicatedMetadata& metadata) {
    return (s << metadata.toString());
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
 * Stores the size, count, and validation hash values for a collection along with state indicating
 * whether the collection had been created or dropped.
 */
struct ReplicatedMetadataDelta {
    CollectionReplicatedMetadata metadata{.sizeCount = CollectionSizeCount{.size = 0, .count = 0},
                                          .hash = boost::none};
    DDLState state{DDLState::kNone};

    bool operator==(const ReplicatedMetadataDelta&) const = default;

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
        return fmt::format("metadata: {}, state: {}", metadata.toString(), stateStr);
    }
};

inline std::ostream& operator<<(std::ostream& s, const ReplicatedMetadataDelta& delta) {
    return (s << delta.toString());
}

namespace replicated_fast_count {

/**
 * Data structure mapping collection UUIDs to their replicated-metadata deltas.
 *
 * Useful for tracking changes to collections' size, count, and hash while scanning the oplog during
 * checkpoints.
 */
using ReplicatedMetadataDeltas = absl::flat_hash_map<UUID, ReplicatedMetadataDelta>;

}  // namespace replicated_fast_count

}  // namespace mongo
