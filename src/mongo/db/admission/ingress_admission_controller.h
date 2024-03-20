/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace mongo {
class IngressAdmissionController {
public:
    /**
     * Returns the reference to IngressAdmissionController associated with the operation's service
     * context.
     */
    static IngressAdmissionController& get(OperationContext* opCtx);

    /**
     * Attempts to acquire an ingress admission ticket for the operation. Blocks until a ticket is
     * acquired, or the operation is interrupted, in which case it throws an AssertionException.
     * Operations with kExempt admission priority will always acquire a ticket without waiting and
     * without reducing the number of available tickets.
     */
    Ticket admitOperation(OperationContext* opCtx);

    /**
     * Adjusts the total number of tickets allocated for ingress admission control to 'newSize'.
     */
    void resizeTicketPool(int32_t newSize);

    /**
     * Reports the ingress admission control metrics.
     */
    void appendStats(BSONObjBuilder& b) const;

    /**
     * Called automatically when the value of the server parameter that controls the ticket pool
     * size changes.
     */
    static Status onUpdateTicketPoolSize(int newValue);
};

}  // namespace mongo
