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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/process_health/fault_facets_container.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace process_health {

/**
 * Detailed description of the current fault.
 * @see FaultManager for more details.
 */
class Fault : public std::enable_shared_from_this<Fault> {
    Fault(const Fault&) = delete;
    Fault& operator=(const Fault&) = delete;

public:
    Fault() = default;
    virtual ~Fault() = default;

    virtual UUID getId() const = 0;

    /**
     * The fault severity value is an aggregate severity calculated
     * from all facets currently owned by this instance.
     *
     * @return Current fault severity. The expected values:
     *         0: Ok
     *         (0, 1.0): Transient fault condition
     *         [1.0, Inf): Active fault condition
     */
    virtual double getSeverity() const = 0;

    /**
     * Gets the duration of an active fault, if any.
     * This is the time from the moment the severity reached the 1.0 value
     * and stayed on or above 1.0.
     *
     * Note: each time the severity drops below 1.0 the duration is reset.
     */
    virtual Milliseconds getActiveFaultDuration() const = 0;

    /**
     * @return The lifetime of this fault from the moment it was created.
     *         Invariant: getDuration() >= getActiveFaultDuration()
     */
    virtual Milliseconds getDuration() const = 0;

    /**
     * Describes the current fault.
     */
    virtual void appendDescription(BSONObjBuilder* builder) const = 0;
};

using FaultConstPtr = std::shared_ptr<const Fault>;

/**
 * Internal Fault interface that has accessors to manage Facets this Fault owns.
 */
class FaultInternal : public Fault, public FaultFacetsContainer {
public:
    ~FaultInternal() override = default;
};


}  // namespace process_health
}  // namespace mongo
