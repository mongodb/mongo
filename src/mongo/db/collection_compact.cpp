// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/collection_compact.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/shard_role/shard_catalog/document_validation.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/views/view.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <memory>
#include <string>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

MONGO_FAIL_POINT_DEFINE(pauseCompactCommandBeforeWTCompact);

using logv2::LogComponent;

StatusWith<int64_t> compactCollection(OperationContext* opCtx,
                                      const CompactOptions& options,
                                      const CollectionPtr& collection) {
    DisableDocumentValidationForInternalOp validationDisabler(opCtx);

    auto collectionNss = collection->ns();
    auto recordStore = collection->getRecordStore();

    if (!recordStore->compactSupported(opCtx))
        return Status(ErrorCodes::CommandNotSupported,
                      str::stream() << "cannot compact collection with record store: "
                                    << recordStore->name());

    LOGV2_OPTIONS(20284, {LogComponent::kCommand}, "Compact begin", logAttrs(collectionNss));

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto bytesBefore = recordStore->storageSize(ru) + collection->getIndexSize(opCtx);
    auto indexCatalog = collection->getIndexCatalog();

    pauseCompactCommandBeforeWTCompact.pauseWhileSet();

    auto compactCollectionStatus =
        recordStore->compact(opCtx, *shard_role_details::getRecoveryUnit(opCtx), options);
    if (!compactCollectionStatus.isOK())
        return compactCollectionStatus;

    // Compact all indexes (not including unfinished indexes)
    auto compactIndexesStatus = indexCatalog->compactIndexes(opCtx, options);
    if (!compactIndexesStatus.isOK())
        return compactIndexesStatus;

    // The compact operation might grow the file size if there is little free space left, because
    // running a compact also triggers a checkpoint, which requires some space. Additionally, it is
    // possible for concurrent writes and index builds to cause the size to grow while compact is
    // running. So it is possible for the size after a compact to be larger than before it.
    if (options.dryRun) {
        // When a dry run is triggered, each compact call returns an estimated number of bytes that
        // can be reclaimed.
        int64_t estimatedBytes =
            compactCollectionStatus.getValue() + compactIndexesStatus.getValue();
        LOGV2(8352600,
              "Compact end",
              logAttrs(collectionNss),
              "estimatedBytes"_attr = estimatedBytes);
        return estimatedBytes;
    } else {
        auto bytesAfter = recordStore->storageSize(ru) + collection->getIndexSize(opCtx);
        auto bytesDiff = static_cast<int64_t>(bytesBefore) - static_cast<int64_t>(bytesAfter);
        LOGV2(7386700,
              "Compact end",
              logAttrs(collectionNss),
              "bytesBefore"_attr = bytesBefore,
              "bytesAfter"_attr = bytesAfter,
              "bytesDiff"_attr = bytesDiff);
        return bytesDiff;
    }
}

}  // namespace mongo
