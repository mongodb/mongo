/*    Copyright 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/logger/log_component_settings.h"

#include "mongo/util/assert_util.h"

namespace mongo {
namespace logger {

LogComponentSettings::LogComponentSettings() {
    _minimumLoggedSeverity[LogComponent::kDefault] = LogSeverity::Log().toInt();

    for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
        _minimumLoggedSeverity[i] = _minimumLoggedSeverity[LogComponent::kDefault];
        _hasMinimumLoggedSeverity[i] = false;
    }

    _hasMinimumLoggedSeverity[LogComponent::kDefault] = true;
}

LogComponentSettings::~LogComponentSettings() {}

bool LogComponentSettings::hasMinimumLogSeverity(LogComponent component) const {
    dassert(int(component) >= 0 && int(component) < LogComponent::kNumLogComponents);
    return _hasMinimumLoggedSeverity[component];
}

LogSeverity LogComponentSettings::getMinimumLogSeverity(LogComponent component) const {
    dassert(int(component) >= 0 && int(component) < LogComponent::kNumLogComponents);
    return LogSeverity::cast(_minimumLoggedSeverity[component]);
}

void LogComponentSettings::setMinimumLoggedSeverity(LogComponent component, LogSeverity severity) {
    dassert(int(component) >= 0 && int(component) < LogComponent::kNumLogComponents);
    _minimumLoggedSeverity[component] = severity.toInt();
    _hasMinimumLoggedSeverity[component] = true;

    // Every unconfigured component will inherit log severity from parent.
    // Traversing the severity array once works because child components always
    // come after the parent in the LogComponent::Value enumeration.
    for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
        if (!_hasMinimumLoggedSeverity[i]) {
            LogComponent::Value v = LogComponent::Value(i);
            LogComponent parentComponent = LogComponent(v).parent();
            LogSeverity parentSeverity = getMinimumLogSeverity(parentComponent);
            _minimumLoggedSeverity[i] = parentSeverity.toInt();
        }
    }
}

void LogComponentSettings::clearMinimumLoggedSeverity(LogComponent component) {
    dassert(int(component) >= 0 && int(component) < LogComponent::kNumLogComponents);

    // LogComponent::kDefault must always be configured.
    if (component == LogComponent::kDefault) {
        setMinimumLoggedSeverity(component, LogSeverity::Log());
        return;
    }

    // Set unconfigured severity level to match LogComponent::kDefault.
    setMinimumLoggedSeverity(component, getMinimumLogSeverity(component.parent()));
    _hasMinimumLoggedSeverity[component] = false;
}

bool LogComponentSettings::shouldLog(LogComponent component, LogSeverity severity) const {
    dassert(int(component) >= 0 && int(component) < LogComponent::kNumLogComponents);

    // Should match parent component if minimum severity level is not configured for
    // component.
    dassert(_hasMinimumLoggedSeverity[component] ||
            _minimumLoggedSeverity[component] == _minimumLoggedSeverity[component.parent()]);

    return severity >= LogSeverity::cast(_minimumLoggedSeverity[component]);
}

}  // logger
}  // mongo
