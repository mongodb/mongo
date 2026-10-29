// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/operation_id.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/testing_proctor.h"

#include <limits>
#include <list>

namespace mongo {

namespace {
const auto getOperationIdManager = ServiceContext::declareDecoration<OperationIdManager>();
}  // namespace

OperationIdManager& OperationIdManager::get(ServiceContext* svcCtx) {
    return (*svcCtx)[getOperationIdManager];
}

struct Lease {
    OperationId start;
};

struct OperationIdManager::IdPool {
    Lease lease(WithLock) {
        if (exhaustedUniqueIds) {
            invariant(!released.empty(),
                      "Process has run out of OperationIds. This indicates that there are too many "
                      "open client connections for the process to handle.");
            auto l = released.front();
            released.pop_front();
            return l;
        }

        auto leaseStart = nextId;
        nextId += leaseSize;
        if (MONGO_unlikely(std::numeric_limits<OperationId>::max() - nextId < leaseSize)) {
            exhaustedUniqueIds = true;
        }
        return {leaseStart};
    }

    void releaseLease(WithLock, Lease lease) {
        released.push_back(lease);
    }

    void setLeaseSize_forTest(size_t size) {
        invariant(!exhaustedUniqueIds && !nextId,
                  "Cannot change lease size after a lease is issued");
        leaseSize = size;
    }

    bool exhaustedUniqueIds{false};
    OperationId nextId{0};
    std::list<Lease> released;
    size_t leaseSize = OperationIdManager::kDefaultLeaseSize;
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
    auto getLease = [this, client](Lease* oldLease) {
        std::lock_guard lk(_mutex);
        auto lease = _pool->lease(lk);
        VERIFY_LEASE_START(lease.start, _leaseStartBitMask);

        bool successfullyInsertedNewClientEntry =
            _clientByOperationId.insert({lease.start, client}).second;
        invariant(successfullyInsertedNewClientEntry,
                  "Attempted to overwrite an existing client assoicated with this OperationId.");

        if (oldLease) {
            VERIFY_LEASE_START(oldLease->start, _leaseStartBitMask);
            _clientByOperationId.erase(oldLease->start);
            _pool->releaseLease(lk, *oldLease);
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
        _pool->releaseLease(lk, state.lease);
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
