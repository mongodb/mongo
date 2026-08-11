/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/stats/system_buckets_metrics.h"

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

// TODO SERVER-119235: Remove chunk-related commands once they support rawData.
static const StringDataSet kCommandsAllowedToTargetBuckets = {"moveChunk"_sd,
                                                              "split"_sd,
                                                              "mergeChunks"_sd,
                                                              "moveRange"_sd,
                                                              "clearJumboFlag"_sd,
                                                              "cleanupOrphaned"_sd,
                                                              "mergeAllChunksOnShard"_sd,
                                                              "configureCollectionBalancing"_sd,
                                                              "balancerCollectionStatus"_sd,
                                                              "getMore"_sd};

SystemBucketsMetricsCommandHooks::SystemBucketsMetricsCommandHooks() {
    _commandsExecuted = &*MetricBuilder<Counter64>("numCommandsTargetingSystemBuckets");
}

void SystemBucketsMetricsCommandHooks::onBeforeRun(OperationContext* opCtx,
                                                   const OpMsgRequest& request,
                                                   CommandInvocation* invocation) {

    if (!invocation->ns().isTimeseriesBucketsCollection()) {
        return;
    }

    if (kCommandsAllowedToTargetBuckets.contains(invocation->definition()->getName())) {
        return;
    }

    const auto& client = *opCtx->getClient();
    if (
        // This command has been initiated by another command (e.g. DBDirectClient)
        (client.isInDirectClient() || !client.session()) ||
        // This command comes from another node within the same cluster
        client.isInternalClient()) {
        return;
    }

    StringData appName;
    StringData driverName;
    if (auto clientMetadata = ClientMetadata::get(opCtx->getClient())) {
        appName = clientMetadata->getApplicationName();
        driverName = clientMetadata->getDriverName();
    }

    LOGV2_DEBUG(11259900,
                _logSuppressor().toInt(),
                "Received command directly targeting a system.buckets namespace",
                "command"_attr = invocation->definition()->getName(),
                "namespace"_attr = invocation->ns().toStringForErrorMsg(),
                "client"_attr = client.clientAddress(true),
                "connId"_attr = client.getConnectionId(),
                "appName"_attr = appName,
                "driverName"_attr = driverName);

    _commandsExecuted->increment();
}

}  // namespace mongo
