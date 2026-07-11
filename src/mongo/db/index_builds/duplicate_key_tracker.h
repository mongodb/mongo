// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/lazy_record_store.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
/**
 * Records keys that have violated duplicate key constraints on unique indexes. The keys are backed
 * by a temporary table that is created and destroyed by this tracker.
 */
class DuplicateKeyTracker {
    DuplicateKeyTracker(const DuplicateKeyTracker&) = delete;
    DuplicateKeyTracker& operator=(const DuplicateKeyTracker&) = delete;

public:
    DuplicateKeyTracker(OperationContext* opCtx,
                        std::string_view ident,
                        LazyRecordStore::CreateMode createMode);

    /**
     * Force-creates the backing table if it was deferred and has not already been created.
     */
    void createDeferredTable(OperationContext* opCtx);

    /**
     * Given a duplicate key, insert it into the key constraint table.
     */
    Status recordKey(OperationContext* opCtx,
                     const CollectionPtr& coll,
                     const IndexCatalogEntry* indexCatalogEntry,
                     const key_string::View& key);

    /**
     * Returns boost::none if all previously recorded duplicate key constraint violations have been
     * resolved for the index. Returns duplicate key information if there are still duplicate key
     * constraint violations on the index.
     *
     * Must not be in a WriteUnitOfWork.
     */
    boost::optional<SortedDataInterface::DuplicateKey> checkConstraints(
        OperationContext* opCtx,
        const CollectionPtr& coll,
        const IndexCatalogEntry* indexCatalogEntry) const;

    /**
     * Drops the temporary table. Requires a minimum timestamp to be provided, which acts as a lower
     * bound for the drop reaper, ensuring the table will stay alive until the oldest timestamp has
     * advanced past the drop time.
     */
    void dropTemporaryTable(OperationContext* opCtx, StorageEngine::DropTime dropTime) {
        _keyConstraintsTable.drop(opCtx, dropTime);
    }

private:
    Atomic<long long> _duplicateCounter{0};
    LazyRecordStore _keyConstraintsTable;
};

}  // namespace mongo
