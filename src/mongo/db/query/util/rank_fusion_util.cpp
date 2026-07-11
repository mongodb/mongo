// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/util/rank_fusion_util.h"

#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"

namespace mongo {
bool isRankFusionFullEnabled() {
    return bypassRankFusionFCVGate ||
        feature_flags::gFeatureFlagRankFusionFull.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
}
}  // namespace mongo
