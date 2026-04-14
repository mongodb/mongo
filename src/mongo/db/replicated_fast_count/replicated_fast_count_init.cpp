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
#include "mongo/db/storage/key_format.h"
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
        massert(11757500,
                fmt::format("Failed to create the {} collection with error '{}' and code {}",
                            collDescription,
                            status.reason(),
                            status.code()),
                false);
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

Status _createInternalFastCountContainer(OperationContext* opCtx,
                                         KVEngine* engine,
                                         rss::PersistenceProvider& provider,
                                         RecoveryUnit& ru,
                                         StringData identName,
                                         KeyFormat keyFormat) {
    RecordStore::Options options;
    options.keyFormat = keyFormat;
    auto status = engine->createRecordStore(
        provider, ru, NamespaceString::kAdminCommandNamespace, identName, options);
    if (status.code() == ErrorCodes::ObjectAlreadyExists) {
        LOGV2(12231502,
              "Replicated fast count RecordStore ident already exists, skipping creation.",
              "ident"_attr = identName);
        return status;
    }
    massert(12231501,
            fmt::format("Failed to create RecordStore for ident '{}' with error '{}' and code {}",
                        identName,
                        status.reason(),
                        status.code()),
            status.isOK());
    LOGV2(12231500, "Created replicated fast count container.", "ident"_attr = identName);
    return status;
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

void _createInternalFastCountContainers(OperationContext* opCtx) {
    auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto* ru = shard_role_details::getRecoveryUnit(opCtx);
    auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
    auto* engine = storageEngine->getEngine();

    Lock::GlobalLock globalLock(opCtx, MODE_IX);
    WriteUnitOfWork wuow(opCtx);

    auto metadataStatus = _createInternalFastCountContainer(
        opCtx, engine, provider, *ru, ident::kFastCountMetadataStore, KeyFormat::String);
    auto timestampStatus = _createInternalFastCountContainer(
        opCtx, engine, provider, *ru, ident::kFastCountMetadataStoreTimestamps, KeyFormat::Long);

    // If either ident was created, we need to replicate this creation on the secondary.
    // TODO SERVER-123094 This should be changed to and instead of or since we should only create
    // both idents or neither ident.
    if (metadataStatus.isOK() || timestampStatus.isOK()) {
        // Write the initReplicatedFastCount oplog entry so secondaries create the same
        // RecordStores.
        InitReplicatedFastCountO2 o2;
        o2.setFastCountMetadataStoreIdent(std::string(ident::kFastCountMetadataStore));
        o2.setFastCountMetadataStoreKeyFormat(static_cast<int>(KeyFormat::String));
        o2.setFastCountMetadataStoreTimestampsIdent(
            std::string(ident::kFastCountMetadataStoreTimestamps));
        o2.setFastCountMetadataStoreTimestampsKeyFormat(static_cast<int>(KeyFormat::Long));

        repl::OpTime opTime;
        opCtx->getServiceContext()->getOpObserver()->onInitReplicatedFastCount(opCtx, o2, opTime);
    }

    wuow.commit();
}
}  // namespace

void setUpReplicatedFastCount(OperationContext* opCtx) {
    _createInternalFastCountCollections(repl::StorageInterface::get(opCtx->getServiceContext()),
                                        opCtx);

    if (gFeatureFlagReplicatedFastCountDurability.isEnabledUseLatestFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        _createInternalFastCountContainers(opCtx);
    }

    ReplicatedFastCountManager::get(opCtx->getServiceContext()).startup(opCtx);
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
