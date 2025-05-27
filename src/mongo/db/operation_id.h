/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/static_assert.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"

#include <cstdint>

namespace mongo {
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

    mutable stdx::mutex _mutex;
    std::unique_ptr<IdPool> _pool;
    stdx::unordered_map<OperationId, Client*> _clientByOperationId;

    size_t _leaseSize = kDefaultLeaseSize;
    size_t _leaseStartBitMask = ~(kDefaultLeaseSize - 1);
};

}  // namespace mongo
