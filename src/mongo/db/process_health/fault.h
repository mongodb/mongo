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
#pragma once

#include "mongo/db/process_health/fault_facet.h"
#include "mongo/db/service_context.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/timer.h"

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

    mutable Mutex _mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "Fault::_mutex");
    // We don't need a map by type because we expect to have only few facets.
    // Linear search is much faster, we want to avoid any lock contention here.
    std::deque<FaultFacetPtr> _facets;
};

using FaultPtr = std::shared_ptr<Fault>;
using FaultConstPtr = std::shared_ptr<const Fault>;

}  // namespace process_health
}  // namespace mongo
