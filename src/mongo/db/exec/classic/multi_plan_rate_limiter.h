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

#include "mongo/db/query/canonical_query.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace mongo {
class MultiPlanTicketHolder;

/**
 * A wrapper class around TicketHolder's Ticket. It mainatins shared pointer to its TicketHolder to
 * gurantee safe deconstructions even if the TikcetHolder has been released.
 */
class MultiPlanTicket {
public:
    MultiPlanTicket(std::shared_ptr<MultiPlanTicketHolder> holder, Ticket&& ticket)
        : _holder(std::move(holder)), _ticket(std::move(ticket)) {}

    MultiPlanTicket(const MultiPlanTicket&) = delete;
    MultiPlanTicket& operator=(const MultiPlanTicket&) = delete;

    MultiPlanTicket(MultiPlanTicket&& other) = default;
    MultiPlanTicket& operator=(MultiPlanTicket&& other) = default;

    bool isTicketHolderReleased() const;

    void release() {
        _ticket = boost::none;
    }

    ~MultiPlanTicket() {
        release();
    }

private:
    std::shared_ptr<MultiPlanTicketHolder> _holder;
    boost::optional<Ticket> _ticket;
};

/**
 * A wrapper class around TicketHolder. It is released when it not required to rate limit
 * multiplanning for its corresponing multi plan key.
 */
class MultiPlanTicketHolder : public std::enable_shared_from_this<MultiPlanTicketHolder> {
public:
    MultiPlanTicketHolder(ServiceContext* serviceContext, int numTickets);

    MultiPlanTicketHolder(const MultiPlanTicketHolder&) = delete;
    MultiPlanTicketHolder(MultiPlanTicketHolder&&) = delete;
    MultiPlanTicketHolder& operator=(const MultiPlanTicketHolder&) = delete;
    MultiPlanTicketHolder& operator=(MultiPlanTicketHolder&&) = delete;

    boost::optional<MultiPlanTicket> tryAcquire(AdmissionContext* admCtx);
    MultiPlanTicket waitForTicket(OperationContext* opCtx, AdmissionContext* admCtx);

    bool isReleased() const {
        return _isReleased.load();
    }

    void release();

private:
    TicketHolder _ticketHolder;
    AtomicWord<bool> _isReleased;
};

/**
 * A helper class that encapsulates operation-specific state for rate limiting.
 */
class MultiPlanTicketManager {
public:
    MultiPlanTicketManager(std::shared_ptr<MultiPlanTicketHolder> holder, OperationContext* opCtx);

    boost::optional<MultiPlanTicket> tryAcquire() {
        return _holder->tryAcquire(_admCtx);
    }

    MultiPlanTicket waitForTicket() {
        return _holder->waitForTicket(_opCtx, _admCtx);
    }

private:
    std::shared_ptr<MultiPlanTicketHolder> _holder;
    OperationContext* _opCtx;
    AdmissionContext* _admCtx;
};

/*
 * The MultiPlanRateLimiter controls the number of concurrent multiplannings per plan cache key (ie
 * query shape) to reduce the multiplanner's resource consumption and protect performance. Each rate
 * limited plan cache key is assigned a TicketHolder, which maintains and distributes tickets across
 * multiplan operations from a limited pool of tickets. The query shape and its paired TicketHolder
 * are maintained by the TicketTable, which tracks the query shapes the server has identified
 * as requiring multiplan rate limiting.
 *
 * The multiplanner's rate limit algorithm is as follows:
 *
 * 1. The server will only begin to rate limit multiplanning once the the number of concurrent
 * trialing multiplan candidates across all plan cache keys, passes the
 * internalQueryConcurrentMultiPlanningThreshold.
 *
 * 2. With that threshold surpassed, each thread must request one ticket per candidate plan from
 * that query shape's TicketHolderBefore performing the work of multiplanning in
 * MultiPlanStage::pickBestPlan().
 *
 * 3. Each TicketHolder maintains a number of ticket to ensure that number of concurrent
 * multi-planning candidate plans does not exceed
 * <internalQueryMaxConcurrentMultiPlanJobsPerCacheKey>. If the TicketHolder has a ticket available
 * for each candidate plan, the thread receives the tickets immediately and proceeds to multiplan.
 *
 * 4. If, however, no tickets are available, the thread must wait for the ticket before it can
 * proceed. In this case, the MultiPlanStage yields all resources before and while it waits for
 * tickets.
 *
 * 5. Once tickets are received, multiplanning resumes. When MultiPlanStage::pickBestPlan() returns,
 * the tickets go out of scope and are automatically released back to the TicketHolder's pool of
 * available tickets.
 *
 * 6. When a query shape's plan cache entry is promoted to active, the server will skip
 * multiplanning the next time it encounters said query shape. In other words, plan cache entry
 * activation indicates that the server no longer needs to rate limit multi planning for this query
 * shape. For this reason, in onPromoteCacheEntry(), we wake up all the threads associated with this
 * query shape that are waiting in step 4. We do this by resizing the query shape's associated
 * TicketHolder to the max size to ensure there are enough tickets for all the sleeping threads to
 * immediately receive tickets and proceed to multiplan. After this, we can remove the query shape's
 * entry from the MultiPlanRateLimiter's TicketTable.
 */

class MultiPlanRateLimiter {
public:
    static MultiPlanRateLimiter& get(ServiceContext* serviceContext);

    static AtomicWord<long> concurrentMultiPlansCounter;

    MultiPlanRateLimiter() = default;

    MultiPlanRateLimiter(const MultiPlanRateLimiter&) = delete;
    MultiPlanRateLimiter(MultiPlanRateLimiter&&) = delete;
    MultiPlanRateLimiter& operator=(const MultiPlanRateLimiter&) = delete;
    MultiPlanRateLimiter& operator=(MultiPlanRateLimiter&&) = delete;

    void removeQueryShapeFromMultiPlanRateLimiter(const CollectionPtr& collection,
                                                  const std::string& key);

    MultiPlanTicketManager getTicketManager(OperationContext* opCtx,
                                            const CollectionPtr& coll,
                                            const CanonicalQuery& cq,
                                            size_t numCandidatePlans);

private:
};
}  // namespace mongo
