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

#include "mongo/s/analyze_shard_key_role.h"

#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/topology/cluster_role.h"

namespace mongo {
namespace analyze_shard_key {

namespace {

bool isReplEnabled(ServiceContext* serviceContext) {
    if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        return false;
    }
    auto replCoord = repl::ReplicationCoordinator::get(serviceContext);
    return replCoord && replCoord->getSettings().isReplSet();
}

}  // namespace

bool supportsCoordinatingQueryAnalysis(bool isReplEnabled) {
    if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        return false;
    }
    return isReplEnabled && !gMultitenancySupport &&
        (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) ||
         serverGlobalParams.clusterRole.has(ClusterRole::None));
}

bool supportsCoordinatingQueryAnalysis(OperationContext* opCtx) {
    return supportsCoordinatingQueryAnalysis(isReplEnabled(opCtx->getServiceContext()));
}

bool supportsPersistingSampledQueries(bool isReplEnabled) {
    if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        return false;
    }
    return isReplEnabled && !gMultitenancySupport &&
        (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
         serverGlobalParams.clusterRole.has(ClusterRole::None));
}

bool supportsPersistingSampledQueries(OperationContext* opCtx) {
    return supportsPersistingSampledQueries(isReplEnabled(opCtx->getServiceContext()));
}

bool supportsSamplingQueries(bool isReplEnabled) {
    if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        return true;
    }
    return isReplEnabled && !gMultitenancySupport &&
        (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
         serverGlobalParams.clusterRole.has(ClusterRole::None));
}

bool supportsSamplingQueries(ServiceContext* serviceContext) {
    return supportsSamplingQueries(isReplEnabled(serviceContext));
}

bool supportsSamplingQueries(OperationContext* opCtx) {
    return supportsSamplingQueries(opCtx->getServiceContext());
}

}  // namespace analyze_shard_key
}  // namespace mongo
