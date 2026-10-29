// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/admission/ticketing/ticketholder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/util/modules.h"

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
    Atomic<bool> _isReleased;
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

    static Atomic<long> concurrentMultiPlansCounter;

    MultiPlanRateLimiter() = default;

    MultiPlanRateLimiter(const MultiPlanRateLimiter&) = delete;
    MultiPlanRateLimiter(MultiPlanRateLimiter&&) = delete;
    MultiPlanRateLimiter& operator=(const MultiPlanRateLimiter&) = delete;
    MultiPlanRateLimiter& operator=(MultiPlanRateLimiter&&) = delete;

    void removeQueryShapeFromMultiPlanRateLimiter(const CollectionPtr& collection,
                                                  const std::string& key);

    MultiPlanTicketManager getTicketManager(OperationContext* opCtx,
                                            const CollectionAcquisition& coll,
                                            const CanonicalQuery& cq,
                                            size_t numCandidatePlans);

private:
};
}  // namespace mongo
