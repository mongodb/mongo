// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/historical_placement_fetcher_impl.h"

#include "mongo/db/global_catalog/ddl/placement_history_commands_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

namespace mongo {

HistoricalPlacement HistoricalPlacementFetcherImpl::fetch(
    OperationContext* opCtx,
    const boost::optional<NamespaceString>& nss,
    Timestamp atClusterTime,
    bool checkIfPointInTimeIsInFuture,
    bool ignoreRemovedShards) {
    // config.placementHistory never contains a marker earlier than the 'Dawn of Time' entry at
    // Timestamp(0, 1), so a query for the zero timestamp could never be satisfied. Normalize it to
    // the earliest queryable point in time so that change stream callers may legitimately use
    // Timestamp(0, 0) as a placeholder for "the beginning of time".
    if (atClusterTime == Timestamp(0, 0)) {
        atClusterTime = Timestamp(0, 1);
    }

    // The config server request must always have a namespace string, even if it is the empty
    // string.
    const auto targetWholeCluster = !nss.has_value() || nss->isEmpty();
    ConfigsvrGetHistoricalPlacement request(targetWholeCluster ? NamespaceString::kEmpty
                                                               : nss.value(),
                                            atClusterTime,
                                            ignoreRemovedShards);
    request.setTargetWholeCluster(targetWholeCluster);
    request.setCheckIfPointInTimeIsInFuture(checkIfPointInTimeIsInFuture);

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto remoteResponse = uassertStatusOK(
        configShard->runCommand(opCtx,
                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                DatabaseName::kAdmin,
                                request.toBSON(),
                                Milliseconds(defaultConfigCommandTimeoutMS.load()),
                                Shard::RetryPolicy::kIdempotentOrCursorInvalidated));
    uassertStatusOK(remoteResponse.commandStatus);

    return ConfigsvrGetHistoricalPlacementResponse::parse(
               remoteResponse.response, IDLParserContext("HistoricalPlacementFetcherImpl"))
        .getHistoricalPlacement();
}

}  // namespace mongo
