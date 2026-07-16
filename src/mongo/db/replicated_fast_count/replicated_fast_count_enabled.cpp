// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"

#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/repl/local_oplog_info.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/version_context.h"

namespace mongo {
bool isReplicatedFastCountEnabled(OperationContext* opCtx) {
    // TODO(SERVER-117326): Remove feature flag check.
    return (rss::ReplicatedStorageService::get(opCtx)
                .getPersistenceProvider()
                .shouldUseReplicatedFastCount() ||
            gFeatureFlagReplicatedFastCount.isEnabledUseLatestFCVWhenUninitialized(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) &&
        repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet();
}

bool isReplicatedFastCountEligible(const NamespaceString& nss) {
    if (nss.isOplog() && gFeatureFlagSizeBasedOplogTruncationForDisagg.isEnabled()) {
        return true;
    }
    if (nss.isLocalDB() || nss.isImplicitlyReplicated() || nss.isServerConfigurationCollection() ||
        nss.isSystemDotProfile()) {
        return false;
    }
    // Exclude the fast count store collections themselves to avoid circular tracking.
    const auto fastCountStoreNss =
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);
    const auto fastCountTimestampsNss = NamespaceString::makeGlobalConfigCollection(
        NamespaceString::kReplicatedFastCountStoreTimestamps);
    return nss != fastCountStoreNss && nss != fastCountTimestampsNss;
}

bool shouldReadFromReplicatedFastCount(OperationContext* opCtx, const NamespaceString& nss) {
    if (!isReplicatedFastCountEligible(nss)) {
        return false;
    }

    if (rss::ReplicatedStorageService::get(opCtx)
            .getPersistenceProvider()
            .shouldUseReplicatedFastCount()) {
        return true;
    }

    if (!gFeatureFlagReplicatedFastCount.isEnabledUseLatestFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return false;
    }

    // TODO SERVER-129370: Updated conditions for when to read from replicated fast count when the
    // persistence provider does not use replicated fast count.
    return repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet() && !nss.isOplog();
}

bool shouldUseReplicatedFastCountContainers(OperationContext* opCtx) {
    return rss::ReplicatedStorageService::get(opCtx)
               .getPersistenceProvider()
               .mustUseContainerWrites() ||
        feature_flags::gContainerWrites.isEnabledUseLatestFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
}

bool isReplicatedFastCountListCollectionsEnabled(OperationContext* opCtx) {
    if (!getTestCommandsEnabled()) {
        return false;
    }
    const auto vCtx = VersionContext::getDecoration(opCtx);
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    // We don't consult the mustUseContainerWrites or shouldUseReplicatedFastCount persistence
    // provider fields since this is test only functionality.
    return gFeatureFlagReplicatedFastCount.isEnabledUseLatestFCVWhenUninitialized(vCtx,
                                                                                  fcvSnapshot) &&
        feature_flags::gContainerWrites.isEnabledUseLatestFCVWhenUninitialized(vCtx, fcvSnapshot);
}

bool isReplicatedFastCountInitialSyncEnabled(OperationContext* opCtx) {
    if (!getTestCommandsEnabled()) {
        return false;
    }
    const auto vCtx = VersionContext::getDecoration(opCtx);
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    return gFeatureFlagReplicatedFastCount.isEnabledUseLatestFCVWhenUninitialized(vCtx,
                                                                                  fcvSnapshot);
}

}  // namespace mongo
