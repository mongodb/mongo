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
enum class FaultFacetType { kSystem, kMock1, kMock2, kTestObserver, kLdap, kDns, kConfigServer };
static const StringData FaultFacetTypeStrings[] = {
    "systemObserver", "mock1", "mock2", "testObserver", "LDAP", "DNS", "configServer"};

FaultFacetType toFaultFacetType(HealthObserverTypeEnum type);


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

    static constexpr auto toObserverType =
        [](FaultFacetType type) -> boost::optional<HealthObserverTypeEnum> {
        switch (type) {
            case FaultFacetType::kLdap:
                return HealthObserverTypeEnum::kLdap;
            case FaultFacetType::kDns:
                return HealthObserverTypeEnum::kDns;
            case FaultFacetType::kConfigServer:
                return HealthObserverTypeEnum::kConfigServer;
            case FaultFacetType::kTestObserver:
                return HealthObserverTypeEnum::kTest;
            default:
                return boost::none;
        }
    };

    HealthObserverIntensityEnum getHealthObserverIntensity(FaultFacetType type) const {
        auto intensities = _getHealthObserverIntensities();

        auto getIntensity = [this, intensities](FaultFacetType type) {
            auto observerType = toObserverType(type);
            if (observerType) {
                stdx::lock_guard lock(_mutex);
                if (_facetToIntensityMapForTest.contains(type)) {
                    return _facetToIntensityMapForTest.at(type);
                }

                auto x = intensities->_data->getValues();
                if (x) {
                    for (auto setting : *x) {
                        if (setting.getType() == observerType) {
                            return setting.getIntensity();
                        }
                    }
                }
                return HealthObserverIntensityEnum::kOff;
            } else {
                // TODO SERVER-61944: this is for kMock1 & kMock2. Remove this branch once mock
                // types are deleted.
                stdx::lock_guard lock(_mutex);
                if (_facetToIntensityMapForTest.contains(type)) {
                    return _facetToIntensityMapForTest.at(type);
                }
                return HealthObserverIntensityEnum::kCritical;
            }
        };

        return getIntensity(type);
    }

    bool isHealthObserverEnabled(FaultFacetType type) const {
        return getHealthObserverIntensity(type) != HealthObserverIntensityEnum::kOff;
    }

    void setIntensityForType(FaultFacetType type, HealthObserverIntensityEnum intensity) {
        stdx::lock_guard lock(_mutex);
        _facetToIntensityMapForTest.insert({type, intensity});
    }

    // If the server persists in TransientFault for more than this duration
    // it will move to the ActiveFault state and terminate.
    Milliseconds getActiveFaultDuration() const {
        return Seconds(mongo::gActiveFaultDurationSecs.load());
    }

    Milliseconds getPeriodicHealthCheckInterval(FaultFacetType type) const {
        auto intervals = _getHealthObserverIntervals();
        // TODO(SERVER-62125): replace with unified type from IDL.
        const auto convertedType = toObserverType(type);
        if (convertedType) {
            const auto values = intervals->_data->getValues();
            if (values) {
                const auto intervalIt =
                    std::find_if(values->begin(), values->end(), [&](const auto& v) {
                        return v.getType() == *convertedType;
                    });
                if (intervalIt != values->end()) {
                    return Milliseconds(intervalIt->getInterval());
                }
            }
        }
        return _getDefaultObserverInterval(type);
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

    static Milliseconds _getDefaultObserverInterval(FaultFacetType type);

    template <typename T, typename R>
    R _getPropertyByType(FaultFacetType type, synchronized_value<T>* data, R defaultValue) const {
        boost::optional<R> result;
        switch (type) {
            case FaultFacetType::kLdap:
                result = (*data)->getLdap();
                break;
            case FaultFacetType::kDns:
                result = (*data)->getDns();
                break;
            case FaultFacetType::kTestObserver:
                result = (*data)->getTest();
                break;
            case FaultFacetType::kConfigServer:
                result = (*data)->getConfigServer();
                break;
            case FaultFacetType::kSystem:
                result = defaultValue;
                break;
            case FaultFacetType::kMock1:
                result = defaultValue;
                break;
            case FaultFacetType::kMock2:
                result = defaultValue;
                break;
            default:
                MONGO_UNREACHABLE;
        }
        return *result;
    }

    bool _periodicChecksDisabledForTests = false;

    stdx::unordered_map<FaultFacetType, HealthObserverIntensityEnum> _facetToIntensityMapForTest;
    mutable Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(5), "FaultManagerConfig::_mutex");
};

}  // namespace process_health
}  // namespace mongo
