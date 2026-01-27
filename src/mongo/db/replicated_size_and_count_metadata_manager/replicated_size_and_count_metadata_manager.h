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
#include "mongo/db/replicated_size_and_count_metadata_manager/size_and_count.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <boost/container/flat_map.hpp>
#include <boost/optional/optional.hpp>


namespace mongo {

class MONGO_MOD_PUBLIC ReplicatedSizeAndCountMetadataManager {
public:
    static ReplicatedSizeAndCountMetadataManager& get(ServiceContext* svcCtx);

    inline static StringData kCountKey = "c"_sd;
    inline static StringData kSizeKey = "s"_sd;

    /**
     * Signals fastcount thread to start.
     */
    void startup(OperationContext* opCtx) {
        uasserted(ErrorCodes::NotImplemented,
                  "ReplicatedSizeAndCountMetadataManager::startup not yet implemented");
    }
    /**
     * Signals fastcount thread to stop.
     */
    void shutdown();

    void commit(const boost::container::flat_map<UUID, CollectionSizeCount>& changes,
                boost::optional<Timestamp> commitTime);

    CollectionSizeCount find(const UUID& uuid) const;

private:
    void _startBackgroundThread(ServiceContext* svcCtx);
    void _runBackgroundThreadOnTimer(OperationContext* opCtx);
    void _runIteration(OperationContext* opCtx);

    void _createMetadataCollection(OperationContext* opCtx);

    void _writeMetadata(OperationContext* opCtx,
                        const UUID& uuid,
                        const CollectionSizeCount& sizeCount,
                        RecordId recordId) {
        uasserted(ErrorCodes::NotImplemented,
                  "ReplicatedSizeAndCountMetadataManager::_writeMetadata not yet implemented");
    }

    void _updateMetadata(OperationContext* opCtx,
                         const CollectionPtr& coll,
                         const Snapshotted<BSONObj>& doc,
                         const UUID& uuid,
                         const CollectionSizeCount& sizeCount);
    void _insertMetadata(OperationContext* opCtx,
                         const CollectionPtr& coll,
                         const UUID& uuid,
                         const CollectionSizeCount& sizeCount) {
        uasserted(ErrorCodes::NotImplemented,
                  "ReplicatedSizeAndCountMetadataManager::_insertMetadata not yet implemented");
    }

    /**
     * Formats and returns the document to write to the metadata collection.
     */
    BSONObj _getDocForWrite(const UUID& uuid, const CollectionSizeCount& sizeCount);

    // Acquire or create if missing, the kSystemReplicatedSizeAndCountMetadataStore collection.
    // TODO SERVER-117575: Change return type to CollectionOrViewAcquisition
    // boost::optional<CollectionOrViewAcquisition> _acquireMetadataCollection(
    boost::optional<int> _acquireMetadataCollection(OperationContext* opCtx) {
        uasserted(ErrorCodes::NotImplemented,
                  "ReplicatedSizeAndCountMetadataManager::_acquireMetadataCollection not yet "
                  "implemented");
        return boost::none;
    }

    // void _fetch(OperationContext* opCtx, UUID uuid);
    RecordId _keyForUUID(const UUID& uuid) {
        uasserted(ErrorCodes::NotImplemented,
                  "ReplicatedSizeAndCountMetadataManager::_keyForUUID not yet implemented");
        return RecordId();
    }
    UUID _UUIDForKey(RecordId key);

    AtomicWord<bool> _inShutdown = false;
    StringData _threadName = "sizeCount"_sd;

    mutable stdx::mutex _mutex;
    struct StoredSizeCount {
        CollectionSizeCount sizeCount;
        bool dirty{false};  // indicate if write is needed
        // boost::optional<Timestamp> lastUpdated;
    };
    absl::flat_hash_map<UUID, StoredSizeCount> _metadata;

    stdx::thread _backgroundThread;
};

}  // namespace mongo

