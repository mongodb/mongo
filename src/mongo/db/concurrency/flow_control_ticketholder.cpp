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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/flow_control_ticketholder.h"

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
const auto getFlowControlTicketholder =
    ServiceContext::declareDecoration<std::unique_ptr<FlowControlTicketholder>>();
}  // namespace

FlowControlTicketholder* FlowControlTicketholder::get(ServiceContext* service) {
    return getFlowControlTicketholder(service).get();
}

FlowControlTicketholder* FlowControlTicketholder::get(ServiceContext& service) {
    return getFlowControlTicketholder(service).get();
}

FlowControlTicketholder* FlowControlTicketholder::get(OperationContext* ctx) {
    return get(ctx->getClient()->getServiceContext());
}

void FlowControlTicketholder::set(ServiceContext* service,
                                  std::unique_ptr<FlowControlTicketholder> flowControl) {
    auto& globalFlow = getFlowControlTicketholder(service);
    globalFlow = std::move(flowControl);
}

void FlowControlTicketholder::refreshTo(int numTickets) {
    invariant(numTickets >= 0);
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    LOG(4) << "Refreshing tickets. Before: " << _tickets << " Now: " << numTickets;
    _tickets = numTickets;
    _cv.notify_all();
}

void FlowControlTicketholder::getTicket(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    LOG(4) << "Taking ticket. Available: " << _tickets;
    while (_tickets == 0) {
        auto code = uassertStatusOK(
            opCtx->waitForConditionOrInterruptNoAssertUntil(_cv, lk, Date_t::max()));
        invariant(code != stdx::cv_status::timeout);
    }
    --_tickets;
}

}  // namespace mongo
