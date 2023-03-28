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

#include <queue>

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/ticket_pool.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {
enum class QueueType : unsigned int { kLowPriority = 0, kNormalPriority = 1, NumQueues = 2 };

}  // namespace

class Ticket;

/**
 * This SimplePriorityTicketQueue implements a queue policy that separates normal and low priority
 * operations into separate queues. Normal priority operations are always scheduled ahead of low
 * priority ones, except when a positive lowPriorityBypassThreshold is provided. This parameter
 * specifies how often a waiting low-priority operation should skip the queue and be scheduled ahead
 * of waiting normal priority operations.
 */
class SimplePriorityTicketQueue : public TicketQueue {
public:
    SimplePriorityTicketQueue(int lowPriorityBypassThreshold)
        : _lowPriorityBypassThreshold(lowPriorityBypassThreshold) {}

    bool empty() const final {
        return _normal.empty() && _low.empty();
    }

    void push(std::shared_ptr<TicketWaiter> val) final {
        if (val->context->getPriority() == AdmissionContext::Priority::kLow) {
            _low.push(std::move(val));
            return;
        }
        invariant(val->context->getPriority() == AdmissionContext::Priority::kNormal);
        _normal.push(std::move(val));
    }

    std::shared_ptr<TicketWaiter> pop() final {
        if (!_normal.empty() && !_low.empty() && _lowPriorityBypassThreshold.load() > 0 &&
            _lowPriorityBypassCount.fetchAndAdd(1) % _lowPriorityBypassThreshold.load() == 0) {
            auto front = std::move(_low.front());
            _low.pop();
            _expeditedLowPriorityAdmissions.addAndFetch(1);
            return front;
        }
        if (!_normal.empty()) {
            auto front = std::move(_normal.front());
            _normal.pop();
            return front;
        }
        auto front = std::move(_low.front());
        _low.pop();
        return front;
    }

    /**
     * Number of times low priority operations are expedited for ticket admission over normal
     * priority operations.
     */
    std::int64_t expedited() const {
        return _expeditedLowPriorityAdmissions.loadRelaxed();
    }

    /**
     * Returns the number of times the low priority queue is bypassed in favor of dequeuing from the
     * normal priority queue when a ticket becomes available.
     */
    std::int64_t bypassed() const {
        return _lowPriorityBypassCount.loadRelaxed();
    }

    void updateLowPriorityAdmissionBypassThreshold(int32_t newBypassThreshold) {
        _lowPriorityBypassThreshold.store(newBypassThreshold);
    }

private:
    /**
     * Limits the number times the low priority queue is non-empty and bypassed in favor of the
     * normal priority queue for the next ticket admission.
     */
    AtomicWord<std::int32_t> _lowPriorityBypassThreshold;

    /**
     * Number of times ticket admission is expedited for low priority operations.
     */
    AtomicWord<std::int64_t> _expeditedLowPriorityAdmissions{0};

    /**
     * Counts the number of times normal operations are dequeued over operations queued in the low
     * priority queue. We explicitly use an unsigned type here because rollover is desired.
     */
    AtomicWord<std::uint64_t> _lowPriorityBypassCount{0};

    std::queue<std::shared_ptr<TicketWaiter>> _normal;
    std::queue<std::shared_ptr<TicketWaiter>> _low;
};

/**
 * A PriorityTicketHolder supports queueing and prioritization of operations based on
 * AdmissionContext::Priority.
 *
 * MODIFICATIONS TO THIS CLASS MUST BE ACCOMPANIED BY AN UPDATE OF
 * src/mongo/tla_plus/PriorityTicketHolder/MCPriorityTicketHolder.tla TO ENSURE IT IS CORRECT
 */
class PriorityTicketHolder : public TicketHolder {
public:
    explicit PriorityTicketHolder(int32_t numTickets,
                                  int32_t lowPriorityBypassThreshold,
                                  ServiceContext* serviceContext);
    ~PriorityTicketHolder() override{};

    int32_t available() const override final;

    int64_t queued() const override final;

    int64_t numFinishedProcessing() const override final;

    std::int64_t expedited() const;

    std::int64_t bypassed() const;

    void updateLowPriorityAdmissionBypassThreshold(int32_t newBypassThreshold);

private:
    boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) override final;

    boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                    AdmissionContext* admCtx,
                                                    Date_t until) override final;

    void _releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept override final;

    void _resize(int32_t newSize, int32_t oldSize) noexcept override final;

    QueueStats& _getQueueStatsToUse(const AdmissionContext* admCtx) noexcept override final;

    void _appendImplStats(BSONObjBuilder& b) const override final;

    static QueueType _getQueueType(const AdmissionContext* admCtx) {
        auto priority = admCtx->getPriority();
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

    std::unique_ptr<TicketPool> _pool;

    ServiceContext* _serviceContext;
};
}  // namespace mongo
