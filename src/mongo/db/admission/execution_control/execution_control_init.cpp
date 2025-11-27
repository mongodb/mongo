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

#include "mongo/db/admission/execution_control/execution_control_init.h"

#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/execution_control/execution_control_parameters_gen.h"
#include "mongo/db/admission/execution_control/throughput_probing.h"
#include "mongo/db/admission/execution_control/ticketing_system.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/ticketholder.h"  // IWYU pragma: keep

#include <algorithm>
#include <array>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::admission::execution_control {
namespace {

std::unique_ptr<TicketingSystem> createTicketingSystem(
    ServiceContext* svcCtx, ExecutionControlConcurrencyAdjustmentAlgorithmEnum algorithm) {
    using enum ExecutionControlConcurrencyAdjustmentAlgorithmEnum;

    auto delinquentCb = [](AdmissionContext* admCtx, Milliseconds delta) {
        static_cast<ExecutionAdmissionContext*>(admCtx)->recordDelinquentAcquisition(delta);
    };

    auto acquisitionCb = [](AdmissionContext* admCtx) {
        static_cast<ExecutionAdmissionContext*>(admCtx)->recordExecutionAcquisition();
    };

    auto waitedAcquisitionCb = [](AdmissionContext* admCtx, Microseconds timeQueued) {
        static_cast<ExecutionAdmissionContext*>(admCtx)->recordExecutionWaitedAcquisition(
            timeQueued);
    };

    auto releaseCb = [](AdmissionContext* admCtx, Microseconds timeProcessed) {
        static_cast<ExecutionAdmissionContext*>(admCtx)->recordExecutionRelease(timeProcessed);
    };

    return std::make_unique<TicketingSystem>(
        svcCtx,
        TicketingSystem::RWTicketHolder{
            std::make_unique<TicketHolder>(svcCtx,
                                           gConcurrentReadTransactions.load(),
                                           true /* trackPeakUsed */,
                                           gReadMaxQueueDepth.load(),
                                           delinquentCb,
                                           acquisitionCb,
                                           waitedAcquisitionCb,
                                           releaseCb),
            std::make_unique<TicketHolder>(svcCtx,
                                           gConcurrentWriteTransactions.load(),
                                           true /* trackPeakUsed */,
                                           gWriteMaxQueueDepth.load(),
                                           delinquentCb,
                                           acquisitionCb,
                                           waitedAcquisitionCb,
                                           releaseCb)},
        TicketingSystem::RWTicketHolder{
            std::make_unique<TicketHolder>(
                svcCtx,
                TicketingSystem::resolveLowPriorityTickets(gConcurrentReadLowPriorityTransactions),
                false /* trackPeakUsed */,
                gReadLowPriorityMaxQueueDepth.load(),
                delinquentCb,
                acquisitionCb,
                waitedAcquisitionCb,
                releaseCb),
            std::make_unique<TicketHolder>(
                svcCtx,
                TicketingSystem::resolveLowPriorityTickets(gConcurrentWriteLowPriorityTransactions),
                false /* trackPeakUsed */,
                gWriteLowPriorityMaxQueueDepth.load(),
                delinquentCb,
                acquisitionCb,
                waitedAcquisitionCb,
                releaseCb)},
        algorithm);
}

}  // namespace

void initializeTicketingSystem(ServiceContext* svcCtx) {
    auto algorithm = ExecutionControlConcurrencyAdjustmentAlgorithm_parse(
        gConcurrencyAdjustmentAlgorithm,
        IDLParserContext{"executionControlConcurrencyAdjustmentAlgorithm"});

    if (algorithm == ExecutionControlConcurrencyAdjustmentAlgorithmEnum::kThroughputProbing &&
        (gConcurrentReadTransactions.load() !=
             TicketingSystem::kDefaultConcurrentTransactionsValue ||
         gConcurrentWriteTransactions.load() !=
             TicketingSystem::kDefaultConcurrentTransactionsValue)) {
        gConcurrencyAdjustmentAlgorithm = "fixedConcurrentTransactions";
        algorithm =
            ExecutionControlConcurrencyAdjustmentAlgorithmEnum::kFixedConcurrentTransactions;
        LOGV2_WARNING(11039601,
                      "When using the kThroughputProbing execution control algorithm, all "
                      "concurrent transactions server parameters must remain at their default "
                      "values. Non-default values will be ignored.");
    }

    TicketingSystem::use(svcCtx, createTicketingSystem(svcCtx, algorithm));

    auto* ticketingSystem = TicketingSystem::get(svcCtx);

    if (algorithm != ExecutionControlConcurrencyAdjustmentAlgorithmEnum::kThroughputProbing) {
        return;
    }

    // Throughput probing requires creation of an opCtx which in turn creates a locker that accesses
    // the `TicketingSystem` decoration. We defer starting probing until after the decoration has
    // been set to avoid data races.
    ticketingSystem->startThroughputProbe();
}

}  // namespace mongo::admission::execution_control
