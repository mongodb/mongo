/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#pragma once

#include <array>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <type_traits>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/ticket_pool.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {
enum class QueueType : unsigned int { kLowPriority = 0, kNormalPriority = 1, NumQueues = 2 };

}  // namespace

class Ticket;

/**
 * A PriorityTicketHolder supports queueing and prioritization of operations based on
 * AdmissionContext::Priority.
 *
 * MODIFICATIONS TO THIS CLASS MUST BE ACCOMPANIED BY AN UPDATE OF
 * src/mongo/tla_plus/PriorityTicketHolder/MCPriorityTicketHolder.tla TO ENSURE IT IS CORRECT
 */
class PriorityTicketHolder : public TicketHolder {
public:
    explicit PriorityTicketHolder(ServiceContext* serviceContext,
                                  int32_t numTickets,
                                  int32_t lowPriorityBypassThreshold,
                                  bool trackPeakUsed);
    ~PriorityTicketHolder() override{};

    int32_t available() const override final;

    int64_t queued() const override final;

    int64_t numFinishedProcessing() const override final;

    std::int64_t expedited() const;

    std::int64_t bypassed() const;

    void updateLowPriorityAdmissionBypassThreshold(int32_t newBypassThreshold);

private:
    boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) override final;

    boost::optional<Ticket> _waitForTicketUntilImpl(Interruptible& interruptible,
                                                    AdmissionContext* admCtx,
                                                    Date_t until) override final;

    void _releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept override final;

    QueueStats& _getQueueStatsToUse(AdmissionContext::Priority priority) noexcept override final;

    void _appendImplStats(BSONObjBuilder& b) const override final;

    static QueueType _getQueueType(AdmissionContext::Priority priority) {
        switch (priority) {
            case AdmissionContext::Priority::kLow:
                return QueueType::kLowPriority;
            case AdmissionContext::Priority::kNormal:
                return QueueType::kNormalPriority;
            default:
                MONGO_UNREACHABLE;
        }
    }

    static unsigned int _enumToInt(QueueType queueType) {
        return static_cast<std::underlying_type_t<QueueType>>(queueType);
    }

    std::array<QueueStats, static_cast<unsigned int>(QueueType::NumQueues)> _stats;
    ServiceContext* _serviceContext;
    TicketPool<SimplePriorityTicketQueue> _pool;
};
}  // namespace mongo
