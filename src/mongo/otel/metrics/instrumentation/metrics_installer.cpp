// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/metrics_installer.h"

#include "mongo/db/admission/ingress_request_rate_limiter.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/otel/metrics/instrumentation/connections_metrics.h"
#include "mongo/otel/metrics/instrumentation/disk_metrics.h"
#include "mongo/otel/metrics/instrumentation/global_lock_metrics.h"
#include "mongo/otel/metrics/instrumentation/index_build_metrics.h"
#include "mongo/otel/metrics/instrumentation/mongodb_build_info_metrics.h"
#include "mongo/otel/metrics/instrumentation/observable_mutex_metrics.h"
#include "mongo/otel/metrics/instrumentation/process_health_metrics.h"
#include "mongo/otel/metrics/instrumentation/query_memory_metrics.h"
#include "mongo/otel/metrics/instrumentation/system_health_metrics.h"
#include "mongo/otel/metrics/instrumentation/system_mount_metrics.h"
#include "mongo/otel/metrics/instrumentation/wiredtiger_metrics.h"

namespace mongo {

namespace {
#ifdef MONGO_CONFIG_OTEL
void installCommonOtelMetrics(ServiceContext* svcCtx) {
    if (!gFeatureFlagOtelMetrics.isEnabled())
        return;

    installSystemMountOtelMetrics(svcCtx);
    installDiskOtelMetrics(svcCtx);
    installProcessHealthOtelMetrics(svcCtx);
    installSystemHealthOtelMetrics(svcCtx);
    installObservableMutexMetrics(svcCtx);
    installMongoDBBuildInfoMetrics();
    installQueryMemoryOtelMetrics(svcCtx);
    admission::IngressRequestRateLimiter::get(svcCtx).installOtelMetrics(svcCtx);
}
#endif
}  // namespace

void installMongodOtelMetrics(ServiceContext* svcCtx) {
    if (!gFeatureFlagOtelMetrics.isEnabled())
        return;

#ifdef MONGO_CONFIG_OTEL
    installCommonOtelMetrics(svcCtx);
    installGlobalLockOtelMetrics(svcCtx);
    installIndexBuildOtelMetrics(svcCtx);
    installConnectionsOtelMetrics(svcCtx);
    installWiredTigerOtelMetrics(svcCtx);
#endif
}

void installMongosOtelMetrics(ServiceContext* svcCtx) {
    if (!gFeatureFlagOtelMetrics.isEnabled())
        return;

#ifdef MONGO_CONFIG_OTEL
    installCommonOtelMetrics(svcCtx);
    installConnectionsOtelMetrics(svcCtx);
#endif
}


}  // namespace mongo
