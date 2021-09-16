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

#include "mongo/db/process_health/health_observer_base.h"

#include "mongo/db/service_context.h"

namespace mongo {
namespace process_health {

HealthObserverBase::HealthObserverBase(ClockSource* clockSource) : _clockSource(clockSource) {}

void HealthObserverBase::periodicCheck(FaultFacetsContainerFactory& factory) {
    if (getIntensity() == HealthObserverIntensity::kOff) {
        return;
    }

    // Before invoking the implementation callback, we need to find out if
    // there is an ongoing fault of this kind.
    FaultFacetPtr optionalExistingFacet;
    auto optionalExistingContainer = factory.getFaultFacetsContainer();

    if (optionalExistingContainer) {
        optionalExistingFacet = optionalExistingContainer->getFaultFacet(getType());
    }

    // Do the health check.
    optionalExistingFacet = periodicCheckImpl(optionalExistingFacet);

    // Send the result back to container.
    optionalExistingContainer = factory.getFaultFacetsContainer();

    if (!optionalExistingContainer && !optionalExistingFacet) {
        return;  // Nothing to do.
    }

    if (!optionalExistingContainer) {
        // Need to create container first.
        optionalExistingContainer = factory.getOrCreateFaultFacetsContainer();
    }
    invariant(optionalExistingContainer);

    optionalExistingContainer->updateWithSuppliedFacet(getType(), optionalExistingFacet);
}

HealthObserverIntensity HealthObserverBase::getIntensity() {
    return _intensity;
}

}  // namespace process_health
}  // namespace mongo
