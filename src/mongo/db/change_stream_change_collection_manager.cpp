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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/change_stream_change_collection_manager.h"

#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {
const auto getChangeCollectionManager =
    ServiceContext::declareDecoration<boost::optional<ChangeStreamChangeCollectionManager>>();

// TODO: SERVER-65950 create or update the change collection for a particular tenant.
NamespaceString getTenantChangeCollectionNamespace(boost::optional<TenantId> tenantId) {
    return NamespaceString{NamespaceString::kConfigDb, NamespaceString::kChangeCollectionName};
}

}  // namespace

ChangeStreamChangeCollectionManager& ChangeStreamChangeCollectionManager::get(
    ServiceContext* service) {
    return *getChangeCollectionManager(service);
}

ChangeStreamChangeCollectionManager& ChangeStreamChangeCollectionManager::get(
    OperationContext* opCtx) {
    return *getChangeCollectionManager(opCtx->getServiceContext());
}

void ChangeStreamChangeCollectionManager::create(ServiceContext* service) {
    getChangeCollectionManager(service).emplace(service);
}

Status ChangeStreamChangeCollectionManager::createChangeCollection(
    OperationContext* opCtx, boost::optional<TenantId> tenantId) {
    // Make the change collection clustered by '_id'. The '_id' field will have the same value as
    // the 'ts' field of the oplog.
    CollectionOptions changeCollectionOptions;
    changeCollectionOptions.clusteredIndex.emplace(clustered_util::makeDefaultClusteredIdIndex());
    changeCollectionOptions.capped = true;

    auto status = createCollection(
        opCtx, getTenantChangeCollectionNamespace(tenantId), changeCollectionOptions, BSONObj());
    if (status.code() == ErrorCodes::NamespaceExists) {
        return Status::OK();
    }

    return status;
}

Status ChangeStreamChangeCollectionManager::dropChangeCollection(
    OperationContext* opCtx, boost::optional<TenantId> tenantId) {
    DropReply dropReply;
    return dropCollection(opCtx,
                          getTenantChangeCollectionNamespace(tenantId),
                          &dropReply,
                          DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
}

void ChangeStreamChangeCollectionManager::insertDocumentsToChangeCollection(
    OperationContext* opCtx,
    const std::vector<Record>& oplogRecords,
    const std::vector<Timestamp>& oplogTimestamps) {
    invariant(oplogRecords.size() == oplogTimestamps.size());

    // This method must be called within a 'WriteUnitOfWork'. The caller must be responsible for
    // commiting the unit of work.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    // Maps statements that should be inserted to the change collection for each tenant.
    stdx::unordered_map<TenantId, std::vector<InsertStatement>, TenantId::Hasher>
        tenantToInsertStatements;

    for (size_t idx = 0; idx < oplogRecords.size(); idx++) {
        auto& record = oplogRecords[idx];
        auto& ts = oplogTimestamps[idx];

        // Create a mutable document and update the '_id' field with the oplog entry timestamp. The
        // '_id' field will be use to order the change collection documents.
        Document oplogDoc(record.data.toBson());
        MutableDocument changeCollDoc(oplogDoc);
        changeCollDoc["_id"] = Value(ts);

        // Create an insert statement that should be written at the timestamp 'ts' for a particular
        // tenant.
        auto readyChangeCollDoc = changeCollDoc.freeze();
        tenantToInsertStatements[TenantId::kSystemTenantId].push_back(
            InsertStatement{readyChangeCollDoc.toBson(), ts, repl::OpTime::kUninitializedTerm});
    }

    for (auto&& [tenantId, insertStatements] : tenantToInsertStatements) {
        // TODO SERVER-66715 avoid taking 'AutoGetCollection' and remove
        // 'AllowLockAcquisitionOnTimestampedUnitOfWork'.
        AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(opCtx->lockState());
        AutoGetCollection tenantChangeCollection(
            opCtx, getTenantChangeCollectionNamespace(tenantId), LockMode::MODE_IX);

        // The change collection does not exist for a particular tenant because either the change
        // collection is not enabled or is in the process of enablement. Ignore this insert for now.
        // TODO: SERVER-65950 move this check before inserting to the map
        // 'tenantToInsertStatements'.
        if (!tenantChangeCollection) {
            continue;
        }

        // Writes to the change collection should not be replicated.
        repl::UnreplicatedWritesBlock unReplBlock(opCtx);

        Status status = tenantChangeCollection->insertDocuments(opCtx,
                                                                insertStatements.begin(),
                                                                insertStatements.end(),
                                                                nullptr /* opDebug */,
                                                                false /* fromMigrate */);
        if (!status.isOK()) {
            LOGV2_FATAL(6612300,
                        "Write to change collection: {ns} failed: {error}",
                        "Write to change collection failed",
                        "ns"_attr = tenantChangeCollection->ns().toString(),
                        "error"_attr = status.toString());
        }
    }
}

}  // namespace mongo
