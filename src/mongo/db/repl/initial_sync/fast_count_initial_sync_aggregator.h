// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/util/uuid.h"

#include <mutex>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::repl {

using FastCountEntry = replicated_fast_count::ReplicatedFastCountManager::FastCountEntry;

/**
 * Thread-safe accumulator used by initial sync to collect persisted replicated fast count
 * metadata as it is reported by the primary in listCollections responses.
 *
 * DatabaseCloner instances populate the aggregator while parsing listCollections results.
 * Once all cloners finish, InitialSyncer consumes the contents to seed the local
 * ReplicatedFastCountManager stores before oplog application begins.
 *
 * The aggregator is consumed exactly once: takeEntries() must be called at most once, by
 * InitialSyncer after all cloners have finished.
 */
class FastCountInitialSyncAggregator {
public:
    void addEntry(UUID uuid, FastCountEntry entry) {
        std::lock_guard lk(_mutex);
        _entries.emplace_back(uuid, entry);
    }

    void recordTimestampStoreTs(Timestamp ts) {
        std::lock_guard lk(_mutex);
        if (!_timestampStoreTs || ts > *_timestampStoreTs) {
            _timestampStoreTs = ts;
        }
    }

    // Destructive, one-shot consume. Must be called at most once.
    std::vector<std::pair<UUID, FastCountEntry>> takeEntries() {
        std::lock_guard lk(_mutex);
        return std::move(_entries);
    }

    boost::optional<Timestamp> getTimestampStoreTs() const {
        std::lock_guard lk(_mutex);
        return _timestampStoreTs;
    }

private:
    mutable std::mutex _mutex;
    std::vector<std::pair<UUID, FastCountEntry>> _entries;
    boost::optional<Timestamp> _timestampStoreTs;
};

}  // namespace mongo::repl
