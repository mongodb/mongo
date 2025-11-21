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

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/throughput_probing.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/concurrency/ticketholder.h"

#include <cstdint>
#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo::admission::execution_control {

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
class TicketingSystem {
public:
    static constexpr auto kDefaultConcurrentTransactionsValue = 128;
    static constexpr auto kUnsetLowPriorityConcurrentTransactionsValue = -1;
    static constexpr auto kExemptPriorityName = "exempt"_sd;
    static constexpr auto kLowPriorityName = "lowPriority"_sd;
    static constexpr auto kNormalPriorityName = "normalPriority"_sd;

    enum class OperationType { kRead = 0, kWrite };

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
        static Status validateConcurrentWriteTransactions(const int32_t& newWriteTransactions,
                                                          boost::optional<TenantId>);
        static Status validateConcurrentReadTransactions(const int32_t& newReadTransactions,
                                                         boost::optional<TenantId>);
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
        static Status validateConcurrentWriteTransactions(const int32_t& newWriteTransactions,
                                                          boost::optional<TenantId>);
        static Status validateConcurrentReadTransactions(const int32_t& newReadTransactions,
                                                         boost::optional<TenantId>);
    };

    static Status validateConcurrencyAdjustmentAlgorithm(const std::string& name,
                                                         const boost::optional<TenantId>&);

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
    static int resolveLowPriorityTickets(const AtomicWord<int32_t>& serverParam);

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
     * Bumps the delinquency counters to all ticket holders (read and write pools) and the
     * de-prioritization stats.
     */
    void incrementStats(OperationContext* opCtx);

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

private:
    /**
     * Returns the appropriate TicketHolder based on the given priority and operation type.
     */
    TicketHolder* _getHolder(AdmissionContext::Priority p, OperationType o) const;

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
    AtomicWord<TicketingState> _state;

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
    AtomicWord<std::int64_t> _opsDeprioritized;
};

}  // namespace mongo::admission::execution_control
