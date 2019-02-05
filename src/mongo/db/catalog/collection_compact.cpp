
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/catalog/collection_compact.h"

#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

StatusWith<CompactStats> compactCollection(OperationContext* opCtx,
                                           Collection* collection,
                                           const CompactOptions* compactOptions) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(collection->ns().toString(), MODE_X));

    DisableDocumentValidation validationDisabler(opCtx);

    auto recordStore = collection->getRecordStore();
    auto indexCatalog = collection->getIndexCatalog();

    if (!recordStore->compactSupported())
        return StatusWith<CompactStats>(ErrorCodes::CommandNotSupported,
                                        str::stream()
                                            << "cannot compact collection with record store: "
                                            << recordStore->name());

    if (recordStore->compactsInPlace()) {
        CompactStats stats;
        Status status = recordStore->compact(opCtx);
        if (!status.isOK())
            return StatusWith<CompactStats>(status);

        // Compact all indexes (not including unfinished indexes)
        status = indexCatalog->compactIndexes(opCtx);
        if (!status.isOK())
            return StatusWith<CompactStats>(status);

        return StatusWith<CompactStats>(stats);
    }

    if (indexCatalog->numIndexesInProgress(opCtx))
        return StatusWith<CompactStats>(ErrorCodes::BadValue,
                                        "cannot compact when indexes in progress");

    std::vector<BSONObj> indexSpecs;
    {
        std::unique_ptr<IndexCatalog::IndexIterator> ii(
            indexCatalog->getIndexIterator(opCtx, false));
        while (ii->more()) {
            const IndexDescriptor* descriptor = ii->next()->descriptor();

            // Compact always creates the new index in the foreground.
            const BSONObj spec =
                descriptor->infoObj().removeField(IndexDescriptor::kBackgroundFieldName);
            const BSONObj key = spec.getObjectField("key");
            const Status keyStatus =
                index_key_validate::validateKeyPattern(key, descriptor->version());
            if (!keyStatus.isOK()) {
                return StatusWith<CompactStats>(
                    ErrorCodes::CannotCreateIndex,
                    str::stream() << "Cannot compact collection due to invalid index " << spec
                                  << ": "
                                  << keyStatus.reason()
                                  << " For more info see"
                                  << " http://dochub.mongodb.org/core/index-validation");
            }
            indexSpecs.push_back(spec);
        }
    }

    // Give a chance to be interrupted *before* we drop all indexes.
    opCtx->checkForInterrupt();

    {
        // note that the drop indexes call also invalidates all clientcursors for the namespace,
        // which is important and wanted here
        WriteUnitOfWork wunit(opCtx);
        log() << "compact dropping indexes";
        indexCatalog->dropAllIndexes(opCtx, true);
        wunit.commit();
    }

    CompactStats stats;

    MultiIndexBlock indexer(opCtx, collection);
    indexer.ignoreUniqueConstraint();  // in compact we should be doing no checking

    Status status = indexer.init(indexSpecs, MultiIndexBlock::kNoopOnInitFn).getStatus();
    if (!status.isOK())
        return StatusWith<CompactStats>(status);

    status = recordStore->compact(opCtx);
    if (!status.isOK())
        return StatusWith<CompactStats>(status);

    log() << "starting index commits";
    status = indexer.dumpInsertsFromBulk();
    if (!status.isOK())
        return StatusWith<CompactStats>(status);

    {
        WriteUnitOfWork wunit(opCtx);
        status =
            indexer.commit(MultiIndexBlock::kNoopOnCreateEachFn, MultiIndexBlock::kNoopOnCommitFn);
        if (!status.isOK()) {
            return StatusWith<CompactStats>(status);
        }
        wunit.commit();
    }

    return StatusWith<CompactStats>(stats);
}

}  // namespace mongo
