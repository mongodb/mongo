// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/process_health/fault_manager_config.h"
#include "mongo/db/process_health/health_check_status.h"
#include "mongo/util/duration.h"

#include <memory>

namespace mongo {
namespace process_health {

/**
 * Tracks the state of one particular fault facet.
 * The instance is created and deleted by the fault observer when a fault
 * condition is detected or resolved.
 */
class FaultFacet : public std::enable_shared_from_this<FaultFacet> {
public:
    virtual ~FaultFacet() = default;

    virtual FaultFacetType getType() const = 0;

    /**
     * The interface used to communicate with the Fault instance that
     * owns all facets.
     *
     * @return HealthCheckStatus
     */
    virtual HealthCheckStatus getStatus() const = 0;

    virtual Milliseconds getDuration() const = 0;

    /**
     * Change the state of this Facet with health check result.
     */
    virtual void update(HealthCheckStatus status) = 0;

    virtual void appendDescription(BSONObjBuilder* builder) const = 0;

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        appendDescription(&builder);
        return builder.obj();
    }
};

using FaultFacetPtr = std::shared_ptr<FaultFacet>;

}  // namespace process_health
}  // namespace mongo
