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

#include <memory>

#include "mongo/db/process_health/fault_facet.h"

namespace mongo {
namespace process_health {

/**
 * Interface for the container of Fault facets.
 */
class FaultFacetsContainer {
public:
    /**
     * We do not allow the facets added to this container to be immediately deleted. This
     * is the minimal lifetime before a fully resolved facet could be deleted.
     */
    static constexpr Milliseconds kMinimalFacetLifetimeToDelete = Milliseconds(10000);

    virtual ~FaultFacetsContainer() = default;

    virtual std::vector<FaultFacetPtr> getFacets() const = 0;

    /**
     * Checks that a Facet of a given type already exists and returns it.
     */
    virtual boost::optional<FaultFacetPtr> getFaultFacet(FaultFacetType type) = 0;

    /**
     * Getter that takes a create callback in case the facet of a given type is missing.
     * We do not have a separate create factory interface to avoid having the registration
     * mechanism for those factories, which is not necessary.
     *
     * @param createCb The callback is invoked only if the facet of this type does not exist.
     */
    virtual FaultFacetPtr getOrCreateFaultFacet(FaultFacetType type,
                                                std::function<FaultFacetPtr()> createCb) = 0;

    /**
     * Performs necessary actions to delete all resolved facets with lifetime of
     * at least kMinimalFacetLifetimeToDelete.
     *
     * The interface for deleting facets is not provided because the container should
     * garbage collect them.
     */
    virtual void garbageCollectResolvedFacets() = 0;
};

using FaultFacetsContainerPtr = std::shared_ptr<FaultFacetsContainer>;

/**
 * Interface to get or create a FaultFacetsContainer.
 * The implementor of this interface owns the singleton instance.
 */
class FaultFacetsContainerFactory {
public:
    virtual ~FaultFacetsContainerFactory() = default;

    virtual boost::optional<FaultFacetsContainerPtr> getFaultFacetsContainer() = 0;

    virtual FaultFacetsContainerPtr getOrCreateFaultFacetsContainer() = 0;
};

}  // namespace process_health
}  // namespace mongo
