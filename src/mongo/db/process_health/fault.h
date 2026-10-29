// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/process_health/fault_facet.h"
#include "mongo/db/process_health/fault_manager_config.h"
#include "mongo/db/service_context.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"
#include "mongo/util/uuid.h"

#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace process_health {

/**
 * Internal implementation of the Fault class.
 * @see Fault
 */
class Fault : public std::enable_shared_from_this<Fault> {
    Fault(const Fault&) = delete;
    Fault& operator=(const Fault&) = delete;

public:
    explicit Fault(ClockSource* clockSource);

    ~Fault() = default;

    // Fault interface.

    UUID getId() const;

    /**
     * @return The lifetime of this fault from the moment it was created.
     *         Invariant: getDuration() >= getActiveFaultDuration()
     */
    Milliseconds getDuration() const;

    /**
     * Describes the current fault.
     */
    void appendDescription(BSONObjBuilder* builder) const;

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        appendDescription(&builder);
        return builder.obj();
    }

    std::vector<FaultFacetPtr> getFacets() const;

    /**
     * Checks that a Facet of a given type already exists and returns it.
     *
     * @returns existing facet or null.
     */
    FaultFacetPtr getFaultFacet(FaultFacetType type);

    /**
     * Update the fault with supplied facet.
     *
     * @param facet new value to insert/replace or nullptr to delete.
     */
    void upsertFacet(FaultFacetPtr facet);


    /**
     * Delete a facet from this fault by its type.
     *
     * @param type type of facet to remove.
     */
    void removeFacet(FaultFacetType type);

    /**
     * Performs necessary actions to delete all resolved facets.
     */
    void garbageCollectResolvedFacets();

    bool hasCriticalFacet(const FaultManagerConfig& config) const;

private:
    const UUID _id = UUID::gen();

    ClockSource* const _clockSource;
    const Date_t _startTime;

    mutable std::mutex _mutex;
    // We don't need a map by type because we expect to have only few facets.
    // Linear search is much faster, we want to avoid any lock contention here.
    std::deque<FaultFacetPtr> _facets;
};

using FaultPtr = std::shared_ptr<Fault>;
using FaultConstPtr = std::shared_ptr<const Fault>;

}  // namespace process_health
}  // namespace mongo
