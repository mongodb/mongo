// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_UNFORTUNATELY_OPEN]] HistoricalPlacementFetcher {
public:
    virtual ~HistoricalPlacementFetcher() = default;

    /**
     * Fetches HistoricalPlacement information for the given namespace 'nss' at time
     * 'atClusterTime'. 'checkIfPointInTimeIsInFuture' is passed as false by default as
     * HistoricalPlacementFetcher is used primarily by ChangeStreamShardTargeters that handle events
     * for already running change streams that can not return change events with the cluster time in
     * the future.
     */
    virtual HistoricalPlacement fetch(OperationContext* opCtx,
                                      const boost::optional<NamespaceString>& nss,
                                      Timestamp atClusterTime,
                                      bool checkIfPointInTimeIsInFuture,
                                      bool ignoreRemovedShards) = 0;
};

}  // namespace mongo
