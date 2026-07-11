// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/ticketing/ticketholder_queue_stats.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/ingress_admission_context.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/admission/write_throttler_admission_context.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/duration.h"

#include <utility>

namespace mongo {
namespace {

std::map<TicketHolderQueueStats::QueueType, std::function<AdmissionContext*(OperationContext*)>>
    gQueueMetricsRegistry;

MONGO_INITIALIZER(InitGlobalQueueLookupTable)(InitializerContext*) {
    gQueueMetricsRegistry[TicketHolderQueueStats::QueueType::Ingress] =
        [](OperationContext* opCtx) {
            return &IngressAdmissionContext::get(opCtx);
        };
    gQueueMetricsRegistry[TicketHolderQueueStats::QueueType::Execution] =
        [](OperationContext* opCtx) {
            return &ExecutionAdmissionContext::get(opCtx);
        };
    gQueueMetricsRegistry[TicketHolderQueueStats::QueueType::WriteThrottle] =
        [](OperationContext* opCtx) {
            return &WriteThrottlerAdmissionContext::get(opCtx);
        };
}
}  // namespace

std::map<TicketHolderQueueStats::QueueType, std::function<AdmissionContext*(OperationContext*)>>
TicketHolderQueueStats::getQueueMetricsRegistry() {
    return gQueueMetricsRegistry;
}


TicketHolderQueueStats::TicketHolderQueueStats(OperationContext* opCtx) {
    for (auto&& [queueName, lookup] : gQueueMetricsRegistry) {
        AdmissionContext* admCtx = lookup(opCtx);
        _statsMap[queueName].admissions = admCtx->getAdmissions();
        _statsMap[queueName].totalTimeQueuedMicros =
            durationCount<Microseconds>(admCtx->totalTimeQueuedMicros());
    }
}

BSONObj TicketHolderQueueStats::toBson() const {
    BSONObjBuilder queuesBuilder;
    for (const auto& [queueName, stats] : _statsMap) {
        BSONObjBuilder bb;
        if (stats.admissions > 0) {
            bb.append("admissions", stats.admissions);
            bb.append("totalTimeQueuedMicros", stats.totalTimeQueuedMicros);
        }
        queuesBuilder.append(queueTypeToString(queueName), bb.obj());
    }
    return queuesBuilder.obj();
}

void TicketHolderQueueStats::add(const TicketHolderQueueStats& other) {
    for (const auto& [key, otherStats] : other._statsMap) {
        _statsMap[key].admissions += otherStats.admissions;
        _statsMap[key].totalTimeQueuedMicros += otherStats.totalTimeQueuedMicros;
    }
}
}  // namespace mongo
