// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/tracing_feature_flags_gen.h"

namespace mongo::otel::traces {

bool isTracingEnabled(OperationContext* opCtx) {
#ifndef MONGO_CONFIG_OTEL
    return false;
#else
    if (!opCtx) {
        return false;
    }
    const auto fcv = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    if (!fcv.isVersionInitialized()) {
        return false;
    }
    const auto& context = VersionContext::getDecoration(opCtx);
    return feature_flags::gFeatureFlagTracing.isEnabled(context, fcv) &&
        feature_flags::gFeatureFlagOtelTraceSampling.isEnabled(context, fcv);
#endif
}

}  // namespace mongo::otel::traces
