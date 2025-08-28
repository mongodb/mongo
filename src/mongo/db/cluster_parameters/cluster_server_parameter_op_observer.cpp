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

#include "mongo/db/cluster_parameters/cluster_server_parameter_op_observer.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/cluster_parameters/cluster_parameter_synchronization_helpers.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {

constexpr auto kIdField = "_id"_sd;
constexpr auto kOplog = "oplog"_sd;

bool isConfigNamespace(const NamespaceString& nss) {
    return nss == NamespaceString::makeClusterParametersNSS(nss.dbName().tenantId());
}

}  // namespace

void ClusterServerParameterOpObserver::onInserts(OperationContext* opCtx,
                                                 const CollectionPtr& coll,
                                                 std::vector<InsertStatement>::const_iterator first,
                                                 std::vector<InsertStatement>::const_iterator last,
                                                 const std::vector<RecordId>& recordIds,
                                                 std::vector<bool> fromMigrate,
                                                 bool defaultFromMigrate,
                                                 OpStateAccumulator* opAccumulator) {

    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().isReplSet() && !replCoord->isDataConsistent()) {
        return;
    }

    if (!isConfigNamespace(coll->ns())) {
        return;
    }

    for (auto it = first; it != last; ++it) {
        auto& doc = it->doc;
        auto tenantId = coll->ns().dbName().tenantId();
        cluster_parameters::validateParameter(doc, tenantId);
        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [doc, tenantId](OperationContext* opCtx, boost::optional<Timestamp>) {
                cluster_parameters::updateParameter(opCtx, doc, kOplog, tenantId);
            });
    }
}

void ClusterServerParameterOpObserver::onUpdate(OperationContext* opCtx,
                                                const OplogUpdateEntryArgs& args,
                                                OpStateAccumulator* opAccumulator) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().isReplSet() && !replCoord->isDataConsistent()) {
        return;
    }

    auto updatedDoc = args.updateArgs->updatedDoc;
    if (!isConfigNamespace(args.coll->ns()) || args.updateArgs->update.isEmpty()) {
        return;
    }

    auto tenantId = args.coll->ns().dbName().tenantId();
    cluster_parameters::validateParameter(updatedDoc, tenantId);
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [updatedDoc, tenantId](OperationContext* opCtx, boost::optional<Timestamp>) {
            cluster_parameters::updateParameter(opCtx, updatedDoc, kOplog, tenantId);
        });
}

void ClusterServerParameterOpObserver::onDelete(OperationContext* opCtx,
                                                const CollectionPtr& coll,
                                                StmtId stmtId,
                                                const BSONObj& doc,
                                                const DocumentKey& documentKey,
                                                const OplogDeleteEntryArgs& args,
                                                OpStateAccumulator* opAccumulator) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().isReplSet() && !replCoord->isDataConsistent()) {
        return;
    }

    const auto& nss = coll->ns();
    if (!isConfigNamespace(nss)) {
        return;
    }

    auto elem = doc[kIdField];
    if (elem.type() != BSONType::string) {
        // This delete makes no sense, but it's safe to ignore since the insert/update
        // would not have resulted in an in-memory update anyway.
        LOGV2_DEBUG(6226304,
                    3,
                    "Deleting a cluster-wide server parameter with non-string name",
                    "name"_attr = elem);
        return;
    }

    // Store the tenantId associated with the doc to be deleted.
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [doc = doc.getOwned(), tenantId = nss.dbName().tenantId()](OperationContext* opCtx,
                                                                   boost::optional<Timestamp>) {
            cluster_parameters::clearParameter(opCtx, doc[kIdField].valueStringData(), tenantId);
        });
}

void ClusterServerParameterOpObserver::onDropDatabase(OperationContext* opCtx,
                                                      const DatabaseName& dbName,
                                                      bool markFromMigrate) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().isReplSet() && !replCoord->isDataConsistent()) {
        return;
    }

    if (!dbName.isConfigDB()) {
        return;
    }

    // Entire config DB deleted, reset to default state.
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [tenantId = dbName.tenantId()](OperationContext* opCtx, boost::optional<Timestamp>) {
            cluster_parameters::clearAllTenantParameters(opCtx, tenantId);
        });
}

repl::OpTime ClusterServerParameterOpObserver::onDropCollection(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    const UUID& uuid,
    std::uint64_t numRecords,
    bool markFromMigrate,
    bool isTimeseries) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().isReplSet() && !replCoord->isDataConsistent()) {
        return {};
    }

    if (!isConfigNamespace(collectionName)) {
        return {};
    }

    // Entire collection deleted, reset to default state.
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [tenantId = collectionName.dbName().tenantId()](OperationContext* opCtx,
                                                        boost::optional<Timestamp>) {
            cluster_parameters::clearAllTenantParameters(opCtx, tenantId);
        });

    return {};
}

void ClusterServerParameterOpObserver::onReplicationRollback(OperationContext* opCtx,
                                                             const RollbackObserverInfo& rbInfo) {
    for (const auto& nss : rbInfo.rollbackNamespaces) {
        if (!isConfigNamespace(nss)) {
            continue;
        }

        const auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(NamespaceString::makeClusterParametersNSS(nss.tenantId()),
                                         PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);

        if (coll.exists()) {
            cluster_parameters::resynchronizeAllTenantParametersFromCollection(
                opCtx, *coll.getCollectionPtr().get());
        } else {
            cluster_parameters::clearAllTenantParameters(opCtx, nss.tenantId());
        }
    }
}

}  // namespace mongo
