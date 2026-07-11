// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/sharding_environment/config_server_op_observer.h"

#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/cluster_identity_loader.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_ready.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/topology/vector_clock/topology_time_ticker.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/version/releases.h"

#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

ConfigServerOpObserver::ConfigServerOpObserver() = default;

ConfigServerOpObserver::~ConfigServerOpObserver() = default;

void ConfigServerOpObserver::onDelete(OperationContext* opCtx,
                                      const CollectionPtr& coll,
                                      StmtId stmtId,
                                      const BSONObj& doc,
                                      const DocumentKey& documentKey,
                                      const OplogDeleteEntryArgs& args,
                                      OpStateAccumulator* opAccumulator) {
    if (coll->ns() == NamespaceString::kConfigVersionNamespace) {
        if (!repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback()) {
            uasserted(40302, "cannot delete config.version document while in --configsvr mode");
        }
    }
}

repl::OpTime ConfigServerOpObserver::onDropCollection(OperationContext* opCtx,
                                                      const NamespaceString& collectionName,
                                                      const UUID& uuid,
                                                      std::uint64_t numRecords,
                                                      bool markFromMigrate,
                                                      bool isTimeseries) {
    if (collectionName == NamespaceString::kConfigVersionNamespace) {
        if (!repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback()) {
            uasserted(40303, "cannot drop config.version document while in --configsvr mode");
        }
    }

    return {};
}

void ConfigServerOpObserver::onReplicationRollback(OperationContext* opCtx,
                                                   const RollbackObserverInfo& rbInfo) {
    if (rbInfo.configServerConfigVersionRolledBack) {
        // Throw out any cached information related to the cluster ID.
        ShardingCatalogManager::get(opCtx)->discardCachedConfigDatabaseInitializationState();
        ClusterIdentityLoader::get(opCtx)->discardCachedClusterId();
    }

    if (rbInfo.rollbackNamespaces.find(NamespaceString::kConfigsvrShardsNamespace) !=
        rbInfo.rollbackNamespaces.end()) {
        // If some entries were rollbacked from config.shards we might need to discard some tick
        // points from the TopologyTimeTicker
        const auto lastApplied = repl::ReplicationCoordinator::get(opCtx)->getMyLastAppliedOpTime();
        TopologyTimeTicker::get(opCtx).onReplicationRollback(lastApplied);
    }
}

void ConfigServerOpObserver::onInserts(OperationContext* opCtx,
                                       const CollectionPtr& coll,
                                       std::vector<InsertStatement>::const_iterator begin,
                                       std::vector<InsertStatement>::const_iterator end,
                                       const std::vector<RecordId>& recordIds,
                                       std::vector<bool> fromMigrate,
                                       bool defaultFromMigrate,
                                       OpStateAccumulator* opAccumulator) {
    if (coll->ns() != NamespaceString::kConfigsvrShardsNamespace) {
        return;
    }

    // (Ignore FCV check): Auto-bootstrapping happens irrespective of the FCV when
    // gFeatureFlagAllMongodsAreSharded is enabled.
    if (gFeatureFlagAllMongodsAreSharded.isEnabledAndIgnoreFCVUnsafe() &&
        !ShardingReady::get(opCtx)->isReady()) {
        for (auto it = begin; it != end; it++) {
            const auto& insertedDoc = it->doc;
            const auto idElem = insertedDoc["_id"];
            if (idElem.str() == ShardId::kConfigServerId) {
                /**
                 * Signal that the config shard is ready when we are certain that the config shard
                 * document inserted into config.shards is committed.
                 */
                shard_role_details::getRecoveryUnit(opCtx)->onCommit(
                    [&](OperationContext* opCtx, boost::optional<Timestamp>) {
                        ShardingReady::get(opCtx)->setIsReady();
                    });
            }
        }
    }

    // TODO (SERVER-91505): Change this to check isDataConsistent instead of
    // isInInitialSyncOrRollback.
    if (!repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        boost::optional<Timestamp> maxTopologyTime;
        for (auto it = begin; it != end; it++) {
            Timestamp newTopologyTime = it->doc[ShardType::topologyTime.name()].timestamp();
            if (newTopologyTime != Timestamp()) {
                if (!maxTopologyTime || newTopologyTime > *maxTopologyTime) {
                    maxTopologyTime = newTopologyTime;
                }
            }
        }

        if (maxTopologyTime) {
            // Insertions into config.shards may be done inside a transaction. This implies that the
            // callback from onCommit can be invoked by a different thread. Since the
            // TopologyTimeTicker is associated to the mongod instance and not to the
            // OperationContext, we can safely obtain a reference at this point and passed it to the
            // onCommit callback.
            auto& topologyTicker = TopologyTimeTicker::get(opCtx);
            shard_role_details::getRecoveryUnit(opCtx)->onCommit(
                [&topologyTicker, maxTopologyTime](OperationContext* opCtx,
                                                   boost::optional<Timestamp> commitTime) mutable {
                    invariant(commitTime);
                    topologyTicker.onNewLocallyCommittedTopologyTimeAvailable(*commitTime,
                                                                              *maxTopologyTime);
                });
        }
    }
}

void ConfigServerOpObserver::onUpdate(OperationContext* opCtx,
                                      const OplogUpdateEntryArgs& args,
                                      OpStateAccumulator* opAccumulator) {
    if (args.coll->ns() != NamespaceString::kConfigsvrShardsNamespace) {
        return;
    }

    // TODO (SERVER-91505): Change this to check isDataConsistent instead of
    // isInInitialSyncOrRollback.
    if (repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        return;
    }

    const auto& updateDoc = args.updateArgs->update;

    if (update_oplog_entry::extractUpdateType(updateDoc) ==
        update_oplog_entry::UpdateType::kReplacement) {
        return;
    }

    auto topologyTimeValue =
        update_oplog_entry::extractNewValueForField(updateDoc, ShardType::topologyTime());
    if (!topologyTimeValue.ok()) {
        return;
    }

    auto topologyTime = topologyTimeValue.timestamp();
    if (topologyTime == Timestamp()) {
        return;
    }

    // Updates to config.shards are always done inside a transaction. This implies that the callback
    // from onCommit can be called in a different thread. Since the TopologyTimeTicker is associated
    // to the mongod instance and not to the OperationContext, we can safely obtain a reference at
    // this point and passed it to the onCommit callback.
    auto& topologyTicker = TopologyTimeTicker::get(opCtx);
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [&topologyTicker, topologyTime](OperationContext*,
                                        boost::optional<Timestamp> commitTime) mutable {
            invariant(commitTime);
            topologyTicker.onNewLocallyCommittedTopologyTimeAvailable(*commitTime, topologyTime);
        });
}

void ConfigServerOpObserver::onMajorityCommitPointUpdate(ServiceContext* service,
                                                         const repl::OpTime& newCommitPoint) {
    Timestamp newCommitPointTime = newCommitPoint.getTimestamp();

    // TopologyTime must always be <= ConfigTime, so ticking them separately is fine as long as
    // ConfigTime is done first.
    VectorClockMutable::get(service)->tickConfigTimeTo(LogicalTime(newCommitPointTime));

    // Letting the TopologyTimeTicker know that the majority commit point was advanced
    TopologyTimeTicker::get(service).onMajorityCommitPointUpdate(service, newCommitPoint);
}

}  // namespace mongo
