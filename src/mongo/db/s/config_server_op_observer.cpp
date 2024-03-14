/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <set>

#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/config_server_op_observer.h"
#include "mongo/db/s/sharding_ready.h"
#include "mongo/db/s/topology_time_ticker.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

ConfigServerOpObserver::ConfigServerOpObserver() = default;

ConfigServerOpObserver::~ConfigServerOpObserver() = default;

void ConfigServerOpObserver::onDelete(OperationContext* opCtx,
                                      const CollectionPtr& coll,
                                      StmtId stmtId,
                                      const BSONObj& doc,
                                      const OplogDeleteEntryArgs& args,
                                      OpStateAccumulator* opAccumulator) {
    if (coll->ns() == VersionType::ConfigNS) {
        if (!repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback()) {
            uasserted(40302, "cannot delete config.version document while in --configsvr mode");
        } else {
            // TODO (SERVER-34165): this is only used for rollback via refetch and can be removed
            // with it.
            // Throw out any cached information related to the cluster ID.
            ShardingCatalogManager::get(opCtx)->discardCachedConfigDatabaseInitializationState();
            ClusterIdentityLoader::get(opCtx)->discardCachedClusterId();
        }
    }
}

repl::OpTime ConfigServerOpObserver::onDropCollection(OperationContext* opCtx,
                                                      const NamespaceString& collectionName,
                                                      const UUID& uuid,
                                                      std::uint64_t numRecords,
                                                      const CollectionDropType dropType,
                                                      bool markFromMigrate) {
    if (collectionName == VersionType::ConfigNS) {
        if (!repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback()) {
            uasserted(40303, "cannot drop config.version document while in --configsvr mode");
        } else {
            // TODO (SERVER-34165): this is only used for rollback via refetch and can be removed
            // with it.
            // Throw out any cached information related to the cluster ID.
            ShardingCatalogManager::get(opCtx)->discardCachedConfigDatabaseInitializationState();
            ClusterIdentityLoader::get(opCtx)->discardCachedClusterId();
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

    // When doing a magic restore, we want to be able to write config.shards without triggering the
    // below.
    if (storageGlobalParams.magicRestore) {
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

    if (!topology_time_ticker_utils::inRecoveryMode(opCtx)) {
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
