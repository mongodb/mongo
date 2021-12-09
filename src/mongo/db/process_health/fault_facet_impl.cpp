/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/process_health/fault_facet_impl.h"

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
    auto lk = stdx::lock_guard(_mutex);
    return HealthCheckStatus(getType(), _severity, _description);
}

Milliseconds FaultFacetImpl::getDuration() const {
    return std::max(Milliseconds(0), Milliseconds(_clockSource->now() - _startTime));
}

void FaultFacetImpl::update(HealthCheckStatus status) {
    auto lk = stdx::lock_guard(_mutex);
    _severity = status.getSeverity();
    _description = status.getShortDescription().toString();
}

void FaultFacetImpl::appendDescription(BSONObjBuilder* builder) const {
    builder->append("type", FaultFacetType_serializer(getType()));
    builder->append("severity", _severity);
    builder->append("duration", getDuration().toBSON());
    builder->append("description", _description);
};

}  // namespace process_health
}  // namespace mongo
