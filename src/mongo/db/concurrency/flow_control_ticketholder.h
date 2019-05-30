/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"

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
class FlowControlTicketholder {
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
        _totalTimeAcquiringMicros.store(0);
    }

    static FlowControlTicketholder* get(ServiceContext* service);
    static FlowControlTicketholder* get(ServiceContext& service);
    static FlowControlTicketholder* get(OperationContext* opCtx);

    static void set(ServiceContext* service, std::unique_ptr<FlowControlTicketholder> flowControl);

    void refreshTo(int numTickets);

    void getTicket(OperationContext* opCtx, FlowControlTicketholder::CurOp* stats);

    std::int64_t totalTimeAcquiringMicros() const {
        return _totalTimeAcquiringMicros.load();
    }

    void setInShutdown();

private:
    // Use an int64_t as this is serialized to bson which does not support unsigned 64-bit numbers.
    AtomicWord<std::int64_t> _totalTimeAcquiringMicros;

    stdx::mutex _mutex;
    stdx::condition_variable _cv;
    int _tickets;

    bool _inShutdown;  // used to synchronize shutdown of the ticket refresher job
};

}  // namespace mongo
