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

#include "mongo/base/status.h"
#include "mongo/crypto/hash_block.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/db/logical_time.h"
#include "mongo/stdx/mutex.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * The TimeProofService holds the key used by mongod and mongos processes to verify cluster times
 * and contains the logic to generate this key. As a performance optimization to avoid expensive
 * signature generation the class also holds the cache.
 */
class TimeProofService {
public:
    // This type must be synchronized with the library that generates SHA1 or other proof.
    using TimeProof = SHA1Block;
    using Key = SHA1Block;

    TimeProofService() = default;

    /**
     * Generates a pseudorandom key to be used for HMAC authentication.
     */
    static Key generateRandomKey();

    /**
     * Returns the proof matching the time argument.
     */
    TimeProof getProof(LogicalTime time, const Key& key);

    /**
     * Verifies that the proof matches the time argument.
     */
    Status checkProof(LogicalTime time, const TimeProof& proof, const Key& key);

    /**
     * Resets the cache.
     */
    void resetCache();

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
    stdx::mutex _cacheMutex;

    // one-entry cache
    boost::optional<CacheEntry> _cache;
};

}  // namespace mongo
