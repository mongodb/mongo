/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/collection_crud/capped_collection_maintenance.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/curop.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <cstdint>
#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace collection_internal {
namespace {

struct CappedCollectionState {
    RecordId cappedFirstRecord;
};

const auto cappedCollectionState =
    SharedCollectionDecorations::declareDecoration<CappedCollectionState>();

}  // namespace


bool shouldDeferCappedDeletesToOplogApplication(OperationContext* opCtx,
                                                const CollectionPtr& collection) {
    // Secondaries only delete from capped collections via oplog application when there are explicit
    // delete oplog entries.
    if (!opCtx->isEnforcingConstraints()) {
        return true;
    }

    if (collection->ns().isTemporaryReshardingCollection()) {
        // Don't do capped deletes if this is a temporary resharding collection since that could
        // lead to multi-timestamp violation. The recipient shard will apply the capped delete oplog
        // entries from the donor shard anyway.
        // TODO PM-3667: Reevaluate if we can setEnforceConstraints(false) during resharding and
        // rely solely on the above check.
        return true;
    }

    return false;
}

void cappedDeleteUntilBelowConfiguredMaximum(OperationContext* opCtx,
                                             const CollectionPtr& collection,
                                             const RecordId& justInserted,
                                             OpDebug* opDebug) {
    if (!collection->isCappedAndNeedsDelete(opCtx))
        return;

    if (shouldDeferCappedDeletesToOplogApplication(opCtx, collection)) {
        // The primary has already executed the following logic and generated the necessary delete
        // oplogs. We will apply them later.
        return;
    }

    const auto& nss = collection->ns();
    auto& ccs = cappedCollectionState(*collection->getSharedDecorations());

    // The capped metadata lock serializes writes to ensure only one one writer is actively deleting
    // a document at at time. As capped deletes can be part of a larger WriteUnitOfWork, this also
    // protect 'cappedFirstRecord' until the outermost WriteUnitOfWork commits or aborts. Locking
    // the metadata resource exclusively on the collection gives us that guarantee as it uses
    // two-phase locking semantics and the lock is held in the onCommit handler.
    invariant(collection->needsCappedLock());
    invariant(shard_role_details::getLocker(opCtx)->getLockMode(
                  ResourceId(RESOURCE_METADATA, nss)) == MODE_X);

    const long long currentDataSize = collection->dataSize(opCtx);
    const long long currentNumRecords = collection->numRecords(opCtx);

    const auto cappedMaxSize = collection->getCollectionOptions().cappedSize;
    const long long sizeOverCap =
        (currentDataSize > cappedMaxSize) ? currentDataSize - cappedMaxSize : 0;

    const auto cappedMaxDocs = collection->getCollectionOptions().cappedMaxDocs;
    const long long docsOverCap = (cappedMaxDocs != 0 && currentNumRecords > cappedMaxDocs)
        ? currentNumRecords - cappedMaxDocs
        : 0;

    long long sizeSaved = 0;
    long long docsRemoved = 0;

    invariant(shard_role_details::getRecoveryUnit(opCtx)->inUnitOfWork());

    boost::optional<Record> record;
    auto cursor = collection->getCursor(opCtx, /*forward=*/true);

    // If the next RecordId to be deleted is known, navigate to it using seek(). Using a cursor
    // and advancing it to the first element by calling next() will be slow for capped collections
    // on particular storage engines, such as WiredTiger. In WiredTiger, there may be many
    // tombstones (invisible deleted records) to traverse at the beginning of the table.
    if (!ccs.cappedFirstRecord.isNull()) {
        // Use a bounded seek instead of seekExact. If this node steps down and a new primary starts
        // deleting capped documents then this node's cached record will become stale. If this node
        // steps up again afterwards, then the cached record will be an already deleted document.
        record =
            cursor->seek(ccs.cappedFirstRecord, SeekableRecordCursor::BoundInclusion::kInclude);
    } else {
        record = cursor->next();
    }

    while (sizeSaved < sizeOverCap || docsRemoved < docsOverCap) {
        if (!record) {
            break;
        }

        if (record->id == justInserted) {
            // We're prohibited from deleting what was just inserted.
            break;
        }

        docsRemoved++;
        sizeSaved += record->data.size();

        BSONObj doc = record->data.toBson();
        if (nss.isReplicated()) {
            OpObserver* opObserver = opCtx->getServiceContext()->getOpObserver();

            const auto& documentKey = getDocumentKey(collection, doc);
            OplogDeleteEntryArgs args;

            // If collection has change stream pre-/post-images enabled, pass the 'deletedDoc' for
            // writing it in the pre-images collection.
            if (collection->isChangeStreamPreAndPostImagesEnabled()) {
                args.changeStreamPreAndPostImagesEnabledForCollection = true;
            }

            // Reserves an optime for the deletion and sets the timestamp for future writes.
            opObserver->onDelete(opCtx, collection, kUninitializedStmtId, doc, documentKey, args);
        }

        int64_t keysDeleted = 0;
        collection->getIndexCatalog()->unindexRecord(opCtx,
                                                     collection,
                                                     doc,
                                                     record->id,
                                                     /*logIfError=*/false,
                                                     &keysDeleted);

        // We're about to delete the record our cursor is positioned on, so advance the cursor.
        RecordId toDelete = std::move(record->id);
        record = cursor->next();

        collection->getRecordStore()->deleteRecord(
            opCtx, *shard_role_details::getRecoveryUnit(opCtx), toDelete);

        if (opDebug) {
            opDebug->additiveMetrics.incrementKeysDeleted(keysDeleted);
            opDebug->additiveMetrics.incrementNdeleted(1);
        }
        serviceOpCounters(opCtx).gotDelete();
    }

    // Update the next record to be deleted. The next record must exist as we're using the same
    // snapshot the insert was performed on and we can't delete newly inserted records.
    invariant(record);
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [&ccs, recordId = std::move(record->id)](OperationContext*, boost::optional<Timestamp>) {
            ccs.cappedFirstRecord = std::move(recordId);
        });
}

}  // namespace collection_internal
}  // namespace mongo
