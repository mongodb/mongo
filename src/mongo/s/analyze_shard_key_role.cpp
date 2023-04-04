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
#include "mongo/s/analyze_shard_key_feature_flag_gen.h"
#include "mongo/s/is_mongos.h"

namespace mongo {
namespace analyze_shard_key {

namespace {

bool isReplEnabled(ServiceContext* serviceContext) {
    if (isMongos()) {
        return false;
    }
    auto replCoord = repl::ReplicationCoordinator::get(serviceContext);
    return replCoord && replCoord->isReplEnabled();
}

}  // namespace

bool isFeatureFlagEnabled(bool ignoreFCV) {
    if (ignoreFCV) {
        // (Ignore FCV check): In the following cases, ignoreFCV is set to true.
        // 1. The call is before FCV initialization.
        // 2. We want to stop QueryAnalysisSampler regardless of FCV.
        return gFeatureFlagAnalyzeShardKey.isEnabledAndIgnoreFCVUnsafe();
    }
    return serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        gFeatureFlagAnalyzeShardKey.isEnabled(serverGlobalParams.featureCompatibility);
}

bool supportsCoordinatingQueryAnalysis(bool isReplEnabled, bool ignoreFCV) {
    if (!isFeatureFlagEnabled(ignoreFCV)) {
        return false;
    }
    if (isMongos()) {
        return false;
    }
    return isReplEnabled && !gMultitenancySupport &&
        (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) ||
         serverGlobalParams.clusterRole.has(ClusterRole::None));
}

bool supportsCoordinatingQueryAnalysis(OperationContext* opCtx, bool ignoreFCV) {
    return supportsCoordinatingQueryAnalysis(isReplEnabled(opCtx->getServiceContext()), ignoreFCV);
}

bool supportsPersistingSampledQueries(bool isReplEnabled, bool ignoreFCV) {
    if (!isFeatureFlagEnabled(ignoreFCV)) {
        return false;
    }
    if (isMongos()) {
        return false;
    }
    return isReplEnabled && !gMultitenancySupport &&
        (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
         serverGlobalParams.clusterRole.has(ClusterRole::None));
}

bool supportsPersistingSampledQueries(OperationContext* opCtx, bool ignoreFCV) {
    return supportsPersistingSampledQueries(isReplEnabled(opCtx->getServiceContext()), ignoreFCV);
}

bool supportsSamplingQueries(bool isReplEnabled, bool ignoreFCV) {
    if (!isFeatureFlagEnabled(ignoreFCV)) {
        return false;
    }
    if (isMongos()) {
        return true;
    }
    return isReplEnabled && !gMultitenancySupport &&
        (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
         serverGlobalParams.clusterRole.has(ClusterRole::None));
}

bool supportsSamplingQueries(ServiceContext* serviceContext, bool ignoreFCV) {
    return supportsSamplingQueries(isReplEnabled(serviceContext), ignoreFCV);
}

bool supportsSamplingQueries(OperationContext* opCtx, bool ignoreFCV) {
    return supportsSamplingQueries(opCtx->getServiceContext(), ignoreFCV);
}

}  // namespace analyze_shard_key
}  // namespace mongo
