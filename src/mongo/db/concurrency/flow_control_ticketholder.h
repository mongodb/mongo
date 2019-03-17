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

#include "mongo/db/operation_context.h"

namespace mongo {

/**
 * This class is fundamentally a semaphore, but allows a caller to increment by X in constant time.
 *
 * It's expected usage maybe differs from a classic resource management protocol. Typically a client
 * would acquire a ticket when it begins an operation and release the ticket to the pool when the
 * operation is completed.
 *
 * In the context of flow control, clients take a ticket and do not return them to the pool. There
 * is an external client that calculates the maximum number of tickets that should be allotted for
 * the next second. The consumers will call `getTicket` and the producer will call `refreshTo`.
 */
class FlowControlTicketholder {
public:
    FlowControlTicketholder(int startTickets) : _tickets(startTickets) {}

    static FlowControlTicketholder* get(ServiceContext* service);
    static FlowControlTicketholder* get(ServiceContext& service);
    static FlowControlTicketholder* get(OperationContext* ctx);

    static void set(ServiceContext* service, std::unique_ptr<FlowControlTicketholder> flowControl);

    void refreshTo(int numTickets);

    void getTicket(OperationContext* opCtx);

private:
    stdx::mutex _mutex;
    stdx::condition_variable _cv;
    int _tickets;
};

}  // namespace mongo
