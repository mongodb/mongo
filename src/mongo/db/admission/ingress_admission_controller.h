// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/ticketing/ticketholder.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {
using namespace std::literals::string_view_literals;

class [[MONGO_MOD_PUBLIC]] IngressAdmissionController {
public:
    static constexpr auto kNormalPriorityName = "normalPriority"sv;
    static constexpr auto kExemptPriorityName = "exempt"sv;
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
    void resizeTicketPool(OperationContext* opCtx, int32_t newSize);

    /**
     * Adjusts the max queue depth for ingress admission control to 'newMaxQueueDepth'.
     */
    void setMaxQueueDepth(std::int32_t newMaxQueueDepth);

    /**
     * Reports the ingress admission control metrics.
     */
    void appendStats(BSONObjBuilder& b) const;

    /**
     * Called automatically when the value of the server parameter that controls the ticket pool
     * size changes.
     */
    static Status onUpdateTicketPoolSize(int newValue);

    /**
     * Called automatically when the value of the server parameter that controls the max queue
     * depth changes.
     */
    static Status onUpdateMaxQueueDepth(std::int32_t newMaxQueueDepth);

    /**
     * Initialize the IngressAdmissionController after the ServiceContext is constructed. This will
     * be called automatically during static initialization.
     */
    void init();

private:
    std::unique_ptr<TicketHolder> _ticketHolder{nullptr};
};

}  // namespace mongo
