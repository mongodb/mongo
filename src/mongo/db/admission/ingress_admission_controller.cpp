// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/ingress_admission_controller.h"

#include "mongo/db/admission/ingress_admission_context.h"
#include "mongo/db/admission/ingress_admission_control_gen.h"
#include "mongo/db/service_context.h"
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
}  // namespace

void IngressAdmissionController::init() {
    _ticketHolder = std::make_unique<TicketHolder>(&getIngressAdmissionController.owner(*this),
                                                   gIngressAdmissionControllerTicketPoolSize.load(),
                                                   false,
                                                   gIngressAdmissionControllerMaxQueueDepth.load(),
                                                   nullptr /* delinquentCallback */,
                                                   nullptr /* executionAcquisitionCallback */,
                                                   nullptr /* executionWaitedAcquisitionCallback */,
                                                   nullptr /* executionReleaseCallback */,
                                                   nullptr /* startQueueingCallback */,
                                                   TicketHolder::ResizePolicy::kImmediate);
}

IngressAdmissionController& IngressAdmissionController::get(OperationContext* opCtx) {
    return getIngressAdmissionController(opCtx->getServiceContext());
}

Ticket IngressAdmissionController::admitOperation(OperationContext* opCtx) {
    auto& admCtx = IngressAdmissionContext::get(opCtx);

    // Try to get the ticket without waiting
    if (auto ticket = _ticketHolder->tryAcquire(&admCtx)) {
        return std::move(*ticket);
    }

    return _ticketHolder->waitForTicket(opCtx, &admCtx);
}

void IngressAdmissionController::resizeTicketPool(OperationContext* opCtx, int32_t newSize) {
    uassert(8611200, "Failed to resize ticket pool", _ticketHolder->resize(opCtx, newSize));
}

void IngressAdmissionController::setMaxQueueDepth(std::int32_t newMaxQueueDepth) {
    _ticketHolder->setMaxQueueDepth(newMaxQueueDepth);
}

void IngressAdmissionController::appendStats(BSONObjBuilder& b) const {
    _ticketHolder->appendTicketStats(b);
    {
        BSONObjBuilder bb(b.subobjStart(kNormalPriorityName));
        _ticketHolder->appendHolderStats(bb);
        bb.done();
    }
    {
        BSONObjBuilder bb(b.subobjStart(kExemptPriorityName));
        _ticketHolder->appendExemptStats(b);
        bb.done();
    }
}

Status IngressAdmissionController::onUpdateTicketPoolSize(int32_t newValue) try {
    if (auto client = Client::getCurrent()) {
        auto opCtx = client->getOperationContext();
        getIngressAdmissionController(client->getServiceContext())
            .resizeTicketPool(opCtx, newValue);
    }

    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

Status IngressAdmissionController::onUpdateMaxQueueDepth(std::int32_t newMaxQueueDepth) {
    if (auto const client = Client::getCurrent()) {
        getIngressAdmissionController(client->getServiceContext())
            .setMaxQueueDepth(newMaxQueueDepth);
    }

    return Status::OK();
}
}  // namespace mongo
