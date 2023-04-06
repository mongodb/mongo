/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/config/placement_history_cleaner.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

static constexpr int kminPlacementHistoryEntries = 100 * 1000;  // 100k entries
static constexpr Seconds kJobExecutionPeriod{60 * 60 * 24};     // 1 day

const auto placementHistoryCleanerDecorator =
    ServiceContext::declareDecoration<PlacementHistoryCleaner>();

boost::optional<Timestamp> getEarliestOpLogTimestampAmongAllShards(OperationContext* opCtx) {
    // send server status and get earliest oplog time
    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    const auto serverStatusCmd = BSON("serverStatus" << 1 << "oplog" << 1);
    const auto recipients = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

    auto responses = sharding_util::sendCommandToShards(opCtx,
                                                        DatabaseName::kAdmin.db(),
                                                        serverStatusCmd,
                                                        recipients,
                                                        executor,
                                                        false /*throwOnError*/);
    boost::optional<Timestamp> earliestOplogTime = boost::none;
    for (const auto& cmdResponse : responses) {
        if (!cmdResponse.swResponse.isOK()) {
            LOGV2_DEBUG(
                7068800,
                2,
                "getEarliestOpLogTimestampAmongAllShards - Failed to collect the earliest op "
                "time from one of the shards");
            earliestOplogTime = boost::none;
            break;
        }
        auto receivedOpTime =
            cmdResponse.swResponse.getValue().data["oplog"]["earliestOptime"].timestamp();
        // Shards returning Timestamp(0,0) as earliestOptime have not yet dropped any entry from
        // their oplogs. Disregard such values.
        if (receivedOpTime > Timestamp(0, 0)) {
            if (earliestOplogTime) {
                earliestOplogTime = std::min(receivedOpTime, *earliestOplogTime);
            } else {
                earliestOplogTime = receivedOpTime;
            }
        }
    }

    return earliestOplogTime;
}
}  // namespace

const ReplicaSetAwareServiceRegistry::Registerer<PlacementHistoryCleaner>
    placementHistoryCleanerRegistryRegisterer("PlacementHistoryCleaner");


PlacementHistoryCleaner* PlacementHistoryCleaner::get(ServiceContext* serviceContext) {
    return &placementHistoryCleanerDecorator(serviceContext);
}

PlacementHistoryCleaner* PlacementHistoryCleaner::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void PlacementHistoryCleaner::runOnce(Client* client, size_t minPlacementHistoryDocs) {
    auto opCtx = client->makeOperationContext();
    opCtx.get()->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    try {
        // Count the number of entries in the placementHistory collection; skip cleanup if below
        // threshold.
        const auto numPlacementHistoryDocs = [&] {
            PersistentTaskStore<NamespacePlacementType> store(
                NamespaceString::kConfigsvrPlacementHistoryNamespace);
            return store.count(opCtx.get(), BSONObj());
        }();

        if (numPlacementHistoryDocs <= minPlacementHistoryDocs) {
            LOGV2_DEBUG(7068801, 3, "PlacementHistoryCleaner: nothing to be deleted on this round");
            return;
        }

        // Get the time of the oldest op entry still persisted among the cluster shards; historical
        // placement entries that precede it may be safely dropped.
        auto earliestOplogTime = getEarliestOpLogTimestampAmongAllShards(opCtx.get());
        if (!earliestOplogTime) {
            LOGV2(7068802,
                  "Skipping cleanup of config.placementHistory - no earliestOplogTime could "
                  "be retrieved");
            return;
        }

        ShardingCatalogManager::get(opCtx.get())
            ->cleanUpPlacementHistory(opCtx.get(), *earliestOplogTime);
    } catch (const DBException& e) {
        LOGV2(7068804, "Periodic cleanup of config.placementHistory failed", "error"_attr = e);
    }
}

void PlacementHistoryCleaner::onStepUpComplete(OperationContext* opCtx, long long term) {
    auto periodicRunner = opCtx->getServiceContext()->getPeriodicRunner();
    invariant(periodicRunner);

    PeriodicRunner::PeriodicJob placementHistoryCleanerJob(
        "PlacementHistoryCleanUpJob",
        [](Client* client) { runOnce(client, kminPlacementHistoryEntries); },
        kJobExecutionPeriod);

    _anchor = periodicRunner->makeJob(std::move(placementHistoryCleanerJob));
    _anchor.start();
}
void PlacementHistoryCleaner::onStepDown() {
    if (_anchor.isValid()) {
        _anchor.stop();
    }
}
}  // namespace mongo
