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
#include "mongo/db/admission/execution_control_parameters_gen.h"

namespace mongo {
namespace admission {

namespace {

bool isOperationDegradedAsLowPriority(OperationContext* opCtx, ExecutionAdmissionContext* admCtx) {
    if (!gStorageEngineHeuristicDeprioritizationEnabled.load()) {
        // If the heuristic is not enabled, we do not de-prioritize based on the number of yields.
        return false;
    }

    if (admCtx->getPriority() == AdmissionContext::Priority::kExempt) {
        // It is illegal to demote a high-priority (exempt) operation to a low-priority operation.
        return false;
    }

    if (admCtx->getPriority() == AdmissionContext::Priority::kLow) {
        // Fast exit for those operations that are manually marked as low-priority.
        return false;
    }

    if (admCtx->getAdmissions() >= gStorageEngineHeuristicNumYieldsDeprioritizeThreshold.load()) {
        return true;
    }

    return false;
}

}  // namespace

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
    boost::optional<BSONObjBuilder> readStats;
    boost::optional<BSONObjBuilder> writeStats;
    int32_t readOut = 0, readAvailable = 0, readTotalTickets = 0;
    int32_t writeOut = 0, writeAvailable = 0, writeTotalTickets = 0;

    for (size_t i = 0; i < _holders.size(); ++i) {
        const auto priority = static_cast<AdmissionContext::Priority>(i);

        if (priority == AdmissionContext::Priority::kExempt) {
            // Do not report statistics for kExempt as they are included in the normal priority pool
            continue;
        }

        const auto& rw = _holders[i];

        const auto& fieldName = priority == AdmissionContext::Priority::kNormal
            ? kNormalPriorityName
            : kLowPriorityName;
        if (rw.read) {
            readOut += rw.read->used();
            readAvailable += rw.read->available();
            readTotalTickets += rw.read->outof();
            if (!readStats.is_initialized()) {
                readStats.emplace();
            }
            BSONObjBuilder bb(readStats->subobjStart(fieldName));
            rw.read->appendTicketStats(bb);
            rw.read->appendHolderStats(bb);
            bb.done();
            if (priority == AdmissionContext::Priority::kNormal) {
                BSONObjBuilder bb(readStats->subobjStart(kExemptPriorityName));
                rw.read->appendExemptStats(readStats.value());
                bb.done();
            }
        }
        if (rw.write) {
            writeOut += rw.write->used();
            writeAvailable += rw.write->available();
            writeTotalTickets += rw.write->outof();
            if (!writeStats.is_initialized()) {
                writeStats.emplace();
            }
            BSONObjBuilder bb(writeStats->subobjStart(fieldName));
            rw.write->appendTicketStats(bb);
            rw.write->appendHolderStats(bb);
            bb.done();
            if (priority == AdmissionContext::Priority::kNormal) {
                BSONObjBuilder bb(writeStats->subobjStart(kExemptPriorityName));
                rw.write->appendExemptStats(writeStats.value());
                bb.done();
            }
        }
    }
    if (readStats.is_initialized()) {
        readStats->append("out", readOut);
        readStats->append("available", readAvailable);
        readStats->append("totalTickets", readTotalTickets);
        readStats->done();
        b.append("read", readStats->obj());
    }
    if (writeStats.is_initialized()) {
        writeStats->appendNumber("out", writeOut);
        writeStats->appendNumber("available", writeAvailable);
        writeStats->appendNumber("totalTickets", writeTotalTickets);
        writeStats->done();
        b.append("write", writeStats->obj());
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

    boost::optional<ScopedAdmissionPriority<ExecutionAdmissionContext>> executionPriority;
    if (isOperationDegradedAsLowPriority(opCtx, admCtx)) {
        executionPriority.emplace(opCtx, AdmissionContext::Priority::kLow);
    }

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
