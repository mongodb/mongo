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
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/concurrency/ticketholder.h"

#include <cstdint>
#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace admission {

/**
 * A ticket mechanism is required for global lock acquisition to reduce contention on storage engine
 * resources.
 *
 * The TicketingSystem maintains n pools of m available tickets. It is responsible for sizing each
 * ticket pool and determining which pool a caller should use for ticket acquisition.
 *
 */
class TicketingSystem {
public:
    static constexpr auto kDefaultConcurrentTransactions = 128;
    static constexpr auto kDefaultLowPriorityConcurrentTransactions = 5;

    enum class Operation { kRead = 0, kWrite };

    TicketingSystem() = default;
    virtual ~TicketingSystem() = default;

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
    };

    static TicketingSystem* get(ServiceContext* svcCtx);

    static void use(ServiceContext* svcCtx, std::unique_ptr<TicketingSystem> newTicketingSystem);

    /**
     * Returns true if this TicketingSystem supports runtime size adjustment.
     */
    virtual bool isRuntimeResizable() const = 0;

    /**
     * Returns true if this TicketingSystem supports different ticket pools for prioritization.
     */
    virtual bool usesPrioritization() const = 0;

    /**
     * Sets the maximum queue depth for operations of a given priority and type.
     */
    virtual void setMaxQueueDepth(AdmissionContext::Priority p, Operation o, int32_t depth) = 0;

    /**
     * Sets the maximum number of concurrent transactions (i.e., available tickets) for a given
     * priority and operation type.
     */
    virtual void setConcurrentTransactions(OperationContext* opCtx,
                                           AdmissionContext::Priority p,
                                           Operation o,
                                           int32_t transactions) = 0;

    /**
     * Appends statistics about the ticketing system's state to a BSON.
     */
    virtual void appendStats(BSONObjBuilder& b) const = 0;

    /**
     * Instantaneous number of tickets that are checked out by an operation.
     */
    virtual int32_t numOfTicketsUsed() const = 0;

    /**
     * Bumps the delinquency counters to all ticket holders (read and write pools).
     */
    virtual void incrementDelinquencyStats(OperationContext* opCtx) = 0;

    /**
     * Attempts to acquire a ticket within a deadline, 'until'.
     */
    virtual boost::optional<Ticket> waitForTicketUntil(OperationContext* opCtx,
                                                       Operation o,
                                                       Date_t until) const = 0;
};

}  // namespace admission
}  // namespace mongo
