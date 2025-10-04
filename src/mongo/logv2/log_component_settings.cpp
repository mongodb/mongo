/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/logv2/log_component_settings.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"

#include <mutex>

namespace mongo::logv2 {

LogComponentSettings::LogComponentSettings() {
    _minimumLoggedSeverity[LogComponent::kDefault].store(LogSeverity::Log().toInt());

    for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
        _minimumLoggedSeverity[i].store(_minimumLoggedSeverity[LogComponent::kDefault].load());
        _hasMinimumLoggedSeverity[i].store(false);
    }

    _hasMinimumLoggedSeverity[LogComponent::kDefault].store(true);
}

LogComponentSettings::~LogComponentSettings() {}

bool LogComponentSettings::hasMinimumLogSeverity(LogComponent component) const {
    dassert(int(component) >= 0 && int(component) < LogComponent::kNumLogComponents);
    return _hasMinimumLoggedSeverity[component].load();
}

LogSeverity LogComponentSettings::getMinimumLogSeverity(LogComponent component) const {
    dassert(int(component) >= 0 && int(component) < LogComponent::kNumLogComponents);
    return LogSeverity::cast(_minimumLoggedSeverity[component].load());
}

void LogComponentSettings::setMinimumLoggedSeverity(LogComponent component, LogSeverity severity) {
    dassert(int(component) >= 0 && int(component) < LogComponent::kNumLogComponents);
    stdx::lock_guard<stdx::mutex> lk(_mtx);
    _setMinimumLoggedSeverityInLock(component, severity);
}

void LogComponentSettings::_setMinimumLoggedSeverityInLock(LogComponent component,
                                                           LogSeverity severity) {
    _minimumLoggedSeverity[component].store(severity.toInt());
    _hasMinimumLoggedSeverity[component].store(true);

    // Every unconfigured component will inherit log severity from parent.
    // Traversing the severity array once works because child components always
    // come after the parent in the LogComponent::Value enumeration.
    for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
        if (!_hasMinimumLoggedSeverity[i].load()) {
            LogComponent::Value v = LogComponent::Value(i);
            LogComponent parentComponent = LogComponent(v).parent();
            LogSeverity parentSeverity = getMinimumLogSeverity(parentComponent);
            _minimumLoggedSeverity[i].store(parentSeverity.toInt());
        }
    }

    if (kDebugBuild) {
        // This loop validates the guarantee that either an element has an individual log severity
        // set or that it's value is equal to its parent's (i.e. either the value is set or
        // inherited).
        for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
            LogComponent::Value v = LogComponent::Value(i);
            LogComponent parentComponent = LogComponent(v).parent();
            invariant(_hasMinimumLoggedSeverity[i].load() ||
                      _minimumLoggedSeverity[i].load() ==
                          _minimumLoggedSeverity[parentComponent].load());
        }
    }
}

void LogComponentSettings::clearMinimumLoggedSeverity(LogComponent component) {
    dassert(int(component) >= 0 && int(component) < LogComponent::kNumLogComponents);

    stdx::lock_guard<stdx::mutex> lk(_mtx);

    // LogComponent::kDefault must always be configured.
    if (component == LogComponent::kDefault) {
        _setMinimumLoggedSeverityInLock(component, LogSeverity::Log());
        return;
    }

    // Set unconfigured severity level to match LogComponent::kDefault.
    _setMinimumLoggedSeverityInLock(component, getMinimumLogSeverity(component.parent()));
    _hasMinimumLoggedSeverity[component].store(false);
}

bool LogComponentSettings::shouldLog(LogComponent component, LogSeverity severity) const {
    dassert(int(component) >= 0 && int(component) < LogComponent::kNumLogComponents);
    return severity >= LogSeverity::cast(_minimumLoggedSeverity[component].loadRelaxed());
}

}  // namespace mongo::logv2
