// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/static_assert.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <mutex>

namespace [[MONGO_MOD_PUBLIC]] mongo {
class ServiceContext;
class Client;
class ClientLock;

/**
 * Every OperationContext is expected to have a unique OperationId within the scope of its
 * ServiceContext. Generally speaking, OperationId is used for forming maps of OperationContexts and
 * directing metaoperations like killop.
 */
using OperationId = uint32_t;

/**
 * Facility for clients to uniquely identify their operations in the scope of a `ServiceContext`.
 * All public APIs are thread-safe. The first OperationId issued by the OperationIdManager will
 * always be 0.
 */
class OperationIdManager {
public:
    // Supports up to 4,194,304 clients for 32-bit id types.
    // Maximum # of clients = MAX_VALUE(OperationId) / leaseSize
    static constexpr size_t kDefaultLeaseSize = 1024;
    MONGO_STATIC_ASSERT_MSG((kDefaultLeaseSize & (kDefaultLeaseSize - 1)) == 0,
                            "Lease size must be a power of two");

    OperationIdManager();

    static OperationIdManager& get(ServiceContext*);

    /**
     * Issues the next OperationId from the client's lease. May acquire a lock on client's
     * `ServiceContext` if the client has exhausted it's lease.
     */
    OperationId issueForClient(Client*);

    /**
     * Finds the client that holds the lease containing the OperationId -- the id itself will not
     * necessarily be in the map, but the leaseStart that contains the id will be.
     */
    ClientLock findAndLockClient(OperationId id) const;

    /**
     * Removes the client from the _clientByOperationId map. This must be called before the Client
     * is destructed.
     */
    void eraseClientFromMap(Client*);

    // For testing purposes only, we can change the lease size to test behavior when we run out of
    // leases to issue. Other than for testing purposes, leaseSize can be considered a private
    // implementation detail, and should not be modified.
    void setLeaseSize_forTest(size_t);

private:
    struct IdPool;
    friend struct ClientState;

    mutable std::mutex _mutex;
    std::unique_ptr<IdPool> _pool;
    stdx::unordered_map<OperationId, Client*> _clientByOperationId;

    size_t _leaseSize = kDefaultLeaseSize;
    size_t _leaseStartBitMask = ~(kDefaultLeaseSize - 1);
};

}  // namespace mongo
