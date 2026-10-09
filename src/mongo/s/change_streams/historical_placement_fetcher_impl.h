// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/historical_placement_fetcher.h"
#include "mongo/s/change_streams/historical_placement_fetcher_metrics.h"
#include "mongo/util/modules.h"

namespace mongo {

class HistoricalPlacementFetcherImpl : public HistoricalPlacementFetcher {
public:
    explicit HistoricalPlacementFetcherImpl(HistoricalPlacementFetcherMetricsRecorder metrics =
                                                getHistoricalPlacementFetcherMetricsRecorder())
        : _metrics(metrics) {}

    /**
     * Issues ConfigsvrGetHistoricalPlacement command to the configsvr for the given namespace 'nss'
     * for 'atClusterTime' time.
     */
    HistoricalPlacement fetch(OperationContext* opCtx,
                              const boost::optional<NamespaceString>& nss,
                              Timestamp atClusterTime,
                              bool checkIfPointInTimeIsInFuture,
                              bool ignoreRemovedShards) override;

private:
    HistoricalPlacementFetcherMetricsRecorder _metrics;
};

}  // namespace mongo
