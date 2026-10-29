// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/process_health/fault_facet_impl.h"

#include "mongo/bson/bsonobj.h"

#include <algorithm>
#include <mutex>

namespace mongo {
namespace process_health {

FaultFacetImpl::FaultFacetImpl(FaultFacetType type,
                               ClockSource* clockSource,
                               HealthCheckStatus status)
    : _type(type), _clockSource(clockSource) {
    update(status);
}

FaultFacetType FaultFacetImpl::getType() const {
    return _type;
}

HealthCheckStatus FaultFacetImpl::getStatus() const {
    auto lk = std::lock_guard(_mutex);
    return HealthCheckStatus(getType(), _severity, _description);
}

Milliseconds FaultFacetImpl::getDuration() const {
    return std::max(Milliseconds(0), Milliseconds(_clockSource->now() - _startTime));
}

void FaultFacetImpl::update(HealthCheckStatus status) {
    auto lk = std::lock_guard(_mutex);
    _severity = status.getSeverity();
    _description = std::string{status.getShortDescription()};
}

void FaultFacetImpl::appendDescription(BSONObjBuilder* builder) const {
    decltype(_severity) severity;
    decltype(_description) description;
    {
        std::lock_guard lk(_mutex);
        severity = _severity;
        description = _description;
    }
    builder->append("type", FaultFacetType_serializer(getType()));
    builder->append("severity", severity);
    builder->append("duration", getDuration().toBSON());
    builder->append("description", description);
};

}  // namespace process_health
}  // namespace mongo
