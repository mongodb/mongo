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

#include "mongo/db/process_health/fault_impl.h"

namespace mongo {
namespace process_health {

FaultImpl::FaultImpl(ClockSource* clockSource)
    : _clockSource(clockSource), _startTime(_clockSource->now()) {
    invariant(clockSource);  // Will crash before this line, just for readability.
}

UUID FaultImpl::getId() const {
    return _id;
}

double FaultImpl::getSeverity() const {
    return 0;
}

Milliseconds FaultImpl::getActiveFaultDuration() const {
    return Milliseconds(0);
}

Milliseconds FaultImpl::getDuration() const {
    return Milliseconds(_clockSource->now() - _startTime);
}

std::vector<FaultFacetPtr> FaultImpl::getFacets() const {
    auto lk = stdx::lock_guard(_mutex);
    std::vector<FaultFacetPtr> result(_facets.begin(), _facets.end());
    return result;
}

FaultFacetPtr FaultImpl::getFaultFacet(FaultFacetType type) {
    auto lk = stdx::lock_guard(_mutex);
    auto it = std::find_if(_facets.begin(), _facets.end(), [type](const FaultFacetPtr& facet) {
        return facet->getType() == type;
    });
    if (it == _facets.end()) {
        return {};
    }
    return *it;
}

void FaultImpl::updateWithSuppliedFacet(FaultFacetType type, FaultFacetPtr facet) {
    auto lk = stdx::lock_guard(_mutex);

    if (!facet) {
        // Delete existing.
        _facets.erase(
            std::remove_if(_facets.begin(),
                           _facets.end(),
                           [this, type](const FaultFacetPtr& f) { return f->getType() == type; }),
            _facets.end());
        return;
    }

    invariant(type == facet->getType());
    // Update or insert.
    for (auto& existing : _facets) {
        invariant(existing);
        if (existing->getType() == type) {
            existing = facet;
            return;
        }
    }

    // We are here if existing was not found - insert new.
    _facets.push_back(std::move(facet));
}

void FaultImpl::garbageCollectResolvedFacets() {
    auto lk = stdx::lock_guard(_mutex);
    _facets.erase(std::remove_if(_facets.begin(),
                                 _facets.end(),
                                 [this](const FaultFacetPtr& facet) {
                                     auto status = facet->getStatus();
                                     // Remove if severity is zero
                                     return HealthCheckStatus::isResolved(status.getSeverity());
                                 }),
                  _facets.end());
}

void FaultImpl::appendDescription(BSONObjBuilder* builder) const {}

}  // namespace process_health
}  // namespace mongo
