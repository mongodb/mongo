// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/persistence/persistent_stats_config_validation.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/version_context.h"

namespace mongo {

Status validatePersistentStatsConfigIsApplicable(OperationContext* opCtx, bool isView) {
    if (!feature_flags::gFeatureFlagPersistentStats.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return {ErrorCodes::InvalidOptions, "unknown option: persistentStatsConfig"};
    }

    if (isView) {
        return {ErrorCodes::InvalidOptions,
                "option not supported on a view: persistentStatsConfig"};
    }

    return Status::OK();
}

Status validatePersistentStatsConfig(const PersistentStatsConfig& persistentStatsConfig) {
    const auto& samples = persistentStatsConfig.getSamples();
    if (!samples || (!samples->getSampleSize() && !samples->getJoinSampleSize())) {
        return {ErrorCodes::InvalidOptions,
                "'persistentStatsConfig' must specify at least one of 'samples.sampleSize' or "
                "'samples.joinSampleSize'"};
    }

    return Status::OK();
}

Status validatePersistentStatsConfigUpdate(const PersistentStatsConfigUpdate& update) {
    const auto& samples = update.getSamples();
    if (!samples || (!samples->getSampleSize() && !samples->getJoinSampleSize())) {
        return {ErrorCodes::InvalidOptions,
                "'persistentStatsConfig' must specify at least one of 'samples.sampleSize' or "
                "'samples.joinSampleSize'"};
    }

    return Status::OK();
}

}  // namespace mongo
