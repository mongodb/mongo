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

#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/replicated_fast_count/init_replicated_fast_count_oplog_entry_gen.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/version_context.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {

void _handleStatus(const Status& status, StringData collDescription, const NamespaceString& nss) {
    if (status.isOK()) {
        LOGV2(11718601,
              "Created internal {collDescription} collection.",
              "collDescription"_attr = collDescription,
              "ns"_attr = nss.toStringForErrorMsg());
    } else if (status.code() == ErrorCodes::NamespaceExists) {
        LOGV2(11886900,
              "{collDescription} collection already exists.",
              "collDescription"_attr = collDescription,
              "ns"_attr = nss.toStringForErrorMsg());
    } else {
        massertStatusOK(
            status.withContext(fmt::format("Failed to create the {} collection", collDescription)));
    }
}

Status _createInternalFastCountCollection(repl::StorageInterface* storageInterface,
                                          OperationContext* opCtx,
                                          const NamespaceString& nss) {
    return storageInterface->createCollection(
        opCtx,
        nss,
        CollectionOptions{.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex()});
}

void _createInternalFastCountCollections(repl::StorageInterface* storageInterface,
                                         OperationContext* opCtx) {
    const auto storeNss =
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);
    _handleStatus(
        replicated_fast_count::createReplicatedFastCountCollection(storageInterface, opCtx),
        "replicated fast count metadata store",
        storeNss);

    const auto timestampsNss = NamespaceString::makeGlobalConfigCollection(
        NamespaceString::kReplicatedFastCountStoreTimestamps);
    _handleStatus(replicated_fast_count::createReplicatedFastCountTimestampCollection(
                      storageInterface, opCtx),
                  "replicated fast count metadata store timestamps",
                  timestampsNss);
}

void _writeInitReplicatedFastCountOplogEntry(OperationContext* opCtx) {
    InitReplicatedFastCountO2 o2;
    o2.setFastCountMetadataStoreIdent(std::string(ident::kFastCountMetadataStore));
    o2.setFastCountMetadataStoreKeyFormat(static_cast<int>(KeyFormat::String));
    o2.setFastCountMetadataStoreTimestampsIdent(
        std::string(ident::kFastCountMetadataStoreTimestamps));
    o2.setFastCountMetadataStoreTimestampsKeyFormat(static_cast<int>(KeyFormat::Long));

    repl::OpTime opTime;
    opCtx->getServiceContext()->getOpObserver()->onInitReplicatedFastCount(opCtx, o2, opTime);
}
}  // namespace

void setUpReplicatedFastCount(OperationContext* opCtx) {
    _createInternalFastCountCollections(repl::StorageInterface::get(opCtx->getServiceContext()),
                                        opCtx);

    if (gFeatureFlagReplicatedFastCountDurability.isEnabledUseLatestFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        auto status = createInternalFastCountContainers(opCtx,
                                                        NamespaceString::kAdminCommandNamespace,
                                                        ident::kFastCountMetadataStore,
                                                        KeyFormat::String,
                                                        ident::kFastCountMetadataStoreTimestamps,
                                                        KeyFormat::Long,
                                                        /*writeToOplog=*/true);
        if (status == ErrorCodes::ObjectAlreadyExists) {
            LOGV2(12309403,
                  "Replicated fast count idents already exist during stepup",
                  "metadataIdent"_attr = ident::kFastCountMetadataStore,
                  "timestampsIdent"_attr = ident::kFastCountMetadataStoreTimestamps);
        } else {
            massertStatusOK(status);
        }
    }

    ReplicatedFastCountManager::get(opCtx->getServiceContext()).startup(opCtx);
}

namespace {
Status _createInternalFastCountContainer(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         StringData ident,
                                         KeyFormat keyFormat) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto* engine = storageEngine->getEngine();
    auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();

    // Rolling back table creation performs a two-phase drop, so this ident may be pending drop. If
    // it is we'll need to complete the drop before we can proceed. If it isn't, this is a no-op.
    if (auto status = storageEngine->immediatelyCompletePendingDrop(opCtx, ident); !status.isOK()) {
        LOGV2(12309404,
              "Replicated fast count ident being created was drop-pending and could not be dropped "
              "immediately",
              "ident"_attr = ident,
              "error"_attr = status);
        return status;
    }

    RecordStore::Options options;
    options.keyFormat = keyFormat;
    auto status = engine->createRecordStore(provider, ru, nss, ident, options);
    if (!status.isOK()) {
        return status;
    }

    ru.onRollback([ident = std::string(ident)](OperationContext* opCtx) {
        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        storageEngine->addDropPendingIdent(StorageEngine::Immediate{},
                                           std::make_shared<Ident>(ident));
    });

    return status;
}
}  // namespace

std::pair<Status, std::string> handleExistingFastCountIdent(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            StringData existingIdent,
                                                            KeyFormat existingIdentFormat,
                                                            StringData nonExistentIdent) {
    auto* engine = opCtx->getServiceContext()->getStorageEngine()->getEngine();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

    RecordStore::Options existingIdentOptions{.keyFormat = existingIdentFormat};

    bool empty = false;
    try {
        auto existingRs = engine->getRecordStore(opCtx,
                                                 nss,
                                                 existingIdent,
                                                 existingIdentOptions,
                                                 /*uuid=*/boost::none);
        auto cursor = existingRs->getCursor(opCtx, ru);
        empty = !cursor->next();
    } catch (const DBException& ex) {
        return {ex.toStatus(), ""};
    }

    if (!empty) {
        return {Status(ErrorCodes::Error{12309402},
                       fmt::format("The fast count ident {} already exists and was non-empty even "
                                   "though the ident {} did not exist",
                                   existingIdent,
                                   nonExistentIdent)),
                ""};
    }

    return {Status::OK(),
            fmt::format("One replicated fast count ident already exists on disk after drop attempt "
                        "but the other did not. {} can be re-used and {} was created",
                        existingIdent,
                        nonExistentIdent)};
}

Status createInternalFastCountContainers(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         StringData metadataIdent,
                                         KeyFormat metadataKeyFormat,
                                         StringData timestampsIdent,
                                         KeyFormat timestampsKeyFormat,
                                         bool writeToOplog) {
    // During secondary oplog application we need to use local write intent since regular write
    // intent is for writable primaries.
    auto intent = writeToOplog ? rss::consensus::IntentRegistry::Intent::Write
                               : rss::consensus::IntentRegistry::Intent::LocalWrite;
    Lock::GlobalLock globalLock(opCtx, MODE_IX, Lock::GlobalLockOptions{.explicitIntent = intent});

    // Try creating both containers. ObjectAlreadyExists tells us which idents exist.

    WriteUnitOfWork wuow(opCtx);
    auto metadataStatus =
        _createInternalFastCountContainer(opCtx, nss, metadataIdent, metadataKeyFormat);
    auto timestampsStatus =
        _createInternalFastCountContainer(opCtx, nss, timestampsIdent, timestampsKeyFormat);

    if (metadataStatus.isOK() && timestampsStatus.isOK()) {
        if (writeToOplog) {
            _writeInitReplicatedFastCountOplogEntry(opCtx);
        }
        wuow.commit();
        return Status::OK();
    }

    if (metadataStatus == ErrorCodes::ObjectAlreadyExists &&
        timestampsStatus == ErrorCodes::ObjectAlreadyExists) {
        // Handling for both idents existing differs between stepup initialization and oplog
        // application.
        return metadataStatus;
    }

    if ((!metadataStatus.isOK() && metadataStatus != ErrorCodes::ObjectAlreadyExists) ||
        (!timestampsStatus.isOK() && timestampsStatus != ErrorCodes::ObjectAlreadyExists)) {
        // Return any unexpected error. WUOW rollback will remove any ident that was newly created.
        return !metadataStatus.isOK() ? metadataStatus : timestampsStatus;
    }

    // TODO SERVER-114575 Layered table drops can sometimes report OK but collide with subsequent
    // table creations. We can re-use the colliding ident as long as it is empty.
    Status existingIdentStatus = Status::OK();
    std::string msg;
    if (metadataStatus == ErrorCodes::ObjectAlreadyExists) {
        std::tie(existingIdentStatus, msg) = handleExistingFastCountIdent(
            opCtx, nss, metadataIdent, metadataKeyFormat, timestampsIdent);
    } else if (timestampsStatus == ErrorCodes::ObjectAlreadyExists) {
        std::tie(existingIdentStatus, msg) = handleExistingFastCountIdent(
            opCtx, nss, timestampsIdent, timestampsKeyFormat, metadataIdent);
    } else {
        // Success cases or unexpected errors should have already been handled.
        MONGO_UNREACHABLE_TASSERT(12309405);
    }

    if (existingIdentStatus.isOK()) {
        // The colliding ident was empty so we can treat it as newly created.
        LOGV2(12309400, "Reusing empty ident", "details"_attr = msg);
        if (writeToOplog) {
            _writeInitReplicatedFastCountOplogEntry(opCtx);
        }
        wuow.commit();
    }
    return existingIdentStatus;
}

namespace replicated_fast_count {

Status createReplicatedFastCountCollection(repl::StorageInterface* storageInterface,
                                           OperationContext* opCtx) {
    return _createInternalFastCountCollection(
        storageInterface,
        opCtx,
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore));
}

Status createReplicatedFastCountTimestampCollection(repl::StorageInterface* storageInterface,
                                                    OperationContext* opCtx) {
    return _createInternalFastCountCollection(
        storageInterface,
        opCtx,
        NamespaceString::makeGlobalConfigCollection(
            NamespaceString::kReplicatedFastCountStoreTimestamps));
}
}  // namespace replicated_fast_count
}  // namespace mongo
