// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

struct [[MONGO_MOD_PUBLIC]] MultikeyPathInfo {
    NamespaceString nss;
    UUID collectionUUID;
    std::string indexName;
    KeyStringSet multikeyMetadataKeys;
    MultikeyPaths multikeyPaths;
    // The oplog entry timestamp of the first write that made the index multikey.
    Timestamp earliestTimestamp;

    /**
     * True when 'info' targets the same collection and index at the same write timestamp as this
     * entry.
     */
    bool sameIndexAndCollectionAtTime(const MultikeyPathInfo& info) const;

    /**
     * Merges only multikey path and metadata-key state from 'info'. This does not update identity
     * fields.
     */
    void mergePathsAndKeys(MultikeyPathInfo&& info);
};

using WorkerMultikeyPathInfo [[MONGO_MOD_PUBLIC]] = std::vector<MultikeyPathInfo>;

/**
 * An OperationContext decoration that tracks which indexes should be made multikey. This is used
 * by IndexCatalogEntryImpl::setMultikey() to track what indexes should be set as multikey during
 * secondary oplog application and primary vectored inserts with per-document timestamps. This both
 * marks if the multikey path information should be tracked instead of set immediately and saves the
 * multikey path information for later if needed. Deferred writes must be applied at the exact
 * timestamp of the write that first made the index multikey for timestamp consistency.
 * The tracker stores one entry per (collection UUID, index, timestamp). Entries with the same
 * timestamp are merged immediately, and entries with different timestamps are sorted later so
 * deferred multikey writes are applied in timestamp order.
 */
class [[MONGO_MOD_PUBLIC]] MultikeyPathTracker {
public:
    static const OperationContext::Decoration<MultikeyPathTracker> get;

    /**
     * Returns a string representation of MultikeyPaths for logging.
     */
    static std::string dumpMultikeyPaths(const MultikeyPaths& multikeyPaths);

    static void mergeMultikeyPaths(MultikeyPaths* toMergeInto, const MultikeyPaths& newPaths);

    /**
     * Return true iff the child's paths are a subset of the parent.
     */
    static bool covers(const MultikeyPaths& parent, const MultikeyPaths& child);

    // Decoration requires a default constructor.
    MultikeyPathTracker() = default;

    /**
     * Appends the provided multikey path information to the list of indexes to set as multikey
     * after the current replication batch finishes. Entries from the same insert or update are
     * merged immediately; entries from different writes are saved for later sorted
     * application. Must call startTrackingMultikeyPathInfo() first.
     */
    void addMultikeyPathInfo(MultikeyPathInfo&& info);

    /**
     * Clears out any multikey path information that has been appended.
     * Must call stopTrackingMultikeyPathInfo() first if tracking was previously started.
     */
    void clear();

    /**
     * Returns the multikey path information that has been saved.
     */
    const WorkerMultikeyPathInfo& getMultikeyPathInfo() const;

    /**
     * Returns tracked multikey path info sorted by timestamp. Used by secondaries and by the
     * primary when applying deferred multikey catalog writes for vectored inserts with
     * per-document timestamps.
     */
    WorkerMultikeyPathInfo sortByTimestamp() const;

    /**
     * Returns the multikey path information for the given inputs, or boost::none if none exist.
     */
    boost::optional<MultikeyPaths> getMultikeyPathInfo(const NamespaceString& nss,
                                                       const std::string& indexName);

    /**
     * Specifies that we should track multikey path information on this MultikeyPathTracker. This is
     * only expected to be called during oplog application on secondaries. We cannot simply check
     * 'canAcceptWritesFor' because background index builds use their own OperationContext and
     * cannot store their multikey path info here.
     */
    void startTrackingMultikeyPathInfo();

    /**
     * Specifies to stop tracking multikey path information.
     */
    void stopTrackingMultikeyPathInfo();

    /**
     * Returns if we've called startTrackingMultikeyPathInfo() and not yet called
     * stopTrackingMultikeyPathInfo().
     */
    bool isTrackingMultikeyPathInfo() const;

    /**
     * Returns a boolean representing whether or not any multikey path information
     * has been appended to the list of indexes to set as multikey.
     */
    bool isEmpty() const;

private:
    WorkerMultikeyPathInfo _multikeyPathInfo;
    bool _trackMultikeyPathInfo = false;
};

}  // namespace mongo
