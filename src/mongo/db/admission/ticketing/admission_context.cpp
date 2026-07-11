// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/ticketing/admission_context.h"

#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;
static constexpr std::string_view kNormalString = "normal"sv;
static constexpr std::string_view kLowString = "low"sv;
static constexpr std::string_view kExemptString = "exempt"sv;
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

std::int32_t AdmissionContext::getLowAdmissions() const {
    return _lowAdmissions.loadRelaxed();
}

std::int32_t AdmissionContext::getExemptedAdmissions() const {
    return _exemptedAdmissions.loadRelaxed();
}

AdmissionContext::Priority AdmissionContext::getPriority() const {
    return _priority.loadRelaxed();
}

bool AdmissionContext::getLoadShed() const {
    return _loadShed.loadRelaxed();
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

void AdmissionContext::recordLowAdmission() {
    _lowAdmissions.fetchAndAdd(1);
}

void AdmissionContext::recordExemptedAdmission() {
    _exemptedAdmissions.fetchAndAdd(1);
}

void AdmissionContext::recordOperationLoadShed() {
    _loadShed.store(true);
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

std::string_view toString(AdmissionContext::Priority priority) {
    switch (priority) {
        case AdmissionContext::Priority::kNormal:
            return kNormalString;
        case AdmissionContext::Priority::kLow:
            return kLowString;
        case AdmissionContext::Priority::kExempt:
            return kExemptString;
        case AdmissionContext::Priority::kPrioritiesCount:
            MONGO_UNREACHABLE_TASSERT(11039603);
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
