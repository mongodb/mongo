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

#include "mongo/db/admission/execution_control/ticketing_system.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/execution_control/execution_control_heuristic_parameters_gen.h"
#include "mongo/db/admission/execution_control/execution_control_parameters_gen.h"
#include "mongo/db/admission/execution_control/throughput_probing_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/decorable.h"
#include "mongo/util/processinfo.h"

#include <utility>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::admission::execution_control {

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
                fmt::format("Attempting to modify {} on an instance without execution control", op);
            return {ErrorCodes::IllegalOperation, message};
        }

        return updater(client, ticketingSystem);
    }

    return Status::OK();
}

void warnIfDynamicAdjustmentEnabled(TicketingSystem* ticketingSystem,
                                    const std::string& serverParameter) {
    if (!ticketingSystem->isRuntimeResizable()) {
        LOGV2_WARNING(11280900,
                      "Updating {serverParameter} while a dynamic adjustment algorithm (e.g., "
                      "throughputProbing) is active. The new value will be stored but may not take "
                      "effect until the algorithm is changed.",
                      "serverParameter"_attr = serverParameter);
    }
}

void warnIfPrioritizationDisabled(TicketingSystem* ticketingSystem,
                                  const std::string& serverParameter) {
    if (!ticketingSystem->usesPrioritization()) {
        LOGV2_WARNING(11280901,
                      "Updating {serverParameter} while the execution control concurrency "
                      "adjustment algorithm is using a single pool without prioritization. The new "
                      "value will be stored but will not take effect.",
                      "serverParameter"_attr = serverParameter);
    }
}

bool wasOperationDowngradedToLowPriority(OperationContext* opCtx,
                                         ExecutionAdmissionContext* admCtx) {
    const auto priority = admCtx->getPriority();

    // Check for all conditions that prevent a downgrade:
    //      1. The heuristic must be enabled via its server parameter.
    //      2. We don't deprioritize operations within a multi-document transaction.
    //      3. It is illegal to demote a high-priority (exempt) operation.
    //      4. The operation is already low-priority (no-op).
    if (!gHeuristicDeprioritization.load() || opCtx->inMultiDocumentTransaction() ||
        priority == AdmissionContext::Priority::kExempt ||
        priority == AdmissionContext::Priority::kLow) {
        return false;
    }

    return ExecutionAdmissionContext::shouldDeprioritize(admCtx->getAdmissions());
}

Status checkPrioritizationTransition(bool oldStateUsesPrioritization,
                                     bool newStateUsesPrioritization) {
    const bool transitioningAwayFromPrioritization =
        oldStateUsesPrioritization && !newStateUsesPrioritization;

    if (transitioningAwayFromPrioritization &&
        (gConcurrentReadLowPriorityTransactions.load() == 0 ||
         gConcurrentWriteLowPriorityTransactions.load() == 0)) {
        return Status{ErrorCodes::IllegalOperation,
                      "Cannot transition to not use prioritization when low-priority read or write "
                      "tickets are set to 0"};
    }
    return Status::OK();
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
    const auto spName = "concurrent write transactions limit";
    return updateSettings(spName, [&](Client* client, TicketingSystem* ticketingSystem) {
        warnIfDynamicAdjustmentEnabled(ticketingSystem, spName);

        ticketingSystem->setConcurrentTransactions(client->getOperationContext(),
                                                   AdmissionContext::Priority::kNormal,
                                                   OperationType::kWrite,
                                                   newWriteTransactions);
        return Status::OK();
    });
}

Status TicketingSystem::NormalPrioritySettings::updateConcurrentReadTransactions(
    const int32_t& newReadTransactions) {
    const auto spName = "concurrent read transactions limit";
    return updateSettings(spName, [&](Client* client, TicketingSystem* ticketingSystem) {
        warnIfDynamicAdjustmentEnabled(ticketingSystem, spName);

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

Status TicketingSystem::validateConcurrencyAdjustmentAlgorithm(
    const std::string& name, const boost::optional<TenantId>&) try {
    ExecutionControlConcurrencyAdjustmentAlgorithm_parse(
        name, IDLParserContext{"executionControlConcurrencyAdjustmentAlgorithm"});
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

Status TicketingSystem::LowPrioritySettings::updateWriteMaxQueueDepth(
    std::int32_t newWriteMaxQueueDepth) {
    const auto spName = "low priority write max queue depth";
    return updateSettings(spName, [=](Client*, TicketingSystem* ticketingSystem) {
        warnIfPrioritizationDisabled(ticketingSystem, spName);

        ticketingSystem->setMaxQueueDepth(
            AdmissionContext::Priority::kLow, OperationType::kWrite, newWriteMaxQueueDepth);
        return Status::OK();
    });
}

Status TicketingSystem::LowPrioritySettings::updateReadMaxQueueDepth(
    std::int32_t newReadMaxQueueDepth) {
    const auto spName = "low priority read max queue depth";
    return updateSettings(spName, [=](Client*, TicketingSystem* ticketingSystem) {
        warnIfPrioritizationDisabled(ticketingSystem, spName);

        ticketingSystem->setMaxQueueDepth(
            AdmissionContext::Priority::kLow, OperationType::kRead, newReadMaxQueueDepth);
        return Status::OK();
    });
}

Status TicketingSystem::LowPrioritySettings::updateConcurrentWriteTransactions(
    const int32_t& newWriteTransactions) {
    const auto spName = "low priority concurrent write transactions limit";
    return updateSettings(spName, [&](Client* client, TicketingSystem* ticketingSystem) {
        warnIfPrioritizationDisabled(ticketingSystem, spName);

        ticketingSystem->setConcurrentTransactions(client->getOperationContext(),
                                                   AdmissionContext::Priority::kLow,
                                                   OperationType::kWrite,
                                                   newWriteTransactions);
        return Status::OK();
    });
}

Status TicketingSystem::LowPrioritySettings::updateConcurrentReadTransactions(
    const int32_t& newReadTransactions) {
    const auto spName = "low priority concurrent read transactions limit";
    return updateSettings(spName, [&](Client* client, TicketingSystem* ticketingSystem) {
        warnIfPrioritizationDisabled(ticketingSystem, spName);

        ticketingSystem->setConcurrentTransactions(client->getOperationContext(),
                                                   AdmissionContext::Priority::kLow,
                                                   OperationType::kRead,
                                                   newReadTransactions);
        return Status::OK();
    });
}

Status TicketingSystem::LowPrioritySettings::validateConcurrentWriteTransactions(
    const int32_t& newWriteTransactions, const boost::optional<TenantId>) {
    if (newWriteTransactions < 0 &&
        newWriteTransactions != TicketingSystem::kUnsetLowPriorityConcurrentTransactionsValue) {
        return Status(ErrorCodes::BadValue,
                      "Invalid value for 'executionControlConcurrentWriteLowPriorityTransactions': "
                      "must be >= 0, or -1 to use the number of logical CPU cores.");
    }
    return Status::OK();
}

Status TicketingSystem::LowPrioritySettings::validateConcurrentReadTransactions(
    const int32_t& newReadTransactions, const boost::optional<TenantId>) {
    if (newReadTransactions < 0 &&
        newReadTransactions != TicketingSystem::kUnsetLowPriorityConcurrentTransactionsValue) {
        return Status(ErrorCodes::BadValue,
                      "Invalid value for 'executionControlConcurrentReadLowPriorityTransactions': "
                      "must be >= 0, or -1 to use the number of logical CPU cores.");
    }
    return Status::OK();
}

Status TicketingSystem::updateConcurrencyAdjustmentAlgorithm(std::string algorithmName) {
    return updateSettings("concurrency adjustment algorithm",
                          [&](Client* client, TicketingSystem* ticketingSystem) {
                              ticketingSystem->setConcurrencyAdjustmentAlgorithm(
                                  client->getOperationContext(), algorithmName);
                              return Status::OK();
                          });
}

Status TicketingSystem::updateDeprioritizationGate(bool enabled) {
    return updateSettings("deprioritization gate",
                          [&](Client* client, TicketingSystem* ticketingSystem) {
                              return ticketingSystem->setDeprioritizationGate(enabled);
                          });
}

Status TicketingSystem::updateHeuristicDeprioritization(bool enabled) {
    return updateSettings("heuristic deprioritization",
                          [&](Client* client, TicketingSystem* ticketingSystem) {
                              return ticketingSystem->setHeuristicDeprioritization(enabled);
                          });
}

Status TicketingSystem::updateBackgroundTasksDeprioritization(bool enabled) {
    return updateSettings("background tasks deprioritization",
                          [&](Client* client, TicketingSystem* ticketingSystem) {
                              return ticketingSystem->setBackgroundTasksDeprioritization(enabled);
                          });
}

int TicketingSystem::resolveLowPriorityTickets(const AtomicWord<int32_t>& serverParam) {
    const int loadedValue = serverParam.load();

    return loadedValue == kUnsetLowPriorityConcurrentTransactionsValue
        ? static_cast<int32_t>(ProcessInfo::getNumLogicalCores())
        : loadedValue;
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
    ExecutionControlConcurrencyAdjustmentAlgorithmEnum concurrencyAdjustmentAlgorithm)
    : _state({.usesThroughputProbing = concurrencyAdjustmentAlgorithm ==
                  ExecutionControlConcurrencyAdjustmentAlgorithmEnum::kThroughputProbing,

              .deprioritization = {.gate = gDeprioritizationGate.load(),
                                   .heuristic = gHeuristicDeprioritization.load(),
                                   .backgroundTasks = gBackgroundTasksDeprioritization.load()}}),
      _throughputProbing(svcCtx,
                         normal.read.get(),
                         normal.write.get(),
                         Milliseconds{throughput_probing::gConcurrencyAdjustmentIntervalMillis}) {
    _holders[static_cast<size_t>(AdmissionContext::Priority::kNormal)] = std::move(normal);
    _holders[static_cast<size_t>(AdmissionContext::Priority::kLow)] = std::move(low);
};

bool TicketingSystem::isRuntimeResizable() const {
    return !_state.loadRelaxed().usesThroughputProbing;
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
    const auto parsedAlgorithm = ExecutionControlConcurrencyAdjustmentAlgorithm_parse(
        algorithmName, IDLParserContext{"executionControlConcurrencyAdjustmentAlgorithm"});

    const TicketingState oldState = _state.loadRelaxed();

    TicketingState newState = oldState;
    newState.usesThroughputProbing =
        (parsedAlgorithm == ExecutionControlConcurrencyAdjustmentAlgorithmEnum::kThroughputProbing);

    _state.store(newState);

    if (oldState.usesThroughputProbing == newState.usesThroughputProbing) {
        // No-op. Nothing has changed.
        return;
    }

    if (newState.usesThroughputProbing) {
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
}

Status TicketingSystem::_setDeprioritizationFlag(bool enabled,
                                                 std::function<void(TicketingState&)> setFlag) {
    const TicketingState oldState = _state.loadRelaxed();

    TicketingState newState = oldState;
    setFlag(newState);

    auto status =
        checkPrioritizationTransition(oldState.usesPrioritization(), newState.usesPrioritization());
    if (!status.isOK()) {
        return status;
    }

    _state.store(newState);
    return Status::OK();
}

Status TicketingSystem::setDeprioritizationGate(bool enabled) {
    return _setDeprioritizationFlag(
        enabled, [enabled](TicketingState& state) { state.deprioritization.gate = enabled; });
}

Status TicketingSystem::setHeuristicDeprioritization(bool enabled) {
    return _setDeprioritizationFlag(
        enabled, [enabled](TicketingState& state) { state.deprioritization.heuristic = enabled; });
}

Status TicketingSystem::setBackgroundTasksDeprioritization(bool enabled) {
    return _setDeprioritizationFlag(enabled, [enabled](TicketingState& state) {
        state.deprioritization.backgroundTasks = enabled;
    });
}

void TicketingSystem::_appendOperationStats(BSONObjBuilder& b, OperationType opType) const {
    const auto& shortExecutionStats =
        opType == OperationType::kRead ? _operationStats.readShort : _operationStats.writeShort;
    const auto& longExecutionStats =
        opType == OperationType::kRead ? _operationStats.readLong : _operationStats.writeLong;

    BSONObjBuilder shortB(b.subobjStart(kShortRunningName));
    shortExecutionStats.appendStats(shortB);
    shortB.done();

    BSONObjBuilder longB(b.subobjStart(kLongRunningName));
    longExecutionStats.appendStats(longB);
    longB.done();
}

void TicketingSystem::_appendTicketHolderStats(BSONObjBuilder& b,
                                               StringData fieldName,
                                               bool prioritizationEnabled,
                                               const AdmissionContext::Priority& priority,
                                               const std::unique_ptr<TicketHolder>& holder,
                                               OperationType opType,
                                               boost::optional<BSONObjBuilder>& opStats,
                                               int32_t& out,
                                               int32_t& available,
                                               int32_t& totalTickets) const {
    if (!opStats.is_initialized()) {
        opStats.emplace();
    }
    BSONObjBuilder bb(opStats->subobjStart(fieldName));
    holder->appendTicketStats(bb);
    holder->appendHolderStats(bb);
    auto obj = bb.done();
    // Totalization of tickets for the aggregate only if prioritization is enabled.
    if (priority == AdmissionContext::Priority::kNormal || prioritizationEnabled) {
        out += obj.getIntField("out");
        available += obj.getIntField("available");
        totalTickets += obj.getIntField("totalTickets");
    }
    if (priority == AdmissionContext::Priority::kNormal) {
        BSONObjBuilder exemptBuilder(opStats->subobjStart(kExemptPriorityName));
        holder->appendExemptStats(opStats.value());
        exemptBuilder.done();
        // Report short/long running operation that lives in the normal holder.
        _appendOperationStats(opStats.value(), opType);
    }
}

void TicketingSystem::appendStats(BSONObjBuilder& b) const {
    boost::optional<BSONObjBuilder> readStats;
    boost::optional<BSONObjBuilder> writeStats;
    int32_t readOut = 0, readAvailable = 0, readTotalTickets = 0;
    int32_t writeOut = 0, writeAvailable = 0, writeTotalTickets = 0;
    bool pioritizationEnabled = usesPrioritization();
    _state.loadRelaxed().appendStats(b);
    b.append("totalDeprioritizations", _opsDeprioritized.loadRelaxed());
    b.append("heuristicDeprioritizationThreshold",
             admission::execution_control::gHeuristicNumAdmissionsDeprioritizeThreshold.load());

    for (size_t i = 0; i < _holders.size(); ++i) {
        const auto priority = static_cast<AdmissionContext::Priority>(i);

        if (priority == AdmissionContext::Priority::kExempt) {
            // Do not report statistics for kExempt as they are included in the normal priority
            // pool. Also, low priority statistics should only be reported when prioritization is
            // enabled.
            continue;
        }

        const auto& rw = _holders[i];

        const auto& fieldName = priority == AdmissionContext::Priority::kNormal
            ? kNormalPriorityName
            : kLowPriorityName;
        if (rw.read) {
            _appendTicketHolderStats(b,
                                     fieldName,
                                     pioritizationEnabled,
                                     priority,
                                     rw.read,
                                     OperationType::kRead,
                                     readStats,
                                     readOut,
                                     readAvailable,
                                     readTotalTickets);
        }
        if (rw.write) {
            _appendTicketHolderStats(b,
                                     fieldName,
                                     pioritizationEnabled,
                                     priority,
                                     rw.write,
                                     OperationType::kWrite,
                                     writeStats,
                                     writeOut,
                                     writeAvailable,
                                     writeTotalTickets);
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

    // Report finalized stats (CPU, elapsed, load shed) by short/long running classification.
    // These are independent of read/write operation type.
    {
        BSONObjBuilder shortB(b.subobjStart(kShortRunningName));
        _operationStats.shortRunning.appendStats(shortB);
        shortB.done();
    }
    {
        BSONObjBuilder longB(b.subobjStart(kLongRunningName));
        _operationStats.longRunning.appendStats(longB);
        longB.done();
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

void TicketingSystem::finalizeOperationStats(OperationContext* opCtx,
                                             int64_t elapsedMicros,
                                             int64_t cpuUsageMicros) {
    auto finalizedStats =
        ExecutionAdmissionContext::get(opCtx).finalizeStats(cpuUsageMicros, elapsedMicros);
    bool wasDeprioritized = finalizedStats.wasDeprioritized;

    // Increment ticket holder (normal and low priority) statistics counters.
    auto priority =
        wasDeprioritized ? AdmissionContext::Priority::kLow : AdmissionContext::Priority::kNormal;
    _getHolder(priority, OperationType::kRead)
        ->incrementDelinquencyStats(finalizedStats.readDelinquency);
    _getHolder(priority, OperationType::kWrite)
        ->incrementDelinquencyStats(finalizedStats.writeDelinquency);

    // Increment finalized stats (CPU, elapsed, load shed) - only depend on short/long running.
    _operationStats.shortRunning += finalizedStats.shortRunning;
    _operationStats.longRunning += finalizedStats.longRunning;

    // Increment per-acquisition execution stats (short and long running) by read/write type.
    _operationStats.readShort += finalizedStats.readShort;
    _operationStats.readLong += finalizedStats.readLong;
    _operationStats.writeShort += finalizedStats.writeShort;
    _operationStats.writeLong += finalizedStats.writeLong;

    // Increment other global statistics counters.
    if (wasDeprioritized) {
        _opsDeprioritized.fetchAndAddRelaxed(1);
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
    admCtx->setOperationType(o);

    if (opCtx->uninterruptibleLocksRequested_DO_NOT_USE()) {  // NOLINT
        return holder->waitForTicketUntilNoInterrupt_DO_NOT_USE(opCtx, admCtx, until);
    }

    return holder->waitForTicketUntil(opCtx, admCtx, until);
}

void TicketingSystem::startThroughputProbe() {
    tassert(11132202,
            "The throughput probing parameter should be enabled. This is only safe to use for "
            "initialization purposes.",
            _state.loadRelaxed().usesThroughputProbing);

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
    return deprioritization.isActive();
}

void TicketingSystem::TicketingState::appendStats(BSONObjBuilder& b) const {
    b.append("usesThroughputProbing", usesThroughputProbing);
    b.append("usesPrioritization", usesPrioritization());
    b.append("deprioritizationGate", deprioritization.gate);
    b.append("heuristicDeprioritization", deprioritization.heuristic);
    b.append("backgroundTasksDeprioritization", deprioritization.backgroundTasks);
}

}  // namespace mongo::admission::execution_control
