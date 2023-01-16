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

#if defined(__linux__)
#include <semaphore.h>
#endif

#include <queue>

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/time_support.h"

namespace mongo {

class SemaphoreTicketHolder final : public TicketHolderWithQueueingStats {
public:
    explicit SemaphoreTicketHolder(int numTickets, ServiceContext* serviceContext);
    ~SemaphoreTicketHolder() override final;

    int available() const override final;

    int queued() const override final {
        auto removed = _semaphoreStats.totalRemovedQueue.loadRelaxed();
        auto added = _semaphoreStats.totalAddedQueue.loadRelaxed();
        return std::max(static_cast<int>(added - removed), 0);
    };

private:
    boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                    AdmissionContext* admCtx,
                                                    Date_t until,
                                                    WaitMode waitMode) override final;

    boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) override final;
    void _releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept override final;

    void _appendImplStats(BSONObjBuilder& b) const override final;

    void _resize(OperationContext* opCtx, int newSize, int oldSize) noexcept override final;

    QueueStats& _getQueueStatsToUse(const AdmissionContext* admCtx) noexcept override final {
        return _semaphoreStats;
    }
#if defined(__linux__)
    mutable sem_t _sem;

#else
    bool _tryAcquire();

    int _numTickets;
    Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "SemaphoreTicketHolder::_mutex");
    stdx::condition_variable _newTicket;
#endif
    QueueStats _semaphoreStats;
};

}  // namespace mongo
