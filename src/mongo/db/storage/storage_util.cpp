/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include <string>

#include <boost/filesystem/operations.hpp>

#include "mongo/db/storage/storage_util.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace catalog {
namespace {
auto removeEmptyDirectory =
    [](ServiceContext* svcCtx, StorageEngine* storageEngine, const NamespaceString& ns) {
        // Nothing to do if not using directoryperdb or there are still collections in the database.
        // If we don't support supportsPendingDrops then this is executing before the collection is
        // removed from the catalog. In that case, just blindly attempt to delete the directory, it
        // will only succeed if it is empty which is the behavior we want.
        auto collectionCatalog = CollectionCatalog::latest(svcCtx);
        const DatabaseName& dbName = ns.dbName();
        if (!storageEngine->isUsingDirectoryPerDb() ||
            (storageEngine->supportsPendingDrops() &&
             collectionCatalog->begin(nullptr, dbName) != collectionCatalog->end(nullptr))) {
            return;
        }

        boost::system::error_code ec;
        boost::filesystem::remove(storageEngine->getFilesystemPathForDb(dbName), ec);

        if (!ec) {
            LOGV2(4888200, "Removed empty database directory", logAttrs(dbName));
        } else if (collectionCatalog->begin(nullptr, dbName) == collectionCatalog->end(nullptr)) {
            // It is possible for a new collection to be created in the database between when we
            // check whether the database is empty and actually attempting to remove the directory.
            // In this case, don't log that the removal failed because it is expected. However,
            // since we attempt to remove the directory for both the collection and index ident
            // drops, once the database is empty it will be still logged until the final of these
            // ident drops occurs.
            LOGV2_DEBUG(4888201,
                        1,
                        "Failed to remove database directory",
                        logAttrs(dbName),
                        "error"_attr = ec.message());
        }
    };
}  // namespace

void removeIndex(OperationContext* opCtx,
                 StringData indexName,
                 Collection* collection,
                 std::shared_ptr<IndexCatalogEntry> entry,
                 DataRemoval dataRemoval) {
    auto durableCatalog = DurableCatalog::get(opCtx);

    std::shared_ptr<Ident> ident = [&]() -> std::shared_ptr<Ident> {
        if (!entry) {
            return nullptr;
        }
        return entry->getSharedIdent();
    }();

    // If 'ident' is a nullptr, then there is no in-memory state. In that case, create an otherwise
    // unreferenced Ident for the ident reaper to use: the reaper will not need to wait for existing
    // users to finish.
    if (!ident) {
        ident = std::make_shared<Ident>(
            durableCatalog->getIndexIdent(opCtx, collection->getCatalogId(), indexName));
    }

    // Run the first phase of drop to remove the catalog entry.
    collection->removeIndex(opCtx, indexName);

    // The OperationContext may not be valid when the RecoveryUnit executes the onCommit handlers.
    // Therefore, anything that would normally be fetched from the opCtx must be passed in
    // separately to the onCommit handler below.
    //
    // Index creation (and deletion) are allowed in multi-document transactions that use the same
    // RecoveryUnit throughout but not the same OperationContext.
    auto recoveryUnit = opCtx->recoveryUnit();
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();

    const bool isTwoPhaseDrop =
        storageEngine->supportsPendingDrops() && dataRemoval == DataRemoval::kTwoPhase;

    // TODO SERVER-68674: Remove feature flag check.
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe() && isTwoPhaseDrop) {
        invariant(entry);
        CollectionCatalog::get(opCtx)->dropIndex(
            opCtx, collection->ns(), entry, /*isDropPending=*/true);
    }

    // Schedule the second phase of drop to delete the data when it is no longer in use, if the
    // first phase is successfully committed.
    opCtx->recoveryUnit()->onCommit(
        [svcCtx = opCtx->getServiceContext(),
         recoveryUnit,
         storageEngine,
         uuid = collection->uuid(),
         nss = collection->ns(),
         indexNameStr = indexName.toString(),
         ident,
         isTwoPhaseDrop](OperationContext*, boost::optional<Timestamp> commitTimestamp) {
            StorageEngine::DropIdentCallback onDrop =
                [svcCtx, storageEngine, nss, ident = ident->getIdent(), isTwoPhaseDrop] {
                    removeEmptyDirectory(svcCtx, storageEngine, nss);

                    // TODO SERVER-68674: Remove feature flag check.
                    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade
                    // concerns.
                    if (feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe() &&
                        isTwoPhaseDrop) {
                        CollectionCatalog::write(svcCtx, [&](CollectionCatalog& catalog) {
                            catalog.notifyIdentDropped(ident);
                        });
                    }
                };

            if (isTwoPhaseDrop) {
                if (!commitTimestamp) {
                    // Standalone mode will not provide a timestamp.
                    commitTimestamp = Timestamp::min();
                }
                LOGV2(22206,
                      "Deferring table drop for index",
                      "index"_attr = indexNameStr,
                      logAttrs(nss),
                      "uuid"_attr = uuid,
                      "ident"_attr = ident->getIdent(),
                      "commitTimestamp"_attr = commitTimestamp);
                storageEngine->addDropPendingIdent(*commitTimestamp, ident, std::move(onDrop));
            } else {
                LOGV2(6361201,
                      "Completing drop for index table immediately",
                      "ident"_attr = ident->getIdent(),
                      "index"_attr = indexNameStr,
                      logAttrs(nss));
                // Intentionally ignoring failure here. Since we've removed the metadata pointing to
                // the collection, we should never see it again anyway.
                storageEngine->getEngine()
                    ->dropIdent(recoveryUnit, ident->getIdent(), std::move(onDrop))
                    .ignore();
            }
        });
}

Status dropCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      RecordId collectionCatalogId,
                      std::shared_ptr<Ident> ident) {
    invariant(ident);

    // Run the first phase of drop to remove the catalog entry.
    Status status = DurableCatalog::get(opCtx)->dropCollection(opCtx, collectionCatalogId);
    if (!status.isOK()) {
        return status;
    }

    // The OperationContext may not be valid when the RecoveryUnit executes the onCommit handlers.
    // Therefore, anything that would normally be fetched from the opCtx must be passed in
    // separately to the onCommit handler below.
    //
    // Create (and drop) collection are allowed in multi-document transactions that use the same
    // RecoveryUnit throughout but not the same OperationContext.
    auto recoveryUnit = opCtx->recoveryUnit();
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();


    // Schedule the second phase of drop to delete the data when it is no longer in use, if the
    // first phase is successfully committed.
    opCtx->recoveryUnit()->onCommit(
        [svcCtx = opCtx->getServiceContext(), recoveryUnit, storageEngine, nss, ident](
            OperationContext*, boost::optional<Timestamp> commitTimestamp) {
            StorageEngine::DropIdentCallback onDrop =
                [svcCtx, storageEngine, nss, ident = ident->getIdent()] {
                    removeEmptyDirectory(svcCtx, storageEngine, nss);

                    // TODO SERVER-68674: Remove feature flag check.
                    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade
                    // concerns.
                    if (feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe() &&
                        storageEngine->supportsPendingDrops()) {
                        CollectionCatalog::write(svcCtx, [&](CollectionCatalog& catalog) {
                            catalog.notifyIdentDropped(ident);
                        });
                    }
                };

            if (storageEngine->supportsPendingDrops()) {
                if (!commitTimestamp) {
                    // Standalone mode will not provide a timestamp.
                    commitTimestamp = Timestamp::min();
                }
                LOGV2(22214,
                      "Deferring table drop for collection",
                      logAttrs(nss),
                      "ident"_attr = ident->getIdent(),
                      "commitTimestamp"_attr = commitTimestamp);
                storageEngine->addDropPendingIdent(*commitTimestamp, ident, std::move(onDrop));
            } else {
                // Intentionally ignoring failure here. Since we've removed the metadata pointing to
                // the collection, we should never see it again anyway.
                storageEngine->getEngine()
                    ->dropIdent(recoveryUnit, ident->getIdent(), std::move(onDrop))
                    .ignore();
            }
        });

    return Status::OK();
}

}  // namespace catalog
}  // namespace mongo
