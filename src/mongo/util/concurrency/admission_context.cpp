/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/util/concurrency/admission_context.h"

#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {
static constexpr StringData kLowString = "low"_sd;
static constexpr StringData kNormalString = "normal"_sd;
static constexpr StringData kExemptString = "exempt"_sd;

const auto admissionContextDecoration = OperationContext::declareDecoration<AdmissionContext>();
}  // namespace

AdmissionContext& AdmissionContext::get(OperationContext* opCtx) {
    return admissionContextDecoration(opCtx);
}

void AdmissionContext::copyTo(OperationContext* opCtx,
                              boost::optional<AdmissionContext::Priority> newPriority) {
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    if (newPriority) {
        AdmissionContext newContext(*this);
        newContext._priority = *newPriority;
        admissionContextDecoration(opCtx) = newContext;
        return;
    }

    admissionContextDecoration(opCtx) = *this;
}

ScopedAdmissionPriority::ScopedAdmissionPriority(OperationContext* opCtx,
                                                 AdmissionContext::Priority priority)
    : _opCtx(opCtx), _originalPriority(AdmissionContext::get(opCtx).getPriority()) {
    uassert(ErrorCodes::IllegalOperation,
            "It is illegal for an operation to demote a high priority to a lower priority "
            "operation",
            _originalPriority != AdmissionContext::Priority::kExempt ||
                priority == AdmissionContext::Priority::kExempt);

    stdx::lock_guard<Client> lk(*_opCtx->getClient());
    admissionContextDecoration(opCtx)._priority = priority;
}

ScopedAdmissionPriority::~ScopedAdmissionPriority() {
    stdx::lock_guard<Client> lk(*_opCtx->getClient());
    admissionContextDecoration(_opCtx)._priority = _originalPriority;
}

StringData toString(AdmissionContext::Priority priority) {
    switch (priority) {
        case AdmissionContext::Priority::kLow:
            return kLowString;
        case AdmissionContext::Priority::kNormal:
            return kNormalString;
        case AdmissionContext::Priority::kExempt:
            return kExemptString;
    }
    MONGO_UNREACHABLE;
}
}  // namespace mongo
