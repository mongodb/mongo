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
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

namespace mongo {

namespace {
const auto getIngressAdmissionController =
    ServiceContext::declareDecoration<IngressAdmissionController>();
}  // namespace

IngressAdmissionController& IngressAdmissionController::get(OperationContext* opCtx) {
    return getIngressAdmissionController(opCtx->getServiceContext());
}

Ticket IngressAdmissionController::admitOperation(OperationContext* opCtx) {
    // TODO(SERVER-86112): Provide fully functional implementation
    return Ticket::ephemeral(AdmissionContext::get(opCtx));
}

void IngressAdmissionController::resizeTicketPool(int32_t newSize) {
    // TODO(SERVER-86112): Provide fully functional implementation
}

void IngressAdmissionController::appendStats(BSONObjBuilder& b) const {
    // TODO(SERVER-86231): Provide fully functional implementation
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
