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

#include <ostream>

#include "mongo/db/process_health/health_monitoring_server_parameters_gen.h"
#include "mongo/platform/basic.h"
#include "mongo/util/duration.h"


namespace mongo {
namespace process_health {

/**
 * Current fault state of the server in a simple actionable form.
 */
enum class FaultState {
    kOk = 0,

    // The manager conducts startup checks, new connections should be refused.
    kStartupCheck,

    // The manager detected a fault, however the fault is either not severe
    // enough or is not observed for sufficiently long period of time.
    kTransientFault,

    // The manager detected a severe fault, which made the server unusable.
    kActiveFault
};


StringBuilder& operator<<(StringBuilder& s, const FaultState& state);
std::ostream& operator<<(std::ostream& os, const FaultState& state);


/**
 * Types of health observers available.
 */
enum class FaultFacetType { kMock1 = 0, kMock2, kLdap, kDns };


class FaultManagerConfig {
public:
    static auto inline constexpr kPeriodicHealthCheckInterval{Milliseconds(50)};

    HealthObserverIntensityEnum getHealthObserverIntensity(FaultFacetType type) {
        auto intensities = getHealthObserverIntensities();
        switch (type) {
            case FaultFacetType::kLdap:
                return intensities->_data->getLdap();
            case FaultFacetType::kDns:
                return intensities->_data->getDns();
            // TODO: update this function with additional fault facets when they are added
            case FaultFacetType::kMock1:
                return HealthObserverIntensityEnum::kCritical;
            case FaultFacetType::kMock2:
                return HealthObserverIntensityEnum::kCritical;
            default:
                MONGO_UNREACHABLE;
        }
    }

    bool isHealthObserverEnabled(FaultFacetType type) {
        return getHealthObserverIntensity(type) != HealthObserverIntensityEnum::kOff;
    }

    Milliseconds getActiveFaultDuration() const {
        return _activeFaultDuration;
    }

    Milliseconds getPeriodicHealthCheckInterval() const {
        return kPeriodicHealthCheckInterval;
    }

    Milliseconds getPeriodicLivenessCheckInterval() const {
        return Milliseconds(50);
    }

    Seconds getPeriodicLivenessDeadline() const {
        return Seconds(300);
    }

    /** @returns true if the periodic checks are disabled for testing purposes. This is
     *    always false in production.
     */
    bool periodicChecksDisabledForTests() const {
        return _periodicChecksDisabledForTests;
    }

    void disablePeriodicChecksForTests() {
        _periodicChecksDisabledForTests = true;
    }

    void setActiveFaultDurationForTests(Milliseconds duration) {
        _activeFaultDuration = duration;
    }

protected:
    // If the server persists in TransientFault for more than this duration
    // it will move to the ActiveFault state and terminate.
    static inline const auto kActiveFaultDuration = Seconds(120);

private:
    static HealthMonitoringIntensitiesServerParameter* getHealthObserverIntensities() {
        return ServerParameterSet::getGlobal()->get<HealthMonitoringIntensitiesServerParameter>(
            "healthMonitoring");
    }

    bool _periodicChecksDisabledForTests = false;
    Milliseconds _activeFaultDuration = kActiveFaultDuration;
};

}  // namespace process_health
}  // namespace mongo
