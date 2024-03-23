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

#include "mongo/db/admission/ingress_admission_controller.h"

#include "mongo/db/admission/ingress_admission_control_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/decorable.h"
#include <memory>

namespace mongo {

namespace {
const auto getIngressAdmissionController =
    ServiceContext::declareDecoration<IngressAdmissionController>();

const ConstructorActionRegistererType<ServiceContext> onServiceContextCreate{
    "InitIngressAdmissionController", [](ServiceContext* ctx) {
        getIngressAdmissionController(ctx).init();
    }};

class WaitingForIngressAdmissionGuard {
public:
    explicit WaitingForIngressAdmissionGuard(OperationContext* opCtx) : _opCtx(opCtx) {
        stdx::unique_lock<Client> lk(*_opCtx->getClient());
        CurOp::get(opCtx)->setWaitingForIngressAdmission(lk, true);
    }

    ~WaitingForIngressAdmissionGuard() {
        stdx::unique_lock<Client> lk(*_opCtx->getClient());
        CurOp::get(_opCtx)->setWaitingForIngressAdmission(lk, false);
    }

private:
    OperationContext* _opCtx;
};
}  // namespace

IngressAdmissionController::IngressAdmissionController() {}

void IngressAdmissionController::init() {
    _ticketHolder =
        std::make_unique<SemaphoreTicketHolder>(&getIngressAdmissionController.owner(*this),
                                                gIngressAdmissionControllerTicketPoolSize.load(),
                                                false);
}

IngressAdmissionController& IngressAdmissionController::get(OperationContext* opCtx) {
    return getIngressAdmissionController(opCtx->getServiceContext());
}

Ticket IngressAdmissionController::admitOperation(OperationContext* opCtx) {
    auto& admCtx = AdmissionContext::get(opCtx);
    auto* curOp = CurOp::get(opCtx);

    // Try to get the ticket without waiting
    if (auto ticket = _ticketHolder->tryAcquire(&admCtx)) {
        return std::move(*ticket);
    }

    // Mark the operation as waiting for ticket
    WaitingForIngressAdmissionGuard guard{opCtx};
    return _ticketHolder->waitForTicket(
        *opCtx, &admCtx, curOp->debug().waitForIngressAdmissionTicketDurationMicros);
}

void IngressAdmissionController::resizeTicketPool(int32_t newSize) {
    uassert(8611200, "Failed to resize ticket pool", _ticketHolder->resize(newSize));
}

void IngressAdmissionController::appendStats(BSONObjBuilder& b) const {
    _ticketHolder->appendStats(b);
}

Status IngressAdmissionController::onUpdateTicketPoolSize(int32_t newValue) try {
    auto* svcCtx = getCurrentServiceContext();
    if (svcCtx != nullptr) {
        getIngressAdmissionController(svcCtx).resizeTicketPool(newValue);
    }
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

}  // namespace mongo
