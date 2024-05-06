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

#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/notifyable.h"

namespace mongo {

class SemaphoreTicketHolder final : public TicketHolder {
public:
    enum class ResizePolicy { kGradual = 0, kImmediate };

    explicit SemaphoreTicketHolder(ServiceContext* serviceContext,
                                   int numTickets,
                                   bool trackPeakUsed,
                                   ResizePolicy resizePolicy = ResizePolicy::kGradual);

    int32_t available() const final;


    int64_t queued() const final {
        auto removed = _semaphoreStats.totalRemovedQueue.loadRelaxed();
        auto added = _semaphoreStats.totalAddedQueue.loadRelaxed();
        return std::max(added - removed, (int64_t)0);
    };

    int64_t numFinishedProcessing() const final;

private:
    boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                    AdmissionContext* admCtx,
                                                    Date_t until,
                                                    bool interruptible) final;

    boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) final;
    void _releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept final;

    bool _resizeImpl(WithLock lock,
                     OperationContext* opCtx,
                     int32_t newSize,
                     Date_t deadline) override;
    void _immediateResize(WithLock, int32_t newSize);

    void _appendImplStats(BSONObjBuilder& b) const final;

    QueueStats& _getQueueStatsToUse(AdmissionContext::Priority priority) noexcept final {
        return _semaphoreStats;
    }

    ResizePolicy _resizePolicy;
    QueueStats _semaphoreStats;
    Atomic<int64_t> _tickets;
    NotifyableParkingLot _parkingLot;
};

}  // namespace mongo
