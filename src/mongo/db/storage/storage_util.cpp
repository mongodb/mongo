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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/storage/storage_util.h"

#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace catalog {

void removeIndex(OperationContext* opCtx,
                 StringData indexName,
                 RecordId collectionCatalogId,
                 UUID collectionUUID,
                 const NamespaceString& nss) {
    const std::string indexIdent =
        DurableCatalog::get(opCtx)->getIndexIdent(opCtx, collectionCatalogId, indexName);

    // Run the first phase of drop to remove the catalog entry.
    DurableCatalog::get(opCtx)->removeIndex(opCtx, collectionCatalogId, indexName);

    // The OperationContext may not be valid when the RecoveryUnit executes the onCommit handlers.
    // Therefore, anything that would normally be fetched from the opCtx must be passed in
    // separately to the onCommit handler below.
    //
    // Index creation (and deletion) are allowed in multi-document transactions that use the same
    // RecoveryUnit throughout but not the same OperationContext.
    auto recoveryUnit = opCtx->recoveryUnit();
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();

    // Schedule the second phase of drop to delete the data when it is no longer in use, if the
    // first phase is successuflly committed.
    opCtx->recoveryUnit()->onCommit([opCtx,
                                     recoveryUnit,
                                     storageEngine,
                                     collectionUUID,
                                     nss,
                                     indexNameStr = indexName.toString(),
                                     indexIdent](boost::optional<Timestamp> commitTimestamp) {
        if (storageEngine->supportsPendingDrops() && commitTimestamp) {
            LOGV2(22206,
                  "Deferring table drop for index '{index}' on collection "
                  "'{namespace}{uuid}. Ident: '{ident}', commit timestamp: '{commitTimestamp}'",
                  "Deferring table drop for index",
                  "index"_attr = indexNameStr,
                  logAttrs(nss),
                  "uuid"_attr = collectionUUID,
                  "ident"_attr = indexIdent,
                  "commitTimestamp"_attr = commitTimestamp);
            storageEngine->addDropPendingIdent(*commitTimestamp, nss, indexIdent);
        } else {
            auto kvEngine = storageEngine->getEngine();
            kvEngine->dropIdent(opCtx, recoveryUnit, indexIdent).ignore();
        }
    });
}

Status dropCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      RecordId collectionCatalogId,
                      StringData ident) {
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
    // first phase is successuflly committed.
    opCtx->recoveryUnit()->onCommit(
        [opCtx, recoveryUnit, storageEngine, nss, identStr = ident.toString()](
            boost::optional<Timestamp> commitTimestamp) {
            if (storageEngine->supportsPendingDrops() && commitTimestamp) {
                LOGV2(22214,
                      "Deferring table drop for collection '{namespace}'. Ident: {ident}, "
                      "commit timestamp: {commitTimestamp}",
                      "Deferring table drop for collection",
                      logAttrs(nss),
                      "ident"_attr = identStr,
                      "commitTimestamp"_attr = commitTimestamp);
                storageEngine->addDropPendingIdent(*commitTimestamp, nss, identStr);
            } else {
                // Intentionally ignoring failure here. Since we've removed the metadata pointing to
                // the collection, we should never see it again anyway.
                auto kvEngine = storageEngine->getEngine();
                kvEngine->dropIdent(opCtx, recoveryUnit, identStr).ignore();
            }
        });

    return Status::OK();
}

}  // namespace catalog
}  // namespace mongo
