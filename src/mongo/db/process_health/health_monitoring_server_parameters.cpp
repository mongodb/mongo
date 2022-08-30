/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include <algorithm>

#include "mongo/bson/json.h"
#include "mongo/db/process_health/fault_manager.h"
#include "mongo/db/process_health/health_monitoring_server_parameters_gen.h"
#include "mongo/db/process_health/health_observer.h"


namespace mongo {

namespace {
// Replaces values in oldIntensities/Intervals with values in newIntensities/Intervals while
// preserving all values present in old- that are not present in new-.
template <typename ConfigValues>
ConfigValues mergeConfigValues(const ConfigValues& oldValues, const ConfigValues& newValues) {
    using namespace std;
    ConfigValues result = oldValues;
    auto optionalOldValues = result.getValues();
    auto optionalNewValues = newValues.getValues();
    if (!optionalNewValues) {
        return oldValues;
    }
    if (!optionalOldValues) {
        result.setValues(*optionalNewValues);
        return result;
    }
    for (const auto& setting : *optionalNewValues) {
        auto it = find_if(begin(*optionalOldValues),
                          end(*optionalOldValues),
                          [&setting](const auto& destSetting) {
                              return (destSetting.getType() == setting.getType()) ? true : false;
                          });
        if (it != optionalOldValues->end()) {
            *it = setting;
        } else {
            optionalOldValues->emplace_back(setting);
        }
    }
    result.setValues(*optionalOldValues);
    return result;
}
}  // namespace

Status HealthMonitoringIntensitiesServerParameter::setFromString(StringData value,
                                                                 const boost::optional<TenantId>&) {
    const auto oldValue = **_data;
    auto newValue = HealthObserverIntensities::parse(
        IDLParserContext("health monitoring intensities"), fromjson(value));
    newValue = mergeConfigValues(oldValue, newValue);
    **_data = newValue;
    process_health::FaultManager::healthMonitoringIntensitiesUpdated(oldValue, newValue);
    return Status::OK();
}

Status HealthMonitoringIntensitiesServerParameter::set(const BSONElement& newValueElement,
                                                       const boost::optional<TenantId>&) {
    const auto oldValue = **_data;
    auto newValue = HealthObserverIntensities::parse(
        IDLParserContext("health monitoring intensities"), newValueElement.Obj());
    newValue = mergeConfigValues(oldValue, newValue);
    **_data = newValue;
    process_health::FaultManager::healthMonitoringIntensitiesUpdated(oldValue, newValue);
    return Status::OK();
}

void HealthMonitoringIntensitiesServerParameter::append(OperationContext*,
                                                        BSONObjBuilder* b,
                                                        StringData name,
                                                        const boost::optional<TenantId>&) {
    BSONObjBuilder healthMonitoring;
    _data->serialize(&healthMonitoring);
    b->append(name, healthMonitoring.obj());
}

Status HealthMonitoringProgressMonitorServerParameter::setFromString(
    StringData value, const boost::optional<TenantId>&) {
    *_data = HealthObserverProgressMonitorConfig::parse(
        IDLParserContext("health monitoring liveness"), fromjson(value));
    return Status::OK();
}

Status HealthMonitoringProgressMonitorServerParameter::set(const BSONElement& newValueElement,
                                                           const boost::optional<TenantId>&) {
    *_data = HealthObserverProgressMonitorConfig::parse(
        IDLParserContext("health monitoring liveness"), newValueElement.Obj());
    return Status::OK();
}

void HealthMonitoringProgressMonitorServerParameter::append(OperationContext*,
                                                            BSONObjBuilder* b,
                                                            StringData name,
                                                            const boost::optional<TenantId>&) {
    BSONObjBuilder healthMonitoring;
    _data->serialize(&healthMonitoring);
    b->append(name, healthMonitoring.obj());
}

Status PeriodicHealthCheckIntervalsServerParameter::setFromString(
    StringData value, const boost::optional<TenantId>&) {
    const auto oldValue = **_data;
    auto newValue = HealthObserverIntervals::parse(IDLParserContext("health monitoring interval"),
                                                   fromjson(value));
    newValue = mergeConfigValues(oldValue, newValue);
    **_data = newValue;
    return Status::OK();
}

Status PeriodicHealthCheckIntervalsServerParameter::set(const BSONElement& newValueElement,
                                                        const boost::optional<TenantId>&) {
    const auto oldValue = **_data;
    auto newValue = HealthObserverIntervals::parse(IDLParserContext("health monitoring interval"),
                                                   newValueElement.Obj());
    newValue = mergeConfigValues(oldValue, newValue);
    **_data = newValue;
    return Status::OK();
}

void PeriodicHealthCheckIntervalsServerParameter::append(OperationContext*,
                                                         BSONObjBuilder* b,
                                                         StringData name,
                                                         const boost::optional<TenantId>&) {
    BSONObjBuilder healthMonitoring;
    _data->serialize(&healthMonitoring);
    b->append(name, healthMonitoring.obj());
}

}  // namespace mongo
