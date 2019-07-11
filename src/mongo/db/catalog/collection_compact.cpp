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

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

using logger::LogComponent;

namespace {

Collection* getCollectionForCompact(OperationContext* opCtx,
                                    Database* database,
                                    const NamespaceString& collectionNss) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(collectionNss, MODE_IX));

    CollectionCatalog& collectionCatalog = CollectionCatalog::get(opCtx);
    Collection* collection = collectionCatalog.lookupCollectionByNamespace(collectionNss);

    if (!collection) {
        std::shared_ptr<ViewDefinition> view =
            ViewCatalog::get(database)->lookup(opCtx, collectionNss.ns());
        uassert(ErrorCodes::CommandNotSupportedOnView, "can't compact a view", !view);
        uasserted(ErrorCodes::NamespaceNotFound, "collection does not exist");
    }

    return collection;
}

}  // namespace

StatusWith<CompactStats> compactCollection(OperationContext* opCtx,
                                           const NamespaceString& collectionNss,
                                           const CompactOptions* compactOptions) {
    AutoGetDb autoDb(opCtx, collectionNss.db(), MODE_IX);
    Database* database = autoDb.getDb();
    uassert(ErrorCodes::NamespaceNotFound, "database does not exist", database);

    // The collection lock will be downgraded to an intent lock if the record store supports
    // online compaction.
    boost::optional<Lock::CollectionLock> collLk;
    collLk.emplace(opCtx, collectionNss, MODE_X);

    Collection* collection = getCollectionForCompact(opCtx, database, collectionNss);
    DisableDocumentValidation validationDisabler(opCtx);

    auto recordStore = collection->getRecordStore();
    auto indexCatalog = collection->getIndexCatalog();

    OldClientContext ctx(opCtx, collectionNss.ns());

    if (!recordStore->compactSupported())
        return StatusWith<CompactStats>(ErrorCodes::CommandNotSupported,
                                        str::stream()
                                            << "cannot compact collection with record store: "
                                            << recordStore->name());

    if (recordStore->supportsOnlineCompaction()) {
        // Storage engines that allow online compaction should do so using an intent lock on the
        // collection.
        collLk.emplace(opCtx, collectionNss, MODE_IX);

        // Ensure the collection was not dropped during the re-lock.
        collection = getCollectionForCompact(opCtx, database, collectionNss);
    }

    log(LogComponent::kCommand) << "compact " << collectionNss
                                << " begin, options: " << *compactOptions;

    if (recordStore->compactsInPlace()) {
        CompactStats stats;
        Status status = recordStore->compact(opCtx);
        if (!status.isOK())
            return StatusWith<CompactStats>(status);

        // Compact all indexes (not including unfinished indexes)
        status = indexCatalog->compactIndexes(opCtx);
        if (!status.isOK())
            return StatusWith<CompactStats>(status);

        log() << "compact " << collectionNss << " end";
        return StatusWith<CompactStats>(stats);
    }

    invariant(opCtx->lockState()->isCollectionLockedForMode(collectionNss, MODE_X));

    // If the storage engine doesn't support compacting in place, make sure no background operations
    // or indexes are running.
    const UUID collectionUUID = collection->uuid();
    BackgroundOperation::assertNoBgOpInProgForNs(collectionNss);
    IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(collectionUUID);

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

    MultiIndexBlock indexer;
    indexer.ignoreUniqueConstraint();  // in compact we should be doing no checking

    // The 'indexer' could throw, so ensure build cleanup occurs.
    ON_BLOCK_EXIT([&] { indexer.cleanUpAfterBuild(opCtx, collection); });

    Status status =
        indexer.init(opCtx, collection, indexSpecs, MultiIndexBlock::kNoopOnInitFn).getStatus();
    if (!status.isOK())
        return StatusWith<CompactStats>(status);

    status = recordStore->compact(opCtx);
    if (!status.isOK())
        return StatusWith<CompactStats>(status);

    log() << "starting index commits";
    status = indexer.dumpInsertsFromBulk(opCtx);
    if (!status.isOK())
        return StatusWith<CompactStats>(status);

    {
        WriteUnitOfWork wunit(opCtx);
        status = indexer.commit(opCtx,
                                collection,
                                MultiIndexBlock::kNoopOnCreateEachFn,
                                MultiIndexBlock::kNoopOnCommitFn);
        if (!status.isOK()) {
            return StatusWith<CompactStats>(status);
        }
        wunit.commit();
    }

    log() << "compact " << collectionNss << " end";
    return StatusWith<CompactStats>(stats);
}

}  // namespace mongo
