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
}  // namespace

AdmissionContext::AdmissionContext(const AdmissionContext& other)
    : _admissions(other._admissions.load()),
      _priority(other._priority.load()),
      _totalTimeQueuedMicros(other._totalTimeQueuedMicros.load()),
      _startQueueingTime(other._startQueueingTime.load()) {}

AdmissionContext& AdmissionContext::operator=(const AdmissionContext& other) {
    _admissions.store(other._admissions.load());
    _priority.store(other._priority.load());
    _totalTimeQueuedMicros.store(other._totalTimeQueuedMicros.load());
    _startQueueingTime.store(other._startQueueingTime.load());
    return *this;
}

Microseconds AdmissionContext::totalTimeQueuedMicros() const {
    return Microseconds{_totalTimeQueuedMicros.loadRelaxed()};
}

boost::optional<TickSource::Tick> AdmissionContext::startQueueingTime() const {
    TickSource::Tick startQueueingTime = _startQueueingTime.loadRelaxed();
    if (startQueueingTime == kNotQueueing) {
        return boost::none;
    }

    return startQueueingTime;
}

std::int32_t AdmissionContext::getAdmissions() const {
    return _admissions.loadRelaxed();
}

std::int32_t AdmissionContext::getExemptedAdmissions() const {
    return _exemptedAdmissions.loadRelaxed();
}

AdmissionContext::Priority AdmissionContext::getPriority() const {
    return _priority.loadRelaxed();
}

void AdmissionContext::recordAdmission() {
    _admissions.fetchAndAdd(1);
}

void AdmissionContext::setAdmission_forTest(int32_t admissions) {
    _admissions.store(admissions);
}

void AdmissionContext::setTotalTimeQueuedMicros_forTest(int64_t micros) {
    _totalTimeQueuedMicros.store(micros);
}

void AdmissionContext::recordExemptedAdmission() {
    _exemptedAdmissions.fetchAndAdd(1);
}

ScopedAdmissionPriorityBase::ScopedAdmissionPriorityBase(OperationContext* opCtx,
                                                         AdmissionContext& admCtx,
                                                         AdmissionContext::Priority priority)
    : _opCtx(opCtx), _admCtx(&admCtx), _originalPriority(admCtx.getPriority()) {
    uassert(ErrorCodes::IllegalOperation,
            "It is illegal for an operation to demote a high priority to a lower priority "
            "operation",
            _originalPriority != AdmissionContext::Priority::kExempt ||
                priority == AdmissionContext::Priority::kExempt);
    _admCtx->_priority.store(priority);
}

ScopedAdmissionPriorityBase::~ScopedAdmissionPriorityBase() {
    _admCtx->_priority.store(_originalPriority);
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

WaitingForAdmissionGuard::WaitingForAdmissionGuard(AdmissionContext* admCtx, TickSource* tickSource)
    : _admCtx(admCtx), _tickSource(tickSource) {
    invariant(_admCtx->_startQueueingTime.swap(_tickSource->getTicks()) ==
              AdmissionContext::kNotQueueing);
    _admCtx->_startQueueingTime.notifyAll();
}

WaitingForAdmissionGuard::~WaitingForAdmissionGuard() {
    auto startQueueingTime = _admCtx->_startQueueingTime.loadRelaxed();
    invariant(startQueueingTime != AdmissionContext::kNotQueueing);
    _admCtx->_totalTimeQueuedMicros.fetchAndAdd(durationCount<Microseconds>(
        _tickSource->ticksTo<Microseconds>(_tickSource->getTicks() - startQueueingTime)));
    _admCtx->_startQueueingTime.store(AdmissionContext::kNotQueueing);
    _admCtx->_startQueueingTime.notifyAll();
}

}  // namespace mongo
