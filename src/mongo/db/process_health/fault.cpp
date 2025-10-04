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

#include "mongo/db/process_health/fault.h"

#include "mongo/db/process_health/health_check_status.h"
#include "mongo/db/process_health/health_monitoring_server_parameters_gen.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace mongo {
namespace process_health {

Fault::Fault(ClockSource* clockSource)
    : _clockSource(clockSource), _startTime(_clockSource->now()) {
    invariant(clockSource);  // Will crash before this line, just for readability.
}

UUID Fault::getId() const {
    return _id;
}

Milliseconds Fault::getDuration() const {
    return Milliseconds(_clockSource->now() - _startTime);
}

std::vector<FaultFacetPtr> Fault::getFacets() const {
    auto lk = stdx::lock_guard(_mutex);
    std::vector<FaultFacetPtr> result(_facets.begin(), _facets.end());
    return result;
}

FaultFacetPtr Fault::getFaultFacet(FaultFacetType type) {
    auto lk = stdx::lock_guard(_mutex);
    auto it = std::find_if(_facets.begin(), _facets.end(), [type](const FaultFacetPtr& facet) {
        return facet->getType() == type;
    });
    if (it == _facets.end()) {
        return {};
    }
    return *it;
}

void Fault::removeFacet(FaultFacetType type) {
    auto lk = stdx::lock_guard(_mutex);
    _facets.erase(
        std::remove_if(_facets.begin(),
                       _facets.end(),
                       [this, type](const FaultFacetPtr& f) { return f->getType() == type; }),
        _facets.end());
}

void Fault::upsertFacet(FaultFacetPtr facet) {
    invariant(facet);
    auto type = facet->getType();
    auto lk = stdx::lock_guard(_mutex);
    for (auto& existing : _facets) {
        invariant(existing);
        if (existing->getType() == type) {
            existing->update(facet->getStatus());
            return;
        }
    }
    // We are here if existing was not found - insert new.
    _facets.push_back(std::move(facet));
}

void Fault::garbageCollectResolvedFacets() {
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

void Fault::appendDescription(BSONObjBuilder* builder) const {
    builder->append("id", getId().toBSON());
    builder->append("duration", getDuration().toBSON());
    BSONObjBuilder facetsBuilder;
    auto lk = stdx::lock_guard(_mutex);
    for (auto& facet : _facets) {
        facetsBuilder.append(FaultFacetType_serializer(facet->getType()), facet->toBSON());
    }

    builder->append("facets", facetsBuilder.obj());
    builder->append("numFacets", static_cast<int>(_facets.size()));
}

bool Fault::hasCriticalFacet(const FaultManagerConfig& config) const {
    const auto& facets = this->getFacets();
    for (const auto& facet : facets) {
        auto facetType = facet->getType();
        if (config.getHealthObserverIntensity(facetType) ==
            HealthObserverIntensityEnum::kCritical) {
            return true;
        }
    }
    return false;
}

}  // namespace process_health
}  // namespace mongo
