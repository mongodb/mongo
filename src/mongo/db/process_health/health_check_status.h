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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace process_health {

/**
 * All fault types we support in this package.
 */
enum class FaultFacetType { kMock = 0 };

/**
 * The immutable class representing current status of an ongoing fault tracked by facet.
 */
class HealthCheckStatus {
public:
    HealthCheckStatus(FaultFacetType type, double severity, StringData description)
        : _type(type), _severity(severity), _description(description) {}

    HealthCheckStatus(const HealthCheckStatus&) = default;
    HealthCheckStatus& operator=(const HealthCheckStatus&) = default;

    /**
     * @return FaultFacetType of this status.
     */
    FaultFacetType getType() const {
        return _type;
    }

    /**
     * The fault severity value if any.
     *
     * @return Current fault severity. The expected values:
     *         0: Ok
     *         (0, 1.0): Transient fault condition
     *         [1.0, Inf): Active fault condition
     */
    double getSeverity() const {
        return _severity;
    }

    void appendDescription(BSONObjBuilder* builder) const {}

private:
    const FaultFacetType _type;
    const double _severity;
    const std::string _description;
};

}  // namespace process_health
}  // namespace mongo
