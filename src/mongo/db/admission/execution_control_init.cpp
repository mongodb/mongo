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

#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/admission/execution_control_parameters_gen.h"
#include "mongo/db/admission/throughput_probing.h"
#include "mongo/db/admission/ticketholder_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/ticketholder.h"  // IWYU pragma: keep

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace admission {

void initializeExecutionControl(ServiceContext* svcCtx) {
    auto const readMaxQueueDepth = gReadMaxQueueDepth.load();
    auto const writeMaxQueueDepth = gWriteMaxQueueDepth.load();
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
            gStorageEngineConcurrencyAdjustmentAlgorithm,
            IDLParserContext{"storageEngineConcurrencyAdjustmentAlgorithm"}) ==
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

    auto delinquentReadCb = [](AdmissionContext* admCtx, Milliseconds delta) {
        static_cast<ExecutionAdmissionContext*>(admCtx)->recordDelinquentReadAcquisition(delta);
    };

    auto delinquentWriteCb = [](AdmissionContext* admCtx, Milliseconds delta) {
        static_cast<ExecutionAdmissionContext*>(admCtx)->recordDelinquentWriteAcquisition(delta);
    };

    std::unique_ptr<TicketHolderManager> ticketHolderManager = makeTicketHolderManager(
        std::make_unique<TicketHolder>(
            svcCtx, readTransactions, usingThroughputProbing, readMaxQueueDepth, delinquentReadCb),
        std::make_unique<TicketHolder>(svcCtx,
                                       writeTransactions,
                                       usingThroughputProbing,
                                       writeMaxQueueDepth,
                                       delinquentWriteCb));

    TicketHolderManager::use(svcCtx, std::move(ticketHolderManager));

    if (usingThroughputProbing) {
        // Throughput probing requires creation of an opCtx which in turn creates a locker that
        // accesses the `TicketHolderManager` decoration. We defer starting probing until after the
        // decoration has been set to avoid data races.
        reinterpret_cast<ThroughputProbingTicketHolderManager*>(TicketHolderManager::get(svcCtx))
            ->startThroughputProbe();
    }
}

}  // namespace admission
}  // namespace mongo
