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
#include "mongo/db/record_id.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_committer.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <boost/container/flat_map.hpp>
#include <boost/optional/optional.hpp>


namespace mongo {

/**
 * Maintains an in-memory cache of the size and count of all collections.
 *
 * Terminology:
 * The term "fast count" is historical, but should be equivalent to "fast size"
 * and "fast sizecount" to refer to a cached, and therefore "fast", size or count
 * value for a collection.
 *
 *
 * This cache is intended to be accurate, and helps the server avoid expensive
 * collection scans to compute these values. The validate command can check and repair
 * the fast count and size with various flags to the command, like --enforceFastCount.
 *
 * Backing the in-memory cache is a pair of collections used for recovery scenarios.
 *
 * This class and its backing collections are a singleton that creates its internal
 * collection(s) once, the first time a mongod creates its data files. It is assumed
 * these backing collections exist from then on.
 *
 * This class is thread-safe, and synchroizes access to the in-memory SizeCount cache,
 * i.e. _metadata.
 *
 * The write path should generally not depend directly on this class, because it relies on
 * the collection write path to persist fast SizeCounts. Instead, operations should
 * interact with this class through UncommittedFastCountChange.
 */
class MONGO_MOD_PUBLIC ReplicatedFastCountManager {
    struct StoredSizeCount {
        CollectionSizeCount sizeCount;
        bool dirty{false};  // Indicates if flush is needed.
    };

public:
    static ReplicatedFastCountManager& get(ServiceContext* svcCtx);

    ReplicatedFastCountManager() {
        initializeFastCountCommitFn();
    }

    /**
     * Registers the fast count commit function that will be called on commit to apply the changes
     * to the in-memory metadata. This function is initialized in this way to avoid introducing a
     * circular dependency by having the UncommittedFastCountChange class depend directly on
     * ReplicatedFastCountManager, since the former is depended on by the collection write path and
     * the latter depends on the collection write path.
     */
    void initializeFastCountCommitFn();

    inline static StringData kSizeKey = "s"_sd;
    inline static StringData kCountKey = "c"_sd;

    /**
     * Signals fastcount thread to start.
     */
    void startup(OperationContext* opCtx);

    /**
     * Signals fastcount thread to stop.
     */
    void shutdown();

    /**
     * Records committed changes to the size and count for the collections in 'changes'.
     */
    void commit(const boost::container::flat_map<UUID, CollectionSizeCount>& changes,
                boost::optional<Timestamp> commitTime);

    /**
     * Given a collection UUID, returns the last committed value of size and count for that
     * collection.
     */
    CollectionSizeCount find(const UUID& uuid) const;

    /**
     * Snapshots and writes dirty in-memory SizeCounts to the internal fastcount collection on disk.
     * Not recommended outside of the Checkpointer, generally for mirroring legacy sizeStorer
     * behavior.
     */
    void flush(OperationContext* opCtx);

    /**
     * Disables periodic background writes of metadata for testing purposes. Must be called before
     * startup().
     */
    void disablePeriodicWrites_ForTest();

private:
    /**
     * Return a copy of a subset of _metadata, only including the dirty entries. Clears the dirty
     * flags for all currently dirty entries.
     */
    absl::flat_hash_map<UUID, StoredSizeCount> _getSnapshotOfDirtyMetadata();

    /**
     * Write out dirtyMetadata to fastCountColl.
     */
    void _doFlush(OperationContext* opCtx,
                  const CollectionPtr& fastCountColl,
                  const absl::flat_hash_map<UUID, StoredSizeCount>& dirtyMetadata);

    void _startBackgroundThread(ServiceContext* svcCtx);
    void _runBackgroundThreadOnTimer(OperationContext* opCtx);

    /**
     * Write one collection's sizeCount to disk.
     */
    void _writeOneMetadata(OperationContext* opCtx,
                           const CollectionPtr& fastCountColl,
                           const UUID& uuid,
                           const CollectionSizeCount& sizeCount,
                           RecordId recordId);

    void _updateOneMetadata(OperationContext* opCtx,
                            const CollectionPtr& fastCountColl,
                            const Snapshotted<BSONObj>& doc,
                            const UUID& uuid,
                            RecordId recordId,
                            const CollectionSizeCount& sizeCount);
    void _insertOneMetadata(OperationContext* opCtx,
                            const CollectionPtr& fastCountColl,
                            const UUID& uuid,
                            const CollectionSizeCount& sizeCount);

    /**
     * Acquire the fastcount collection that underpins this class.
     * Returns boost::none if it doesn't exist.
     */
    boost::optional<CollectionOrViewAcquisition> _acquireFastCountCollection(
        OperationContext* opCtx);

    /**
     * Formats and returns the document to write to the fastcount collection.
     */
    BSONObj _getDocForWrite(const UUID& uuid, const CollectionSizeCount& sizeCount) const;

    /**
     * Generates a key (RecordId) into the fastcount collection given a user
     * collection uuid.
     */
    RecordId _keyForUUID(const UUID& uuid) const;
    UUID _UUIDForKey(RecordId key) const;

    StringData _threadName = "replicatedSizeCount"_sd;
    stdx::thread _backgroundThread;
    bool _writeMetadataPeriodically = true;
    AtomicWord<bool> _isDisabled = false;
    stdx::condition_variable _condVar;

    /**
     * In-memory cache of Fast Size & Counts since last checkpoint.
     * Implemented as a map of collection UUID to the last committed size and count.
     */
    absl::flat_hash_map<UUID, StoredSizeCount> _metadata;
    mutable stdx::mutex _metadataMutex;
};

}  // namespace mongo

