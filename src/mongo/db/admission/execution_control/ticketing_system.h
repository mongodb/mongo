// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/execution_control/execution_control_stats.h"
#include "mongo/db/admission/execution_control/throughput_probing.h"
#include "mongo/db/admission/ticketing/ticketholder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace admission {
namespace [[MONGO_MOD_PUBLIC]] execution_control {
using namespace std::literals::string_view_literals;

enum class ExecutionControlConcurrencyAdjustmentAlgorithmEnum;

/**
 * A ticket mechanism is required for global lock acquisition to reduce contention on storage engine
 * resources.
 *
 * The TicketingSystem maintains n pools of m available tickets. It is responsible for sizing each
 * ticket pool and determining which pool a caller should use for ticket acquisition.
 *
 * It can operate in one of several concurrency adjustment modes:
 *   - Fixed concurrency: a single pool with fixed number of tickets, no prioritization.
 *   - Fixed concurrency with prioritization: two pools, where operations are redirected to each
 * pool based on a heuristic.
 *   - Throughput probing: a single pool is dynamically adjusted based on observed throughput.
 */
class [[MONGO_MOD_PUBLIC]] TicketingSystem {
public:
    static constexpr auto kDefaultConcurrentTransactionsValue = 128;
    static constexpr auto kUnsetLowPriorityConcurrentTransactionsValue = -1;
    static constexpr auto kExemptPriorityName = "exempt"sv;
    static constexpr auto kLowPriorityName = "lowPriority"sv;
    static constexpr auto kNormalPriorityName = "normalPriority"sv;
    static constexpr auto kNonDeprioritizableName = "nonDeprioritizable"sv;
    static constexpr auto kDeprioritizableName = "deprioritizable"sv;

    struct RWTicketHolder {
        std::unique_ptr<TicketHolder> read;
        std::unique_ptr<TicketHolder> write;
    };

    TicketingSystem(
        ServiceContext* svcCtx,
        RWTicketHolder normal,
        RWTicketHolder low,
        ExecutionControlConcurrencyAdjustmentAlgorithmEnum concurrencyAdjustmentAlgorithm);

    /**
     * A collection of static methods for managing normal priority settings.
     */
    class NormalPrioritySettings {
    public:
        static Status updateWriteMaxQueueDepth(std::int32_t newWriteMaxQueueDepth);
        static Status updateReadMaxQueueDepth(std::int32_t newReadMaxQueueDepth);
        static Status updateConcurrentWriteTransactions(const int32_t& newWriteTransactions);
        static Status updateConcurrentReadTransactions(const int32_t& newReadTransactions);
        [[MONGO_MOD_PRIVATE]] static Status validateConcurrentWriteTransactions(
            const int32_t& newWriteTransactions, boost::optional<TenantId>);
        [[MONGO_MOD_PRIVATE]] static Status validateConcurrentReadTransactions(
            const int32_t& newReadTransactions, boost::optional<TenantId>);
    };

    /**
     * A collection of static methods for managing low priority settings.
     */
    class LowPrioritySettings {
    public:
        static Status updateWriteMaxQueueDepth(std::int32_t newWriteMaxQueueDepth);
        static Status updateReadMaxQueueDepth(std::int32_t newReadMaxQueueDepth);
        static Status updateConcurrentWriteTransactions(const int32_t& newWriteTransactions);
        static Status updateConcurrentReadTransactions(const int32_t& newReadTransactions);
        [[MONGO_MOD_PRIVATE]] static Status validateConcurrentWriteTransactions(
            const int32_t& newWriteTransactions, boost::optional<TenantId>);
        [[MONGO_MOD_PRIVATE]] static Status validateConcurrentReadTransactions(
            const int32_t& newReadTransactions, boost::optional<TenantId>);
    };

    [[MONGO_MOD_PRIVATE]] static Status validateConcurrencyAdjustmentAlgorithm(
        const std::string& name, const boost::optional<TenantId>&);

    static Status updateConcurrencyAdjustmentAlgorithm(std::string newAlgorithm);

    static Status updateDeprioritizationGate(bool enabled);

    static Status updateHeuristicDeprioritization(bool enabled);

    static Status updateBackgroundTasksDeprioritization(bool enabled);

    /**
     * Resolves the configured number of low-priority tickets.
     *
     * If the server parameter matches the default, this function returns the number of logical
     * cores. Otherwise, it returns the specific value loaded from the atomic.
     */
    static int resolveLowPriorityTickets(const Atomic<int32_t>& serverParam);

    static TicketingSystem* get(ServiceContext* svcCtx);

    static void use(ServiceContext* svcCtx, std::unique_ptr<TicketingSystem> newTicketingSystem);

    /**
     * Returns true if this TicketingSystem supports runtime size adjustment.
     */
    bool isRuntimeResizable() const;

    /**
     * Returns true if this TicketingSystem supports different ticket pools for prioritization.
     */
    bool usesPrioritization() const;

    /**
     * Sets the maximum queue depth for operations of a given priority and type.
     */
    void setMaxQueueDepth(AdmissionContext::Priority p, OperationType o, int32_t depth);

    /**
     * Sets the maximum number of concurrent transactions (i.e., available tickets) for a given
     * priority and operation type.
     */
    void setConcurrentTransactions(OperationContext* opCtx,
                                   AdmissionContext::Priority p,
                                   OperationType o,
                                   int32_t transactions);

    /**
     * Sets the concurrency adjustment algorithm.
     *
     * This method is used to dynamically change the algorithm used by the ticketing system to
     * adjust the number of concurrent transactions. This includes switching between fixed
     * concurrency and throughput probing-based algorithms.
     */
    void setConcurrencyAdjustmentAlgorithm(OperationContext* opCtx, std::string algorithmName);

    /**
     * Opens or closes the global deprioritization gate in the ticketing system.
     *
     * When opened (`enabled = true`), feature-specific deprioritization controls (such as heuristic
     * and background task deprioritization) are allowed to take effect. When closed (`enabled =
     * false`), all deprioritization mechanisms are blocked, regardless of their individual
     * settings.
     */
    Status setDeprioritizationGate(bool enabled);

    /**
     * Enables or disables automatic deprioritization of operations based on a heuristic.
     *
     * When enabled, operations that yield frequently are automatically assigned to the low-priority
     * ticket pool.
     */
    Status setHeuristicDeprioritization(bool enabled);

    /**
     * Enables or disables automatic deprioritization of background tasks.
     *
     * When enabled, certain background operations (index builds, range deletions, and TTL
     * deletions) are automatically assigned to the low-priority ticket pool upon starting.
     */
    Status setBackgroundTasksDeprioritization(bool enabled);

    /**
     * Appends statistics about the ticketing system's state to a BSON.
     */
    void appendStats(BSONObjBuilder& b) const;

    /**
     * Instantaneous number of tickets that are checked out by an operation.
     */
    int32_t numOfTicketsUsed() const;

    /**
     * Finalizes per-operation stats by accumulating them into the global operation stats.
     *
     * Called at the end of an operation's lifecycle (e.g. CurOp destructor) to aggregate the
     * operation's execution stats into the ticketing system's global counters. This includes
     * delinquency stats, execution time metrics, and operation counts.
     */
    void finalizeOperationStats(OperationContext* opCtx,
                                int64_t elapsedMicros,
                                int64_t cpuUsageMicros);

    /**
     * Attempts to acquire a ticket within a deadline, 'until'.
     */
    boost::optional<Ticket> waitForTicketUntil(OperationContext* opCtx,
                                               OperationType o,
                                               Date_t until) const;

    /**
     * Initializes and starts the periodic job that probes throughput and dynamically adjusts ticket
     * levels. The throughput probing parameter must be enabled before calling this function.
     */
    void startThroughputProbe();

    /**
     * Sets the period for the throughput probing periodic job.
     */
    void setProbingPeriod(Milliseconds period);

private:
    /**
     * Returns the appropriate TicketHolder based on the given priority and operation type.
     */
    TicketHolder* _getHolder(AdmissionContext::Priority p, OperationType o) const;

    /**
     * Appends deprioritizable/non-deprioritizable operation statistics.
     */
    void _appendOperationStats(BSONObjBuilder& b, OperationType opType) const;

    /**
     * Helper function to generalize ticket holder stats report.
     */
    void _appendTicketHolderStats(BSONObjBuilder& b,
                                  std::string_view fieldName,
                                  bool usesPrioritization,
                                  const AdmissionContext::Priority& priority,
                                  const std::unique_ptr<TicketHolder>& holder,
                                  OperationType opType,
                                  boost::optional<BSONObjBuilder>& opStats,
                                  int32_t& out,
                                  int32_t& available,
                                  int32_t& totalTickets) const;

    /**
     * Encapsulates the ticketing system's concurrency mode and the logic that defines its behavior.
     */
    struct TicketingState {
        /**
         * If true, the system uses the throughput probing algorithm (dynamic). Otherwise, it uses
         * the fixed concurrency algorithm (static).
         */
        bool usesThroughputProbing = false;

        /**
         * Logically groups the settings related to operation deprioritization.
         */
        struct DeprioritizationPolicy {
            /**
             * The global switch. If false, NO deprioritization occurs, regardless of the heuristic
             * or background flags.
             */
            bool gate = false;

            /**
             * If true (and gate is true), operations may be deprioritized based on admissions
             * frequency.
             */
            bool heuristic = false;

            /**
             * If true (and gate is true), specific background tasks (e.g., index builds) start as
             * low priority.
             */
            bool backgroundTasks = false;

            /**
             * Returns true if the gate is open and at least one specific policy is enabled.
             */
            bool isActive() const {
                return gate && (heuristic || backgroundTasks);
            }
        };

        /**
         * Holds the current configuration for all deprioritization mechanisms (heuristic,
         * background tasks).
         */
        DeprioritizationPolicy deprioritization;

        /**
         * Returns true if the system is configured to use multiple ticket pools (prioritization).
         */
        bool usesPrioritization() const;

        void appendStats(BSONObjBuilder& b) const;
    };

    /**
     * Atomically holds the current state of the ticketing system. The TicketingState struct
     * contains both the raw algorithm enum and the logic to interpret it.
     */
    Atomic<TicketingState> _state;

    /**
     * Returns true if the operation should be downgraded to low priority.
     */
    bool _shouldDeprioritize(OperationContext* opCtx, ExecutionAdmissionContext* admCtx) const;

    /**
     * Helper method to set a deprioritization flag with proper state transition checking.
     * The setFlag function is invoked to modify the new state before storing it.
     */
    Status _setDeprioritizationFlag(bool enabled, std::function<void(TicketingState&)> setFlag);

    /**
     * Holds the ticket pools for different priority levels.
     *
     * Each entry in the array corresponds to a specific priority level and contains separate
     * holders for read and write operations.
     */
    std::array<RWTicketHolder, static_cast<size_t>(AdmissionContext::Priority::kPrioritiesCount)>
        _holders;

    /**
     * Periodic task that probes system throughput and adjusts the number of concurrent read and
     * write transactions dynamically.
     */
    ThroughputProbing _throughputProbing;

    /**
     * Counts the total number of operations deprioritized.
     */
    Atomic<std::int64_t> _opsDeprioritized;

    /**
     * Counts the total number of operations marked non-deprioritizable. This includes operations
     * that use ScopedTaskTypeNonDeprioritizable and operations which are affected by client based
     * exemptions.
     */
    Atomic<std::int64_t> _opsMarkedNonDeprioritizable;

    /**
     * Accumulate deprioritizable/non-deprioritizable operation statistics for read and write
     * operations.
     */
    AggregatedExecutionStats _operationStats;

    /**
     * Histogram tracking the distribution of completed operations by their total admission count.
     */
    AdmissionsHistogram _admissionsHistogram;
};

}  // namespace execution_control
}  // namespace admission
}  // namespace mongo
