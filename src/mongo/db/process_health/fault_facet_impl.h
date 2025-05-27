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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/process_health/fault_facet.h"
#include "mongo/db/process_health/fault_manager_config.h"
#include "mongo/db/process_health/health_check_status.h"
#include "mongo/db/process_health/health_observer.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/time_support.h"

#include <string>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace process_health {

// Internal representation of the Facet.
class FaultFacetImpl : public FaultFacet {
public:
    FaultFacetImpl(FaultFacetType type, ClockSource* clockSource, HealthCheckStatus status);

    ~FaultFacetImpl() override = default;

    // Public interface methods.

    FaultFacetType getType() const override;

    HealthCheckStatus getStatus() const override;

    Milliseconds getDuration() const override;

    void update(HealthCheckStatus status) override;

    void appendDescription(BSONObjBuilder* builder) const override;

private:
    const FaultFacetType _type;
    ClockSource* const _clockSource;

    const Date_t _startTime = _clockSource->now();

    mutable stdx::mutex _mutex;
    Severity _severity = Severity::kOk;
    std::string _description;
};

}  // namespace process_health
}  // namespace mongo
