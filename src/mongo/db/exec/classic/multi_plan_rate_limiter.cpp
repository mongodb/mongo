// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/multi_plan_rate_limiter.h"

#include "mongo/db/exec/classic/multi_plan_admission_context.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"

namespace mongo {
namespace {
/**
 * The total number of multiplannings allowed to proceed by the rate limiter.
 */
auto& rateLimiterAllowedCount = *MetricBuilder<Counter64>{"query.multiPlanner.rateLimiter.allowed"};

/**
 * The total number of multiplannings delayed but allowed to proceed by the rate limiter.
 */
auto& rateLimiterDelayedCount = *MetricBuilder<Counter64>{"query.multiPlanner.rateLimiter.delayed"};

/**
 * The total number of released rate limiting ticket holders.
 */
auto& rateLimiterReleasedCount =
    *MetricBuilder<Counter64>{"query.multiPlanner.rateLimiter.released"};

class TicketTable : public RefCountable {
public:
    TicketTable() = default;

    TicketTable(const TicketTable&) = delete;
    TicketTable& operator=(const TicketTable&) = delete;
    TicketTable(TicketTable&&) = delete;
    TicketTable& operator=(TicketTable&&) = delete;

    std::shared_ptr<MultiPlanTicketHolder> get(ServiceContext* serviceContext,
                                               const std::string& key,
                                               size_t numCandidatePlans) {
        std::lock_guard guard{_mutex};
        auto [pos, _] = _table.try_emplace(key);
        if (auto holder = pos->second.lock()) {
            return holder;
        }

        // Make sure that we have at least one ticket to be able to always proceed with
        // multi-planning.
        const int numTickets = std::max(internalQueryMaxConcurrentMultiPlanJobsPerCacheKey.load() /
                                            static_cast<int>(numCandidatePlans),
                                        1);
        auto holder = std::make_shared<MultiPlanTicketHolder>(serviceContext, numTickets);
        pos->second = holder;
        return holder;
    }

    void removeQueryShape(const std::string& key) {
        std::lock_guard guard{_mutex};
        auto pos = _table.find(key);
        if (pos != _table.end()) {
            if (auto holder = pos->second.lock()) {
                holder->release();
            }
            _table.erase(pos);
        }
    }

private:
    // The MultiPlanTicket class, which is always allocated on stack and lives during
    // multi-planning, stores shared pointers to MultiPlanTicketHolder. Storing weak pointers here
    // ensures that TicketHolder object is immediately freed once all tickets are destroyed. It
    // is particular useful in situations when a single query of a particular shape is sent just
    // once, and an active cache entry for it is never created.
    stdx::unordered_map<std::string, std::weak_ptr<MultiPlanTicketHolder>> _table;
    std::mutex _mutex;
};

/**
 * Copy assignable wrapper around TicketTable required for Collection decoration.
 */
class TicketTablePointer {
public:
    TicketTablePointer() : _table{make_intrusive<TicketTable>()} {}

    TicketTable* operator->() const {
        return _table.get();
    }

private:
    boost::intrusive_ptr<TicketTable> _table;
};

// Ticket holders stored a collection decoration.
const auto ticketTableDecoration = Collection::declareDecoration<TicketTablePointer>();

const auto rateLimiterDecoration = ServiceContext::declareDecoration<MultiPlanRateLimiter>();
}  // namespace

bool MultiPlanTicket::isTicketHolderReleased() const {
    return _holder->isReleased();
}

MultiPlanTicketHolder::MultiPlanTicketHolder(ServiceContext* serviceContext, int numTickets)
    : _ticketHolder(serviceContext,
                    numTickets,
                    /*trackPeakUsed*/ false,
                    TicketHolder::kDefaultMaxQueueDepth,
                    /* delinquentCallback */ nullptr,
                    /* executionAcquisitionCallback */ nullptr,
                    /* executionWaitedAcquisitionCallback */ nullptr,
                    /* executionReleaseCallback */ nullptr,
                    /* startQueueingCallback */ nullptr,
                    TicketHolder::ResizePolicy::kImmediate),
      _isReleased(false) {}

void MultiPlanTicketHolder::release() {
    // This large value ensures all waiting threads are woken up when the ticket holder is
    // resized because there will be enough tickets for none of them to need to continue
    // waiting.
    static constexpr uint32_t kMaxThreads = 1000000;

    _isReleased.store(true);

    // We don't need operation context for immediate resize.
    _ticketHolder.resize(/* opCtx */ nullptr,
                         /* newSize */ kMaxThreads);
    rateLimiterReleasedCount.increment();
}

boost::optional<MultiPlanTicket> MultiPlanTicketHolder::tryAcquire(AdmissionContext* admCtx) {
    boost::optional<MultiPlanTicket> result{};
    auto ticketOpt = _ticketHolder.tryAcquire(admCtx);
    if (ticketOpt) {
        rateLimiterAllowedCount.increment();
        result = MultiPlanTicket{shared_from_this(), std::move(*ticketOpt)};
    }
    return result;
}

MultiPlanTicket MultiPlanTicketHolder::waitForTicket(OperationContext* opCtx,
                                                     AdmissionContext* admCtx) {
    auto ticket = _ticketHolder.waitForTicket(opCtx, admCtx);
    rateLimiterDelayedCount.increment();
    return MultiPlanTicket{shared_from_this(), std::move(ticket)};
}

MultiPlanTicketManager::MultiPlanTicketManager(std::shared_ptr<MultiPlanTicketHolder> holder,
                                               OperationContext* opCtx)
    : _holder(std::move(holder)), _opCtx(opCtx), _admCtx(&MultiPlanAdmissionContext::get(opCtx)) {}

Atomic<long> MultiPlanRateLimiter::concurrentMultiPlansCounter{0};

MultiPlanRateLimiter& MultiPlanRateLimiter::get(ServiceContext* serviceContext) {
    return rateLimiterDecoration(serviceContext);
}

MultiPlanTicketManager MultiPlanRateLimiter::getTicketManager(OperationContext* opCtx,
                                                              const CollectionAcquisition& coll,
                                                              const CanonicalQuery& cq,
                                                              size_t numCandidatePlans) {
    auto key = plan_cache_key_factory::make<PlanCacheKey>(cq, coll).toString();
    auto ticketHolder = ticketTableDecoration(coll.getCollectionPtr().get())
                            ->get(opCtx->getServiceContext(), key, numCandidatePlans);
    return MultiPlanTicketManager{std::move(ticketHolder), opCtx};
}

void MultiPlanRateLimiter::removeQueryShapeFromMultiPlanRateLimiter(const CollectionPtr& collection,
                                                                    const std::string& key) {
    ticketTableDecoration(collection.get())->removeQueryShape(key);
}
}  // namespace mongo
