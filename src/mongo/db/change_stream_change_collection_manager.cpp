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
#include "mongo/db/namespace_string.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {
const auto getChangeCollectionManager =
    ServiceContext::declareDecoration<boost::optional<ChangeStreamChangeCollectionManager>>();
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
    // TODO: SERVER-65950 create or update the change collection for a particular tenant.
    const NamespaceString nss{NamespaceString::kConfigDb,
                              NamespaceString::kChangeStreamChangeCollection};

    // Make the change collection clustered by '_id'. The '_id' field will have the same value as
    // the 'ts' field of the oplog.
    CollectionOptions changeCollectionOptions;
    changeCollectionOptions.clusteredIndex.emplace(clustered_util::makeDefaultClusteredIdIndex());
    changeCollectionOptions.capped = true;

    auto status = createCollection(opCtx, nss, changeCollectionOptions, BSONObj());
    if (status.code() == ErrorCodes::NamespaceExists) {
        return Status(ErrorCodes::Error::OK, "");
    }

    return status;
}

Status ChangeStreamChangeCollectionManager::dropChangeCollection(
    OperationContext* opCtx, boost::optional<TenantId> tenantId) {
    // TODO: SERVER-65950 remove the change collection for a particular tenant.
    const NamespaceString nss{NamespaceString::kConfigDb,
                              NamespaceString::kChangeStreamChangeCollection};
    DropReply dropReply;
    return dropCollection(
        opCtx, nss, &dropReply, DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
}

Status ChangeStreamChangeCollectionManager::insertDocumentsToChangeCollection(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    std::vector<Record>* records,
    const std::vector<Timestamp>& timestamps) {
    // TODO SERVER-65210 add code to insert to the change collection in the primaries.
    return Status(ErrorCodes::OK, "");
}

}  // namespace mongo
