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
    // The manager conducts startup checks, new connections should be refused.
    kStartupCheck = 0,

    kOk,

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
enum class FaultFacetType { kSystem, kMock1, kMock2, kTestObserver, kLdap, kDns };
static const StringData FaultFacetTypeStrings[] = {
    "kSystem", "kMock1", "kMock2", "kTestObserver", "kLdap", "kDns"};

static const StringData FaultFacetType_serializer(const FaultFacetType value) {
    return FaultFacetTypeStrings[static_cast<int>(value)];
}

inline StringBuilder& operator<<(StringBuilder& s, const FaultFacetType& type) {
    return s << FaultFacetType_serializer(type);
}

inline std::ostream& operator<<(std::ostream& os, const FaultFacetType& type) {
    StringBuilder sb;
    sb << type;
    os << sb.stringData();
    return os;
}

class FaultManagerConfig {
public:
    /* Maximum possible jitter added to the time between health checks */
    static auto inline constexpr kPeriodicHealthCheckMaxJitter{Milliseconds{100}};

    HealthObserverIntensityEnum getHealthObserverIntensity(FaultFacetType type) {
        auto intensities = _getHealthObserverIntensities();
        if (type == FaultFacetType::kMock1 && _facetToIntensityMapForTest.contains(type)) {
            return _facetToIntensityMapForTest.at(type);
        }
        return _getPropertyByType(
            type, &intensities->_data, HealthObserverIntensityEnum::kCritical);
    }

    bool isHealthObserverEnabled(FaultFacetType type) {
        return getHealthObserverIntensity(type) != HealthObserverIntensityEnum::kOff;
    }

    void setIntensityForType(FaultFacetType type, HealthObserverIntensityEnum intensity) {
        _facetToIntensityMapForTest.insert({type, intensity});
    }

    // If the server persists in TransientFault for more than this duration
    // it will move to the ActiveFault state and terminate.
    Milliseconds getActiveFaultDuration() const {
        return Milliseconds(Seconds(mongo::gActiveFaultDurationSecs.load()));
    }

    Milliseconds getPeriodicHealthCheckInterval(FaultFacetType type) const {
        auto intervals = _getHealthObserverIntervals();
        return Milliseconds(_getPropertyByType(type, &intervals->_data, 1000));
    }

    Milliseconds getPeriodicLivenessCheckInterval() const {
        return Milliseconds(_getLivenessConfig()->_data->getInterval());
    }

    Seconds getPeriodicLivenessDeadline() const {
        return Seconds(_getLivenessConfig()->_data->getDeadline());
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

private:
    static HealthMonitoringIntensitiesServerParameter* _getHealthObserverIntensities() {
        return ServerParameterSet::getGlobal()->get<HealthMonitoringIntensitiesServerParameter>(
            "healthMonitoringIntensities");
    }

    static PeriodicHealthCheckIntervalsServerParameter* _getHealthObserverIntervals() {
        return ServerParameterSet::getGlobal()->get<PeriodicHealthCheckIntervalsServerParameter>(
            "healthMonitoringIntervals");
    }

    static HealthMonitoringProgressMonitorServerParameter* _getLivenessConfig() {
        return ServerParameterSet::getGlobal()->get<HealthMonitoringProgressMonitorServerParameter>(
            "progressMonitor");
    }

    template <typename T, typename R>
    R _getPropertyByType(FaultFacetType type, synchronized_value<T>* data, R defaultValue) const {
        switch (type) {
            case FaultFacetType::kLdap:
                return (*data)->getLdap();
            case FaultFacetType::kDns:
                return (*data)->getDns();
            case FaultFacetType::kTestObserver:
                return (*data)->getTest();
            case FaultFacetType::kSystem:
                return defaultValue;
            case FaultFacetType::kMock1:
                return defaultValue;
            case FaultFacetType::kMock2:
                return defaultValue;
            // TODO: update this function with additional fault facets when they are added
            default:
                MONGO_UNREACHABLE;
        }
    }

    bool _periodicChecksDisabledForTests = false;

    stdx::unordered_map<FaultFacetType, HealthObserverIntensityEnum> _facetToIntensityMapForTest;
};

}  // namespace process_health
}  // namespace mongo
