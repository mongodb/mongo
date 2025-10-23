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

#include "mongo/db/admission/execution_control_init.h"

#include "mongo/db/admission/admission_feature_flags_gen.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/admission/execution_control_parameters_gen.h"
#include "mongo/db/admission/throughput_probing.h"
#include "mongo/db/admission/ticketing_system.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/ticketholder.h"  // IWYU pragma: keep

#include <algorithm>
#include <array>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace admission {
namespace {

bool hasNonDefaultTransactionConcurrencySettings() {
    const std::array actual{gConcurrentReadTransactions.load(),
                            gConcurrentWriteTransactions.load(),
                            gConcurrentReadLowPriorityTransactions.load(),
                            gConcurrentWriteLowPriorityTransactions.load()};

    const std::array defaults{TicketingSystem::kDefaultConcurrentTransactions,
                              TicketingSystem::kDefaultConcurrentTransactions,
                              TicketingSystem::kDefaultLowPriorityConcurrentTransactions,
                              TicketingSystem::kDefaultLowPriorityConcurrentTransactions};

    auto [actIt, defIt] = std::mismatch(actual.begin(), actual.end(), defaults.begin());
    return actIt != actual.end();
}

std::unique_ptr<TicketingSystem> createTicketingSystem(
    ServiceContext* svcCtx, StorageEngineConcurrencyAdjustmentAlgorithmEnum algorithm) {
    using enum StorageEngineConcurrencyAdjustmentAlgorithmEnum;

    auto delinquentReadCb = [](AdmissionContext* admCtx, Milliseconds delta) {
        static_cast<ExecutionAdmissionContext*>(admCtx)->recordDelinquentReadAcquisition(delta);
    };

    auto delinquentWriteCb = [](AdmissionContext* admCtx, Milliseconds delta) {
        static_cast<ExecutionAdmissionContext*>(admCtx)->recordDelinquentWriteAcquisition(delta);
    };

    return std::make_unique<TicketingSystem>(
        svcCtx,
        TicketingSystem::RWTicketHolder{
            std::make_unique<TicketHolder>(svcCtx,
                                           gConcurrentReadTransactions.load(),
                                           true /* trackPeakUsed */,
                                           gReadMaxQueueDepth.load(),
                                           delinquentReadCb),
            std::make_unique<TicketHolder>(svcCtx,
                                           gConcurrentWriteTransactions.load(),
                                           true /* trackPeakUsed */,
                                           gWriteMaxQueueDepth.load(),
                                           delinquentWriteCb)},
        TicketingSystem::RWTicketHolder{
            std::make_unique<TicketHolder>(svcCtx,
                                           gConcurrentReadLowPriorityTransactions.load(),
                                           false /* trackPeakUsed */,
                                           gReadLowPriorityMaxQueueDepth.load(),
                                           delinquentReadCb),
            std::make_unique<TicketHolder>(svcCtx,
                                           gConcurrentWriteLowPriorityTransactions.load(),
                                           false /* trackPeakUsed */,
                                           gWriteLowPriorityMaxQueueDepth.load(),
                                           delinquentWriteCb)},
        Milliseconds{gStorageEngineConcurrencyAdjustmentIntervalMillis},
        algorithm);
}

}  // namespace

void initializeExecutionControl(ServiceContext* svcCtx) {
    auto algorithm = StorageEngineConcurrencyAdjustmentAlgorithm_parse(
        gStorageEngineConcurrencyAdjustmentAlgorithm,
        IDLParserContext{"storageEngineConcurrencyAdjustmentAlgorithm"});

    if (algorithm == StorageEngineConcurrencyAdjustmentAlgorithmEnum::kThroughputProbing &&
        (gConcurrentReadTransactions.load() != TicketingSystem::kDefaultConcurrentTransactions ||
         gConcurrentWriteTransactions.load() != TicketingSystem::kDefaultConcurrentTransactions)) {
        gStorageEngineConcurrencyAdjustmentAlgorithm = "fixedConcurrentTransactions";
        algorithm = StorageEngineConcurrencyAdjustmentAlgorithmEnum::kFixedConcurrentTransactions;
    }

    if (algorithm == StorageEngineConcurrencyAdjustmentAlgorithmEnum::kThroughputProbing &&
        hasNonDefaultTransactionConcurrencySettings()) {
        LOGV2_WARNING(11039601,
                      "When using the kThroughputProbing storage engine algorithm, all concurrent "
                      "transactions server parameters must remain at their default values. "
                      "Non-default values will be ignored.");
    }

    if (algorithm ==
        StorageEngineConcurrencyAdjustmentAlgorithmEnum::
            kFixedConcurrentTransactionsWithPrioritization) {
        tassert(11039600,
                "Expected to have the feature flag enabled to use "
                "kFixedConcurrentTransactionsWithPrioritization algorithm",
                gFeatureFlagMultipleTicketPoolsExecutionControl.isEnabled());
    }

    TicketingSystem::use(svcCtx, createTicketingSystem(svcCtx, algorithm));

    auto* ticketingSystem = TicketingSystem::get(svcCtx);

    if (algorithm != StorageEngineConcurrencyAdjustmentAlgorithmEnum::kThroughputProbing) {
        return;
    }

    // Throughput probing requires creation of an opCtx which in turn creates a locker that accesses
    // the `TicketingSystem` decoration. We defer starting probing until after the decoration has
    // been set to avoid data races.
    ticketingSystem->startThroughputProbe();
}

}  // namespace admission
}  // namespace mongo
