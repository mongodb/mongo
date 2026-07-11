// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/lazy_record_store.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {
/**
 * Records keys that have violated index key constraints. The keys are backed by a temporary table
 * that is created and destroyed by this tracker.
 */
class SkippedRecordTracker {
    SkippedRecordTracker(const SkippedRecordTracker&) = delete;

public:
    enum class RetrySkippedRecordMode {
        // Retry key generation but do not update the index or remove the records from the tracker.
        kKeyGeneration,
        // Retry key generation and update the index with the new keys, removing the retried records
        // from the tracker.
        kKeyGenerationAndInsertion
    };

    SkippedRecordTracker(OperationContext* opCtx,
                         std::string_view skippedRecordsIdent,
                         LazyRecordStore::CreateMode createMode);

    /**
     * Records a RecordId that was unable to be indexed due to a key generation error. At the
     * conclusion of the build, the key generation and insertion into the index should be attempted
     * again by calling 'retrySkippedRecords'.
     */
    void record(OperationContext* opCtx, const CollectionPtr& coll, const RecordId& recordId);

    /**
     * Force-creates the backing table if it was deferred.
     */
    void createDeferredTable(OperationContext* opCtx);

    /**
     * Returns true if the temporary table is empty.
     */
    bool areAllRecordsApplied(OperationContext* opCtx) const;

    /**
     * By default, attempts to generate keys for each skipped record and insert into the index.
     * Returns OK if all records were either indexed or no longer exist.
     *
     * The behaviour can be modified by specifying a RetrySkippedRecordMode.
     */
    Status retrySkippedRecords(
        OperationContext* opCtx,
        const CollectionPtr& collection,
        const IndexCatalogEntry* indexCatalogEntry,
        RetrySkippedRecordMode mode = RetrySkippedRecordMode::kKeyGenerationAndInsertion);

    boost::optional<MultikeyPaths> getMultikeyPaths() const {
        return _multikeyPaths;
    }

    /**
     * Drops the temporary table. Requires a minimum timestamp to be provided, which acts as a lower
     * bound for the drop reaper, ensuring the table will stay alive until the oldest timestamp has
     * advanced past the drop time.
     */
    void dropTemporaryTable(OperationContext* opCtx, StorageEngine::DropTime dropTime) {
        _skippedRecordsTable.drop(opCtx, dropTime);
    }

private:
    LazyRecordStore _skippedRecordsTable;

    Atomic<std::uint32_t> _skippedRecordCounter{0};

    boost::optional<MultikeyPaths> _multikeyPaths;
};

}  // namespace mongo
