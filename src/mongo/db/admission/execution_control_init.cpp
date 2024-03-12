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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/db/admission/execution_control_init.h"

#include <string>

#include "mongo/db/admission/execution_control_feature_flags_gen.h"
#include "mongo/db/admission/execution_control_parameters_gen.h"
#include "mongo/db/admission/throughput_probing.h"
#include "mongo/db/admission/ticketholder_manager.h"
#include "mongo/db/feature_flag.h"
#include "mongo/util/concurrency/priority_ticketholder.h"
#include "mongo/util/concurrency/semaphore_ticketholder.h"  // IWYU pragma: keep

namespace mongo {
namespace admission {

void initializeExecutionControl(ServiceContext* svcCtx) {
    auto readTransactions = gConcurrentReadTransactions.load();
    auto writeTransactions = gConcurrentWriteTransactions.load();
    static constexpr auto DEFAULT_TICKETS_VALUE = 128;
    bool userSetConcurrency = readTransactions != 0 || writeTransactions != 0;

    readTransactions = readTransactions == 0 ? DEFAULT_TICKETS_VALUE : readTransactions;
    writeTransactions = writeTransactions == 0 ? DEFAULT_TICKETS_VALUE : writeTransactions;

    if (userSetConcurrency) {
        gStorageEngineConcurrencyAdjustmentAlgorithm = "fixedConcurrentTransactions";
    }

    auto usingThroughputProbing =
        StorageEngineConcurrencyAdjustmentAlgorithm_parse(
            IDLParserContext{"storageEngineConcurrencyAdjustmentAlgorithm"},
            gStorageEngineConcurrencyAdjustmentAlgorithm) ==
        StorageEngineConcurrencyAdjustmentAlgorithmEnum::kThroughputProbing;

    // If the user manually set concurrency limits, then disable execution control implicitly.
    auto makeTicketHolderManager =
        [&](auto readTicketHolder, auto writeTicketHolder) -> std::unique_ptr<TicketHolderManager> {
        if (usingThroughputProbing) {
            return std::make_unique<ThroughputProbingTicketHolderManager>(
                svcCtx,
                std::move(readTicketHolder),
                std::move(writeTicketHolder),
                Milliseconds{gStorageEngineConcurrencyAdjustmentIntervalMillis});
        } else {
            return std::make_unique<FixedTicketHolderManager>(std::move(readTicketHolder),
                                                              std::move(writeTicketHolder));
        }
    };

    if (feature_flags::gFeatureFlagDeprioritizeLowPriorityOperations.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        std::unique_ptr<TicketHolderManager> ticketHolderManager;
#ifdef __linux__
        LOGV2_DEBUG(6902900, 1, "Using Priority Queue-based ticketing scheduler");

        auto lowPriorityBypassThreshold = gLowPriorityAdmissionBypassThreshold.load();
        ticketHolderManager = makeTicketHolderManager(
            std::make_unique<PriorityTicketHolder>(
                svcCtx, readTransactions, lowPriorityBypassThreshold, usingThroughputProbing),
            std::make_unique<PriorityTicketHolder>(
                svcCtx, writeTransactions, lowPriorityBypassThreshold, usingThroughputProbing));
#else
        LOGV2_DEBUG(7207201, 1, "Using semaphore-based ticketing scheduler");
        // PriorityTicketHolder is implemented using an equivalent mechanism to
        // std::atomic::wait which isn't available until C++20. We've implemented it in Linux
        // using futex calls. As this hasn't been implemented in non-Linux platforms we fallback
        // to the existing semaphore implementation even if the feature flag is enabled.
        //
        // TODO SERVER-72616: Remove the ifdefs once TicketPool is implemented with atomic
        // wait.
        ticketHolderManager =
            makeTicketHolderManager(std::make_unique<SemaphoreTicketHolder>(
                                        svcCtx, readTransactions, usingThroughputProbing),
                                    std::make_unique<SemaphoreTicketHolder>(
                                        svcCtx, writeTransactions, usingThroughputProbing));
#endif
        TicketHolderManager::use(svcCtx, std::move(ticketHolderManager));
    } else {
        auto ticketHolderManager =
            makeTicketHolderManager(std::make_unique<SemaphoreTicketHolder>(
                                        svcCtx, readTransactions, usingThroughputProbing),
                                    std::make_unique<SemaphoreTicketHolder>(
                                        svcCtx, writeTransactions, usingThroughputProbing));
        TicketHolderManager::use(svcCtx, std::move(ticketHolderManager));
    }
}
}  // namespace admission
}  // namespace mongo
