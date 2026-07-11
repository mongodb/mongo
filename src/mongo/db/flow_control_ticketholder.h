// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>
#include <mutex>

namespace mongo {

class OperationContext;
class ServiceContext;

/**
 * This class is fundamentally a semaphore, but allows a caller to increment by X in constant time.
 *
 * Its expected usage maybe differs from a classic resource management protocol. Typically a client
 * would acquire a ticket when it begins an operation and release the ticket to the pool when the
 * operation is completed.
 *
 * In the context of flow control, clients take a ticket and do not return them to the pool. There
 * is an external service that calculates the maximum number of tickets that should be allotted for
 * the next time period (one second). The consumers will call `getTicket` and the producer will call
 * `refreshTo`.
 */
class [[MONGO_MOD_PUBLIC]] FlowControlTicketholder {
public:
    /**
     * A structure to accommodate curop reporting.
     */
    struct CurOp {
        bool waiting = false;
        long long ticketsAcquired = 0;
        long long acquireWaitCount = 0;
        long long timeAcquiringMicros = 0;

        /**
         * Create a sub-object "flowControlStats" on the input builder and write in the structure's
         * fields.
         */
        void writeToBuilder(BSONObjBuilder& infoBuilder);
    };

    FlowControlTicketholder(int startTickets) : _tickets(startTickets), _inShutdown(false) {
        _totalTimeAcquiringMicros.storeRelaxed(0);
    }

    static FlowControlTicketholder* get(ServiceContext* service);
    static FlowControlTicketholder* get(ServiceContext& service);
    static FlowControlTicketholder* get(OperationContext* opCtx);

    static void set(ServiceContext* service, std::unique_ptr<FlowControlTicketholder> flowControl);

    void refreshTo(int numTickets);

    void getTicket(OperationContext* opCtx, FlowControlTicketholder::CurOp* stats);

    std::int64_t totalTimeAcquiringMicros() const {
        return _totalTimeAcquiringMicros.loadRelaxed();
    }

    void setInShutdown();

private:
    // Use an int64_t as this is serialized to bson which does not support unsigned 64-bit numbers.
    Atomic<std::int64_t> _totalTimeAcquiringMicros;

    std::mutex _mutex;
    stdx::condition_variable _cv;
    int _tickets;

    bool _inShutdown;  // used to synchronize shutdown of the ticket refresher job
};

}  // namespace mongo
