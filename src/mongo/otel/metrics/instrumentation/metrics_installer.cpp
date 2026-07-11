// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/metrics_installer.h"

#include "mongo/db/admission/ingress_request_rate_limiter.h"
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

namespace mongo {

void installCommonOtelMetrics(ServiceContext* svcCtx) {
    installSystemMountOtelMetrics(svcCtx);
    installDiskOtelMetrics(svcCtx);
    installProcessHealthOtelMetrics(svcCtx);
    installSystemHealthOtelMetrics(svcCtx);
    installObservableMutexMetrics(svcCtx);
    installMongoDBBuildInfoMetrics();
    installQueryMemoryOtelMetrics(svcCtx);
    admission::IngressRequestRateLimiter::get(svcCtx).installOtelMetrics(svcCtx);
}

void installMongodOtelMetrics(ServiceContext* svcCtx) {
    installCommonOtelMetrics(svcCtx);
    installGlobalLockOtelMetrics(svcCtx);
    installIndexBuildOtelMetrics(svcCtx);
    installConnectionsOtelMetrics(svcCtx);
}

void installMongosOtelMetrics(ServiceContext* svcCtx) {
    installCommonOtelMetrics(svcCtx);
}

}  // namespace mongo
