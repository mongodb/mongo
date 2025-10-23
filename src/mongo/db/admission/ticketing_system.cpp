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

#include "mongo/db/admission/ticketing_system.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/admission/admission_feature_flags_gen.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/admission/execution_control_parameters_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/decorable.h"

#include <utility>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace admission {

namespace {
const auto ticketingSystemDecoration =
    mongo::ServiceContext::declareDecoration<std::unique_ptr<TicketingSystem>>();

template <typename Updater>
Status updateSettings(const std::string& op, Updater&& updater) {
    // Global mutex to serialize updates to any ticketing system settings via the server parameters.
    // This ensures that operations like changing the algorithm and changing the concurrent write
    // transactions value do not race with each other.
    static stdx::mutex mutex;
    stdx::lock_guard<stdx::mutex> lock(mutex);

    if (auto client = Client::getCurrent()) {
        auto ticketingSystem = TicketingSystem::get(client->getServiceContext());

        if (!ticketingSystem) {
            auto message =
                fmt::format("Attempting to modify {} on an instance without a storage engine", op);
            return {ErrorCodes::IllegalOperation, message};
        }

        return updater(client, ticketingSystem);
    }

    return Status::OK();
}

bool wasOperationDowngradedToLowPriority(OperationContext* opCtx,
                                         ExecutionAdmissionContext* admCtx) {
    const auto priority = admCtx->getPriority();

    // Check for all conditions that prevent a downgrade:
    //      1. The heuristic must be enabled via its server parameter.
    //      2. We don't deprioritize operations within a multi-document transaction.
    //      3. It is illegal to demote a high-priority (exempt) operation.
    //      4. The operation is already low-priority (no-op).
    if (!gStorageEngineHeuristicDeprioritizationEnabled.load() ||
        opCtx->inMultiDocumentTransaction() || priority == AdmissionContext::Priority::kExempt ||
        priority == AdmissionContext::Priority::kLow) {
        return false;
    }

    // If the op is eligible, downgrade it if it has yielded enough times to meet the threshold.
    return admCtx->getAdmissions() >= gStorageEngineHeuristicNumYieldsDeprioritizeThreshold.load();
}

}  // namespace

Status TicketingSystem::NormalPrioritySettings::updateWriteMaxQueueDepth(
    std::int32_t newWriteMaxQueueDepth) {
    return updateSettings("write max queue depth", [=](Client*, TicketingSystem* ticketingSystem) {
        ticketingSystem->setMaxQueueDepth(
            AdmissionContext::Priority::kNormal, OperationType::kWrite, newWriteMaxQueueDepth);
        return Status::OK();
    });
}

Status TicketingSystem::NormalPrioritySettings::updateReadMaxQueueDepth(
    std::int32_t newReadMaxQueueDepth) {
    return updateSettings("read max queue depth", [=](Client*, TicketingSystem* ticketingSystem) {
        ticketingSystem->setMaxQueueDepth(
            AdmissionContext::Priority::kNormal, OperationType::kRead, newReadMaxQueueDepth);
        return Status::OK();
    });
}

Status TicketingSystem::NormalPrioritySettings::updateConcurrentWriteTransactions(
    const int32_t& newWriteTransactions) {
    return updateSettings(
        "write transactions limit", [&](Client* client, TicketingSystem* ticketingSystem) {
            if (!ticketingSystem->isRuntimeResizable()) {
                return Status{ErrorCodes::IllegalOperation,
                              "Cannot modify concurrent write transactions limit when it is being "
                              "dynamically adjusted"};
            }
            ticketingSystem->setConcurrentTransactions(client->getOperationContext(),
                                                       AdmissionContext::Priority::kNormal,
                                                       OperationType::kWrite,
                                                       newWriteTransactions);
            return Status::OK();
        });
}

Status TicketingSystem::NormalPrioritySettings::updateConcurrentReadTransactions(
    const int32_t& newReadTransactions) {
    return updateSettings(
        "read transactions limit", [&](Client* client, TicketingSystem* ticketingSystem) {
            if (!ticketingSystem->isRuntimeResizable()) {
                return Status{ErrorCodes::IllegalOperation,
                              "Cannot modify concurrent read transactions limit when it is being "
                              "dynamically adjusted"};
            }
            ticketingSystem->setConcurrentTransactions(client->getOperationContext(),
                                                       AdmissionContext::Priority::kNormal,
                                                       OperationType::kRead,
                                                       newReadTransactions);
            return Status::OK();
        });
}

Status TicketingSystem::NormalPrioritySettings::validateConcurrentWriteTransactions(
    const int32_t& newWriteTransactions, const boost::optional<TenantId>) {
    if (!getTestCommandsEnabled() && newWriteTransactions < 5) {
        return Status(ErrorCodes::BadValue,
                      "Concurrent write transactions limit must be greater than or equal to 5.");
    }
    return Status::OK();
}

Status TicketingSystem::NormalPrioritySettings::validateConcurrentReadTransactions(
    const int32_t& newReadTransactions, const boost::optional<TenantId>) {
    if (!getTestCommandsEnabled() && newReadTransactions < 5) {
        return Status(ErrorCodes::BadValue,
                      "Concurrent read transactions limit must be greater than or equal to 5.");
    }
    return Status::OK();
}

Status TicketingSystem::LowPrioritySettings::updateWriteMaxQueueDepth(
    std::int32_t newWriteMaxQueueDepth) {
    return updateSettings("write max queue depth", [=](Client*, TicketingSystem* ticketingSystem) {
        if (!ticketingSystem->usesPrioritization()) {
            return Status{ErrorCodes::IllegalOperation,
                          "Cannot modify concurrent low priority settings if the storage "
                          "engine concurrency adjustment algorithm is using a single pool "
                          "without prioritization"};
        }
        ticketingSystem->setMaxQueueDepth(
            AdmissionContext::Priority::kLow, OperationType::kWrite, newWriteMaxQueueDepth);
        return Status::OK();
    });
}

Status TicketingSystem::LowPrioritySettings::updateReadMaxQueueDepth(
    std::int32_t newReadMaxQueueDepth) {
    return updateSettings("read max queue depth", [=](Client*, TicketingSystem* ticketingSystem) {
        if (!ticketingSystem->usesPrioritization()) {
            return Status{ErrorCodes::IllegalOperation,
                          "Cannot modify concurrent low priority settings if the storage "
                          "engine concurrency adjustment algorithm is using a single pool "
                          "without prioritization"};
        }
        ticketingSystem->setMaxQueueDepth(
            AdmissionContext::Priority::kLow, OperationType::kRead, newReadMaxQueueDepth);
        return Status::OK();
    });
}

Status TicketingSystem::LowPrioritySettings::updateConcurrentWriteTransactions(
    const int32_t& newWriteTransactions) {
    return updateSettings(
        "write transactions limit", [&](Client* client, TicketingSystem* ticketingSystem) {
            if (!ticketingSystem->isRuntimeResizable()) {
                return Status{ErrorCodes::IllegalOperation,
                              "Cannot modify concurrent write transactions limit when it is being "
                              "dynamically adjusted"};
            }
            if (!ticketingSystem->usesPrioritization()) {
                return Status{ErrorCodes::IllegalOperation,
                              "Cannot modify concurrent low priority settings if the storage "
                              "engine concurrency adjustment algorithm is using a single pool "
                              "without prioritization"};
            }
            ticketingSystem->setConcurrentTransactions(client->getOperationContext(),
                                                       AdmissionContext::Priority::kLow,
                                                       OperationType::kWrite,
                                                       newWriteTransactions);
            return Status::OK();
        });
}

Status TicketingSystem::LowPrioritySettings::updateConcurrentReadTransactions(
    const int32_t& newReadTransactions) {
    return updateSettings(
        "read transactions limit", [&](Client* client, TicketingSystem* ticketingSystem) {
            if (!ticketingSystem->isRuntimeResizable()) {
                return Status{ErrorCodes::IllegalOperation,
                              "Cannot modify concurrent read transactions limit when it is being "
                              "dynamically adjusted"};
            }
            if (!ticketingSystem->usesPrioritization()) {
                return Status{ErrorCodes::IllegalOperation,
                              "Cannot modify concurrent low priority settings if the storage "
                              "engine concurrency adjustment algorithm is using a single pool "
                              "without prioritization"};
            }
            ticketingSystem->setConcurrentTransactions(client->getOperationContext(),
                                                       AdmissionContext::Priority::kLow,
                                                       OperationType::kRead,
                                                       newReadTransactions);
            return Status::OK();
        });
}

Status TicketingSystem::updateConcurrencyAdjustmentAlgorithm(std::string algorithmName) {
    return updateSettings(
        "concurrency adjustment algorithm", [&](Client* client, TicketingSystem* ticketingSystem) {
            if (!gFeatureFlagMultipleTicketPoolsExecutionControl.isEnabled()) {
                return Status{ErrorCodes::IllegalOperation,
                              "Cannot modify concurrency adjustment algorithm in runtime"};
            }

            ticketingSystem->setConcurrencyAdjustmentAlgorithm(client->getOperationContext(),
                                                               algorithmName);

            return Status::OK();
        });
}

TicketingSystem* TicketingSystem::get(ServiceContext* svcCtx) {
    return ticketingSystemDecoration(svcCtx).get();
}

void TicketingSystem::use(ServiceContext* svcCtx,
                          std::unique_ptr<TicketingSystem> newTicketingSystem) {
    ticketingSystemDecoration(svcCtx) = std::move(newTicketingSystem);
}

TicketingSystem::TicketingSystem(
    ServiceContext* svcCtx,
    RWTicketHolder normal,
    RWTicketHolder low,
    Milliseconds throughputProbingInterval,
    StorageEngineConcurrencyAdjustmentAlgorithmEnum concurrencyAdjustmentAlgorithm)
    : _state({concurrencyAdjustmentAlgorithm}),
      _throughputProbing(svcCtx, normal.read.get(), normal.write.get(), throughputProbingInterval) {
    _holders[static_cast<size_t>(AdmissionContext::Priority::kNormal)] = std::move(normal);
    _holders[static_cast<size_t>(AdmissionContext::Priority::kLow)] = std::move(low);
};

bool TicketingSystem::isRuntimeResizable() const {
    return _state.loadRelaxed().isRuntimeResizable();
}

bool TicketingSystem::usesPrioritization() const {
    return _state.loadRelaxed().usesPrioritization();
}

void TicketingSystem::setMaxQueueDepth(AdmissionContext::Priority p,
                                       OperationType o,
                                       int32_t depth) {
    auto* holder = _getHolder(p, o);
    invariant(holder != nullptr);
    holder->setMaxQueueDepth(depth);
}

void TicketingSystem::setConcurrentTransactions(OperationContext* opCtx,
                                                AdmissionContext::Priority p,
                                                OperationType o,
                                                int32_t transactions) {
    auto* holder = _getHolder(p, o);
    invariant(holder != nullptr);
    holder->resize(opCtx, transactions, Date_t::max());
}

void TicketingSystem::setConcurrencyAdjustmentAlgorithm(OperationContext* opCtx,
                                                        std::string algorithmName) {
    tassert(11132200,
            "Feature flag MultipleTicketPoolsExecutionControl must be enabled to change the "
            "concurrency adjustment algorithm at runtime",
            gFeatureFlagMultipleTicketPoolsExecutionControl.isEnabled());

    const auto parsedAlgorithm = StorageEngineConcurrencyAdjustmentAlgorithm_parse(
        algorithmName, IDLParserContext{"storageEngineConcurrencyAdjustmentAlgorithm"});

    const TicketingState oldState = _state.loadRelaxed();
    const TicketingState newState = {parsedAlgorithm};
    _state.store(newState);

    const bool wasThroughputProbingRunning = oldState.usesThroughputProbing();
    const bool isThroughputProbingRunningNow = newState.usesThroughputProbing();

    if (wasThroughputProbingRunning == isThroughputProbingRunningNow) {
        // There has been a change in the algorithm related to the prioritization of operations, but
        // no change in the throughput probing state. There is nothing more to update.
        return;
    }

    if (isThroughputProbingRunningNow) {
        // Throughput probing needs to start. Apart from that, there is nothing more to update.
        _throughputProbing.start();
        return;
    }

    // There has been a change in the algorithm from throughput probing to fixed concurrency. We
    // should align the ticketholders size with the server parameter values.

    _throughputProbing.stop();

    setConcurrentTransactions(opCtx,
                              AdmissionContext::Priority::kNormal,
                              OperationType::kRead,
                              gConcurrentReadTransactions.load());
    setConcurrentTransactions(opCtx,
                              AdmissionContext::Priority::kNormal,
                              OperationType::kWrite,
                              gConcurrentWriteTransactions.load());
    setConcurrentTransactions(opCtx,
                              AdmissionContext::Priority::kLow,
                              OperationType::kRead,
                              gConcurrentReadLowPriorityTransactions.load());
    setConcurrentTransactions(opCtx,
                              AdmissionContext::Priority::kLow,
                              OperationType::kWrite,
                              gConcurrentWriteLowPriorityTransactions.load());
}

void TicketingSystem::appendStats(BSONObjBuilder& b) const {
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

    {
        BSONObjBuilder bbb(b.subobjStart("monitor"));
        _throughputProbing.appendStats(bbb);
        bbb.done();
    }
}

int32_t TicketingSystem::numOfTicketsUsed() const {
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

void TicketingSystem::incrementDelinquencyStats(OperationContext* opCtx) {
    auto& admCtx = ExecutionAdmissionContext::get(opCtx);

    auto priority = admCtx.getPriorityLowered() ? AdmissionContext::Priority::kLow
                                                : AdmissionContext::Priority::kNormal;
    {
        const auto& stats = admCtx.readDelinquencyStats();
        _getHolder(priority, OperationType::kRead)
            ->incrementDelinquencyStats(
                stats.delinquentAcquisitions.loadRelaxed(),
                Milliseconds(stats.totalAcquisitionDelinquencyMillis.loadRelaxed()),
                Milliseconds(stats.maxAcquisitionDelinquencyMillis.loadRelaxed()));
    }

    {
        const auto& stats = admCtx.writeDelinquencyStats();
        _getHolder(priority, OperationType::kWrite)
            ->incrementDelinquencyStats(
                stats.delinquentAcquisitions.loadRelaxed(),
                Milliseconds(stats.totalAcquisitionDelinquencyMillis.loadRelaxed()),
                Milliseconds(stats.maxAcquisitionDelinquencyMillis.loadRelaxed()));
    }
}

boost::optional<Ticket> TicketingSystem::waitForTicketUntil(OperationContext* opCtx,
                                                            OperationType o,
                                                            Date_t until) const {
    ExecutionAdmissionContext* admCtx = &ExecutionAdmissionContext::get(opCtx);

    boost::optional<ScopedAdmissionPriority<ExecutionAdmissionContext>> executionPriority;

    auto effectivePriority = admCtx->getPriority();
    if (usesPrioritization()) {
        if (wasOperationDowngradedToLowPriority(opCtx, admCtx)) {
            executionPriority.emplace(opCtx, AdmissionContext::Priority::kLow);
            effectivePriority = AdmissionContext::Priority::kLow;
            admCtx->priorityLowered();
        }
    } else {
        // Fall back to the normal ticket pool for low-priority when prioritization is disabled.
        if (effectivePriority == AdmissionContext::Priority::kLow) {
            effectivePriority = AdmissionContext::Priority::kNormal;
        }
    }

    auto* holder = _getHolder(effectivePriority, o);
    invariant(holder);

    if (opCtx->uninterruptibleLocksRequested_DO_NOT_USE()) {  // NOLINT
        return holder->waitForTicketUntilNoInterrupt_DO_NOT_USE(opCtx, admCtx, until);
    }

    return holder->waitForTicketUntil(opCtx, admCtx, until);
}

void TicketingSystem::startThroughputProbe() {
    tassert(11132202,
            "The throughput probing parameter should be enabled. This is only safe to use for "
            "initialization purposes.",
            _state.loadRelaxed().usesThroughputProbing());

    _throughputProbing.start();
}

TicketHolder* TicketingSystem::_getHolder(AdmissionContext::Priority p, OperationType o) const {
    if (p == AdmissionContext::Priority::kExempt) {
        // Redirect kExempt priority to the normal ticket pool as it bypasses acquisition.
        return _getHolder(AdmissionContext::Priority::kNormal, o);
    }

    const auto index = static_cast<size_t>(p);
    invariant(index < _holders.size());

    const auto& rwHolder = _holders[index];
    return (o == OperationType::kRead) ? rwHolder.read.get() : rwHolder.write.get();
}

bool TicketingSystem::TicketingState::usesPrioritization() const {
    return algorithm ==
        StorageEngineConcurrencyAdjustmentAlgorithmEnum::
            kFixedConcurrentTransactionsWithPrioritization;
}

bool TicketingSystem::TicketingState::usesThroughputProbing() const {
    return algorithm == StorageEngineConcurrencyAdjustmentAlgorithmEnum::kThroughputProbing;
}

bool TicketingSystem::TicketingState::isRuntimeResizable() const {
    return !usesThroughputProbing();
}

}  // namespace admission
}  // namespace mongo
