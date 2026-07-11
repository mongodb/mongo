// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/process_health/fault_manager.h"
#include "mongo/db/process_health/health_monitoring_server_parameters_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/synchronized_value.h"

#include <algorithm>
#include <iosfwd>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>


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

Status HealthMonitoringIntensitiesServerParameter::setFromString(std::string_view value,
                                                                 const boost::optional<TenantId>&) {
    const auto oldValue = **_data;
    auto newValue = HealthObserverIntensities::parse(
        fromjson(value), IDLParserContext("health monitoring intensities"));
    newValue = mergeConfigValues(oldValue, newValue);
    **_data = newValue;
    process_health::FaultManager::healthMonitoringIntensitiesUpdated(oldValue, newValue);
    return Status::OK();
}

Status HealthMonitoringIntensitiesServerParameter::set(const BSONElement& newValueElement,
                                                       const boost::optional<TenantId>&) {
    const auto oldValue = **_data;
    auto newValue = HealthObserverIntensities::parse(
        newValueElement.Obj(), IDLParserContext("health monitoring intensities"));
    newValue = mergeConfigValues(oldValue, newValue);
    **_data = newValue;
    process_health::FaultManager::healthMonitoringIntensitiesUpdated(oldValue, newValue);
    return Status::OK();
}

void HealthMonitoringIntensitiesServerParameter::append(OperationContext*,
                                                        BSONObjBuilder* b,
                                                        std::string_view name,
                                                        const boost::optional<TenantId>&) {
    BSONObjBuilder healthMonitoring;
    _data->serialize(&healthMonitoring);
    b->append(name, healthMonitoring.obj());
}

Status HealthMonitoringProgressMonitorServerParameter::setFromString(
    std::string_view value, const boost::optional<TenantId>&) {
    *_data = HealthObserverProgressMonitorConfig::parse(
        fromjson(value), IDLParserContext("health monitoring liveness"));
    return Status::OK();
}

Status HealthMonitoringProgressMonitorServerParameter::set(const BSONElement& newValueElement,
                                                           const boost::optional<TenantId>&) {
    *_data = HealthObserverProgressMonitorConfig::parse(
        newValueElement.Obj(), IDLParserContext("health monitoring liveness"));
    return Status::OK();
}

void HealthMonitoringProgressMonitorServerParameter::append(OperationContext*,
                                                            BSONObjBuilder* b,
                                                            std::string_view name,
                                                            const boost::optional<TenantId>&) {
    BSONObjBuilder healthMonitoring;
    _data->serialize(&healthMonitoring);
    b->append(name, healthMonitoring.obj());
}

Status PeriodicHealthCheckIntervalsServerParameter::setFromString(
    std::string_view value, const boost::optional<TenantId>&) {
    const auto oldValue = **_data;
    auto newValue = HealthObserverIntervals::parse(fromjson(value),
                                                   IDLParserContext("health monitoring interval"));
    newValue = mergeConfigValues(oldValue, newValue);
    **_data = newValue;
    return Status::OK();
}

Status PeriodicHealthCheckIntervalsServerParameter::set(const BSONElement& newValueElement,
                                                        const boost::optional<TenantId>&) {
    const auto oldValue = **_data;
    auto newValue = HealthObserverIntervals::parse(newValueElement.Obj(),
                                                   IDLParserContext("health monitoring interval"));
    newValue = mergeConfigValues(oldValue, newValue);
    **_data = newValue;
    return Status::OK();
}

void PeriodicHealthCheckIntervalsServerParameter::append(OperationContext*,
                                                         BSONObjBuilder* b,
                                                         std::string_view name,
                                                         const boost::optional<TenantId>&) {
    BSONObjBuilder healthMonitoring;
    _data->serialize(&healthMonitoring);
    b->append(name, healthMonitoring.obj());
}

}  // namespace mongo
