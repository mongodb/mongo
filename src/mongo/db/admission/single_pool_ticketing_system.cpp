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

#include "mongo/db/admission/single_pool_ticketing_system.h"

#include "mongo/db/admission/execution_admission_context.h"

namespace mongo {
namespace admission {

bool SinglePoolTicketingSystem::isRuntimeResizable() const {
    return true;
}

bool SinglePoolTicketingSystem::usesPrioritization() const {
    return false;
}

void SinglePoolTicketingSystem::setMaxQueueDepth(AdmissionContext::Priority p,
                                                 Operation o,
                                                 int32_t depth) {
    auto* holder = o == Operation::kRead ? _readTicketHolder.get() : _writeTicketHolder.get();
    invariant(holder);
    holder->setMaxQueueDepth(depth);
}

void SinglePoolTicketingSystem::setConcurrentTransactions(OperationContext* opCtx,
                                                          AdmissionContext::Priority p,
                                                          Operation o,
                                                          int32_t transactions) {
    auto* holder = o == Operation::kRead ? _readTicketHolder.get() : _writeTicketHolder.get();
    invariant(holder);
    holder->resize(opCtx, transactions, Date_t::max());
}

void SinglePoolTicketingSystem::appendStats(BSONObjBuilder& b) const {
    invariant(_writeTicketHolder, "Writer TicketHolder is not present in the TicketingSystem");
    invariant(_readTicketHolder, "Reader TicketHolder is not present in the TicketingSystem");
    {
        BSONObjBuilder bb(b.subobjStart("write"));
        _writeTicketHolder->appendTicketStats(bb);
        {
            BSONObjBuilder bbb(bb.subobjStart(kNormalPriorityName));
            _writeTicketHolder->appendHolderStats(bbb);
            bbb.done();
        }
        {
            BSONObjBuilder bbb(bb.subobjStart(kExemptPriorityName));
            _writeTicketHolder->appendExemptStats(bbb);
            bbb.done();
        }
        bb.done();
    }
    {
        BSONObjBuilder bb(b.subobjStart("read"));
        _readTicketHolder->appendTicketStats(bb);
        {
            BSONObjBuilder bbb(bb.subobjStart(kNormalPriorityName));
            _readTicketHolder->appendHolderStats(bbb);
            bbb.done();
        }
        {
            BSONObjBuilder bbb(bb.subobjStart(kExemptPriorityName));
            _readTicketHolder->appendExemptStats(bbb);
            bbb.done();
        }
        bb.done();
    }
}

int32_t SinglePoolTicketingSystem::numOfTicketsUsed() const {
    return _readTicketHolder->used() + _writeTicketHolder->used();
}

void SinglePoolTicketingSystem::incrementDelinquencyStats(OperationContext* opCtx) {
    auto& admCtx = ExecutionAdmissionContext::get(opCtx);

    {
        const auto& stats = admCtx.readDelinquencyStats();
        _readTicketHolder->incrementDelinquencyStats(
            stats.delinquentAcquisitions.loadRelaxed(),
            Milliseconds(stats.totalAcquisitionDelinquencyMillis.loadRelaxed()),
            Milliseconds(stats.maxAcquisitionDelinquencyMillis.loadRelaxed()));
    }

    {
        const auto& stats = admCtx.writeDelinquencyStats();
        _writeTicketHolder->incrementDelinquencyStats(
            stats.delinquentAcquisitions.loadRelaxed(),
            Milliseconds(stats.totalAcquisitionDelinquencyMillis.loadRelaxed()),
            Milliseconds(stats.maxAcquisitionDelinquencyMillis.loadRelaxed()));
    }
}

boost::optional<Ticket> SinglePoolTicketingSystem::waitForTicketUntil(OperationContext* opCtx,
                                                                      Operation o,
                                                                      Date_t until) const {
    ExecutionAdmissionContext* admCtx = &ExecutionAdmissionContext::get(opCtx);

    auto* holder = o == Operation::kRead ? _readTicketHolder.get() : _writeTicketHolder.get();
    invariant(holder);

    if (opCtx->uninterruptibleLocksRequested_DO_NOT_USE()) {  // NOLINT
        return holder->waitForTicketUntilNoInterrupt_DO_NOT_USE(opCtx, admCtx, until);
    }

    return holder->waitForTicketUntil(opCtx, admCtx, until);
}

}  // namespace admission
}  // namespace mongo
