// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_op_observer.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/cluster_parameters/cluster_parameter_synchronization_helpers.h"
#include "mongo/logv2/log.h"

#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

constexpr auto kIdField = "_id"sv;
constexpr auto kOplog = "oplog"sv;

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
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
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
