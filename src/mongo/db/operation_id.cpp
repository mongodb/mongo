// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/operation_id.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/tick_source.h"

#include <deque>

namespace mongo {

namespace {
const auto getOperationIdManager = ServiceContext::declareDecoration<OperationIdManager>();
constexpr auto kTimeUntilReuse = Seconds{3600};  // 1 hour

TickSource::Tick durationToTicks(TickSource* tickSource, Seconds duration) {
    return static_cast<TickSource::Tick>(duration.count() * tickSource->getTicksPerSecond());
}
}  // namespace

OperationIdManager& OperationIdManager::get(ServiceContext* svcCtx) {
    return (*svcCtx)[getOperationIdManager];
}

struct Lease {
    OperationId start;
};

struct ReleasedLease {
    Lease lease;
    TickSource::Tick reuseAfter;
};

struct OperationIdManager::IdPool {
    Lease lease(TickSource* tickSource) {
        if (_exhaustedUniqueIds) {
            invariant(!_released.empty(),
                      "Process has run out of OperationIds. This indicates that there are too many "
                      "open client connections for the process to handle.");
            return _reuseOldestLease();
        }

        // If enough time has elapsed, reuse a lease instead of getting a new one.
        if (!_released.empty() && tickSource->getTicks() >= _released.front().reuseAfter) {
            return _reuseOldestLease();
        }

        auto leaseStart = _nextId;
        _nextId += _leaseSize;

        OperationId probe = 0;
        if (MONGO_unlikely(overflow::add(_nextId, static_cast<OperationId>(_leaseSize), &probe))) {
            _exhaustedUniqueIds = true;
        }
        return {leaseStart};
    }

    void releaseLease(TickSource* tickSource, Lease lease) {
        _released.push_back(
            {lease, tickSource->getTicks() + durationToTicks(tickSource, kTimeUntilReuse)});
    }

    void setLeaseSize_forTest(size_t size) {
        invariant(!_exhaustedUniqueIds && !_nextId,
                  "Cannot change lease size after a lease is issued");
        _leaseSize = size;
    }

private:
    Lease _reuseOldestLease() {
        auto l = _released.front();
        _released.pop_front();
        return l.lease;
    }

    bool _exhaustedUniqueIds{false};
    OperationId _nextId{0};
    // Stores leases that have already been used. Such leases can be reused after kTimeUntilReuse
    // has elapsed. The next ready ReleasedLease will always be at the front.
    std::deque<ReleasedLease> _released;
    size_t _leaseSize = OperationIdManager::kDefaultLeaseSize;
};

OperationIdManager::OperationIdManager() : _pool(std::make_unique<OperationIdManager::IdPool>()) {}

#define VERIFY_LEASE_START(start, bitmask) invariant((start & ~bitmask) == 0)

// Rely on Decorable zeroing memory for initialization, so we can use
// the trivially_initializable optimization.
struct ClientState {
    size_t unused;
    Lease lease;
    OperationId nextId;
    bool isInitialized;
};

namespace {
const auto getClientState = Client::declareDecoration<ClientState>();
}  // namespace

OperationId OperationIdManager::issueForClient(Client* client) {
    auto* tickSource = client->getServiceContext()->getTickSource();
    auto getLease = [this, client, tickSource](Lease* oldLease) {
        std::lock_guard lk(_mutex);
        auto lease = _pool->lease(tickSource);
        VERIFY_LEASE_START(lease.start, _leaseStartBitMask);

        bool successfullyInsertedNewClientEntry =
            _clientByOperationId.insert({lease.start, client}).second;
        invariant(successfullyInsertedNewClientEntry,
                  "Attempted to overwrite an existing client assoicated with this OperationId.");

        if (oldLease) {
            VERIFY_LEASE_START(oldLease->start, _leaseStartBitMask);
            _clientByOperationId.erase(oldLease->start);
            _pool->releaseLease(tickSource, *oldLease);
        }

        return lease;
    };

    auto& state = getClientState(client);
    if (MONGO_unlikely(state.unused == 0)) {
        state.unused = _leaseSize;
        state.lease = getLease(state.isInitialized ? &(state.lease) : nullptr);
        state.nextId = state.lease.start;
        state.isInitialized = true;
    }

    --state.unused;
    return state.nextId++;
}

void OperationIdManager::eraseClientFromMap(Client* client) {
    auto& state = getClientState(client);
    if (state.isInitialized) {
        std::lock_guard lk(_mutex);
        VERIFY_LEASE_START(state.lease.start, _leaseStartBitMask);
        auto numErased = _clientByOperationId.erase(state.lease.start);
        invariant(numErased == 1);
        _pool->releaseLease(client->getServiceContext()->getTickSource(), state.lease);
        state.isInitialized = false;
    }
}

ClientLock OperationIdManager::findAndLockClient(OperationId id) const {
    std::lock_guard lk(_mutex);
    auto it = _clientByOperationId.find(id & _leaseStartBitMask);
    if (it == _clientByOperationId.end()) {
        return {};
    }

    ClientLock lc(it->second);

    // Confirm that the provided id matches that of the client's active opCtx.
    if (auto opCtx = lc->getOperationContext(); opCtx && opCtx->getOpID() == id) {
        return lc;
    }

    return {};
}

void OperationIdManager::setLeaseSize_forTest(size_t newSize) {
    invariant(TestingProctor::instance().isEnabled());
    invariant((newSize & (newSize - 1)) == 0, "Thew new lease size must be a power of 2");
    _leaseSize = newSize;
    _leaseStartBitMask = ~(_leaseSize - 1);
    _pool->setLeaseSize_forTest(newSize);
}

}  // namespace mongo
