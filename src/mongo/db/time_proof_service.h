// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/crypto/hash_block.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/db/logical_time.h"
#include "mongo/util/modules.h"

#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * The TimeProofService holds the key used by mongod and mongos processes to verify cluster times
 * and contains the logic to generate this key. As a performance optimization to avoid expensive
 * signature generation the class also holds the cache.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] TimeProofService {
public:
    // This type must be synchronized with the library that generates SHA1 or other proof.
    using TimeProof = SHA1Block;
    using Key = SHA1Block;

    TimeProofService() = default;

    /**
     * Generates a pseudorandom key to be used for HMAC authentication.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] static Key generateRandomKey();

    /**
     * Returns the proof matching the time argument.
     */
    [[MONGO_MOD_PRIVATE]] TimeProof getProof(LogicalTime time, const Key& key);

    /**
     * Verifies that the proof matches the time argument.
     */
    [[MONGO_MOD_PRIVATE]] Status checkProof(LogicalTime time,
                                            const TimeProof& proof,
                                            const Key& key);

    /**
     * Resets the cache.
     */
    [[MONGO_MOD_PRIVATE]] void resetCache();

private:
    /**
     * Nested class to cache TimeProof. It holds proof for the greatest time allowed.
     */
    struct CacheEntry {
        CacheEntry(TimeProof proof, LogicalTime time, const Key& key)
            : _proof(std::move(proof)), _time(time), _key(key) {}

        /**
         * Returns true if it has proof for time greater or equal than the argument.
         */
        bool hasProof(LogicalTime time, const Key& key) const {
            return key == _key && time == _time;
        }

        TimeProof _proof;
        LogicalTime _time;
        Key _key;
    };

    // protects _cache
    std::mutex _cacheMutex;

    // one-entry cache
    boost::optional<CacheEntry> _cache;
};

}  // namespace mongo
