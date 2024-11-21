/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/util/concurrency/ticketholder_queue_stats.h"

#include <utility>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/admission/ingress_admission_context.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/duration.h"

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
        }
        if (stats.totalTimeQueuedMicros > 0) {
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
