// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/ingress_admission_context.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * TicketHolderQueueStats should only be constructed from a completed operation. It retrieves
 * queueing statistics for the operation and stores them so they can be accumulated with future
 * operations (for transactions) or serialized and outputed to the user. Statistics are outputted
 * in slow query and slow transaction logging.
 */
class [[MONGO_MOD_PUBLIC]] TicketHolderQueueStats {
public:
    enum class QueueType { Ingress, Execution, WriteThrottle };

    TicketHolderQueueStats() = default;

    /**
     * Uses the AdmissionContext on the OperationContext to populate stats for both
     * ingress and execution control queues.
     */
    TicketHolderQueueStats(OperationContext* opCtx);

    /**
     * Serializes to BSON for use in slow query logging. Any k/v pairs
     * where the value is zero will be omitted from the output.
     */
    BSONObj toBson() const;

    /**
     * Aggregates queueing statistics on this instance by adding values from otherQueueStats. Used
     * to accumulate metrics across operations in a transaction for slow transaction logging.
     */
    void add(const TicketHolderQueueStats& otherQueueStats);

    static std::map<TicketHolderQueueStats::QueueType,
                    std::function<AdmissionContext*(OperationContext*)>>
    getQueueMetricsRegistry();

    static std::string queueTypeToString(QueueType queueType) {
        switch (queueType) {
            case QueueType::Ingress:
                return "ingress";
            case QueueType::Execution:
                return "execution";
            case QueueType::WriteThrottle:
                return "writeThrottle";
            default:
                MONGO_UNREACHABLE;
        }
    }

private:
    struct Stats {
        int admissions = 0;
        long long totalTimeQueuedMicros = 0;
    };

    std::map<QueueType, Stats> _statsMap;
};

}  // namespace mongo
