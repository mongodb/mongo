/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/admission/multiple_pools_ticketing_system.h"

#include "mongo/db/admission/execution_admission_context.h"

namespace mongo {
namespace admission {

bool MultiplePoolsTicketingSystem::isRuntimeResizable() const {
    return true;
}

bool MultiplePoolsTicketingSystem::usesPrioritization() const {
    return true;
}

void MultiplePoolsTicketingSystem::setMaxQueueDepth(AdmissionContext::Priority p,
                                                    Operation o,
                                                    int32_t depth) {
    auto* holder = _getHolder(p, o);
    invariant(holder != nullptr);
    holder->setMaxQueueDepth(depth);
}

void MultiplePoolsTicketingSystem::setConcurrentTransactions(OperationContext* opCtx,
                                                             AdmissionContext::Priority p,
                                                             Operation o,
                                                             int32_t transactions) {
    auto* holder = _getHolder(p, o);
    invariant(holder != nullptr);
    holder->resize(opCtx, transactions, Date_t::max());
}

void MultiplePoolsTicketingSystem::appendStats(BSONObjBuilder& b) const {
    for (size_t i = 0; i < _holders.size(); ++i) {
        const auto priority = static_cast<AdmissionContext::Priority>(i);

        if (priority == AdmissionContext::Priority::kExempt) {
            // Do not report statistics for kExempt as they are included in the normal priority pool
            continue;
        }

        const auto& rw = _holders[i];

        const char* priorityName =
            (priority == AdmissionContext::Priority::kNormal) ? "normal" : "low";

        BSONObjBuilder sub(b.subobjStart(priorityName));
        if (rw.read) {
            BSONObjBuilder readStats(sub.subobjStart("read"));
            rw.read->appendStats(readStats);
            readStats.done();
        }
        if (rw.write) {
            BSONObjBuilder writeStats(sub.subobjStart("write"));
            rw.write->appendStats(writeStats);
            writeStats.done();
        }
        sub.done();
    }
}

int32_t MultiplePoolsTicketingSystem::numOfTicketsUsed() const {
    int32_t total = 0;
    for (const auto& rw : _holders) {
        if (rw.read) {
            total += rw.read->used();
        }
        if (rw.write) {
            total += rw.write->used();
        }
    }
    return total;
}

void MultiplePoolsTicketingSystem::incrementDelinquencyStats(OperationContext* opCtx) {
    auto& admCtx = ExecutionAdmissionContext::get(opCtx);

    {
        const auto& stats = admCtx.readDelinquencyStats();
        _getHolder(AdmissionContext::Priority::kNormal, Operation::kRead)
            ->incrementDelinquencyStats(
                stats.delinquentAcquisitions.loadRelaxed(),
                Milliseconds(stats.totalAcquisitionDelinquencyMillis.loadRelaxed()),
                Milliseconds(stats.maxAcquisitionDelinquencyMillis.loadRelaxed()));
    }

    {
        const auto& stats = admCtx.writeDelinquencyStats();
        _getHolder(AdmissionContext::Priority::kNormal, Operation::kWrite)
            ->incrementDelinquencyStats(
                stats.delinquentAcquisitions.loadRelaxed(),
                Milliseconds(stats.totalAcquisitionDelinquencyMillis.loadRelaxed()),
                Milliseconds(stats.maxAcquisitionDelinquencyMillis.loadRelaxed()));
    }
}

boost::optional<Ticket> MultiplePoolsTicketingSystem::waitForTicketUntil(OperationContext* opCtx,
                                                                         Operation o,
                                                                         Date_t until) const {
    ExecutionAdmissionContext* admCtx = &ExecutionAdmissionContext::get(opCtx);

    auto* holder = _getHolder(admCtx->getPriority(), o);
    invariant(holder);

    if (opCtx->uninterruptibleLocksRequested_DO_NOT_USE()) {  // NOLINT
        return holder->waitForTicketUntilNoInterrupt_DO_NOT_USE(opCtx, admCtx, until);
    }

    return holder->waitForTicketUntil(opCtx, admCtx, until);
}

TicketHolder* MultiplePoolsTicketingSystem::_getHolder(AdmissionContext::Priority p,
                                                       Operation o) const {
    if (p == AdmissionContext::Priority::kExempt) {
        // Redirect kExempt priority to the normal ticket pool as it bypasses acquisition.
        return _getHolder(AdmissionContext::Priority::kNormal, o);
    }

    const auto index = static_cast<size_t>(p);
    if (index >= _holders.size()) {
        return nullptr;
    }

    const auto& rwHolder = _holders[index];
    return (o == Operation::kRead) ? rwHolder.read.get() : rwHolder.write.get();
}

}  // namespace admission
}  // namespace mongo
