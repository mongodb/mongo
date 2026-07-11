// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/execution_control/execution_control_init.h"

#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/execution_control/execution_control_parameters_gen.h"
#include "mongo/db/admission/execution_control/ticketing_system.h"
#include "mongo/db/admission/ticketing/ticketholder.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"

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

    // The acquisition and waited-acquisition callbacks need to know which ticket queue (normal- or
    // low-priority) actually served the operation. We bind that identity here, where each holder is
    // constructed, rather than reading it from the AdmissionContext: when prioritization is
    // disabled a low-priority operation runs in the normal pool, so its live priority would not
    // reflect the queue it actually used.
    auto makeAcquisitionCb = [](ExecutionAdmissionContext::QueueType queue) {
        return [queue](AdmissionContext* admCtx, AdmissionContext::Priority priority) {
            static_cast<ExecutionAdmissionContext*>(admCtx)->recordExecutionAcquisition(priority,
                                                                                        queue);
        };
    };

    auto makeWaitedAcquisitionCb = [](ExecutionAdmissionContext::QueueType queue) {
        return [queue](AdmissionContext* admCtx, Microseconds timeQueued) {
            static_cast<ExecutionAdmissionContext*>(admCtx)->recordExecutionWaitedAcquisition(
                timeQueued, queue);
        };
    };

    auto releaseCb = [](AdmissionContext* admCtx, Microseconds timeProcessed) {
        static_cast<ExecutionAdmissionContext*>(admCtx)->recordExecutionRelease(timeProcessed);
    };

    auto startQueueingCb = [](AdmissionContext* admCtx) {
        static_cast<ExecutionAdmissionContext*>(admCtx)->recordExecutionStartQueueing();
    };

    return std::make_unique<TicketingSystem>(
        svcCtx,
        TicketingSystem::RWTicketHolder{
            std::make_unique<TicketHolder>(
                svcCtx,
                gConcurrentReadTransactions.load(),
                true /* trackPeakUsed */,
                gReadMaxQueueDepth.load(),
                delinquentCb,
                makeAcquisitionCb(ExecutionAdmissionContext::QueueType::kNormal),
                makeWaitedAcquisitionCb(ExecutionAdmissionContext::QueueType::kNormal),
                releaseCb,
                startQueueingCb),
            std::make_unique<TicketHolder>(
                svcCtx,
                gConcurrentWriteTransactions.load(),
                true /* trackPeakUsed */,
                gWriteMaxQueueDepth.load(),
                delinquentCb,
                makeAcquisitionCb(ExecutionAdmissionContext::QueueType::kNormal),
                makeWaitedAcquisitionCb(ExecutionAdmissionContext::QueueType::kNormal),
                releaseCb,
                startQueueingCb)},
        TicketingSystem::RWTicketHolder{
            std::make_unique<TicketHolder>(
                svcCtx,
                TicketingSystem::resolveLowPriorityTickets(gConcurrentReadLowPriorityTransactions),
                false /* trackPeakUsed */,
                gReadLowPriorityMaxQueueDepth.load(),
                delinquentCb,
                makeAcquisitionCb(ExecutionAdmissionContext::QueueType::kLow),
                makeWaitedAcquisitionCb(ExecutionAdmissionContext::QueueType::kLow),
                releaseCb,
                startQueueingCb,
                TicketHolder::ResizePolicy::kGradual,
                TicketHolder::SemaphoreType::kPrioritizeFewestAdmissions),
            std::make_unique<TicketHolder>(
                svcCtx,
                TicketingSystem::resolveLowPriorityTickets(gConcurrentWriteLowPriorityTransactions),
                false /* trackPeakUsed */,
                gWriteLowPriorityMaxQueueDepth.load(),
                delinquentCb,
                makeAcquisitionCb(ExecutionAdmissionContext::QueueType::kLow),
                makeWaitedAcquisitionCb(ExecutionAdmissionContext::QueueType::kLow),
                releaseCb,
                startQueueingCb,
                TicketHolder::ResizePolicy::kGradual,
                TicketHolder::SemaphoreType::kPrioritizeFewestAdmissions)},
        algorithm);
}

}  // namespace

void initializeTicketingSystem(ServiceContext* svcCtx) {
    auto algorithm = idl::deserialize<ExecutionControlConcurrencyAdjustmentAlgorithmEnum>(
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
