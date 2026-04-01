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
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
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
}  // namespace

void setUpReplicatedFastCount(OperationContext* opCtx) {
    _createInternalFastCountCollections(repl::StorageInterface::get(opCtx->getServiceContext()),
                                        opCtx);
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
