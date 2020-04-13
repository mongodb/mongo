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

#pragma once

#include <boost/optional/optional.hpp>

#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/logv2/log_severity.h"

namespace mongo {

inline logv2::LogSeverity getMinimumLogSeverity() {
    return logv2::LogManager::global().getGlobalSettings().getMinimumLogSeverity(
        mongo::logv2::LogComponent::kDefault);
}

inline logv2::LogSeverity getMinimumLogSeverity(logv2::LogComponent component) {
    return logv2::LogManager::global().getGlobalSettings().getMinimumLogSeverity(component);
}

inline void setMinimumLoggedSeverity(logv2::LogSeverity severity) {
    return logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
        mongo::logv2::LogComponent::kDefault, severity);
}

inline void setMinimumLoggedSeverity(logv2::LogComponent component, logv2::LogSeverity severity) {
    return logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(component,
                                                                                    severity);
}

inline void clearMinimumLoggedSeverity(logv2::LogComponent component) {
    return logv2::LogManager::global().getGlobalSettings().clearMinimumLoggedSeverity(component);
}

inline bool hasMinimumLogSeverity(logv2::LogComponent component) {
    return logv2::LogManager::global().getGlobalSettings().hasMinimumLogSeverity(component);
}

/**
 * Configure a LogComponent`s MinimumLoggedSeverity, saving the old state and restoring it
 * when this guard object dies. There can be no severity mapping for a LogComponent, so
 * the logged severity 'state' is read and written as a boost::optional.
 */
class MinimumLoggedSeverityGuard {
public:
    MinimumLoggedSeverityGuard(logv2::LogComponent component,
                               boost::optional<logv2::LogSeverity> severity)
        : _component{component}, _savedSeverity{_get()} {
        _put(severity);
    }

    ~MinimumLoggedSeverityGuard() {
        _put(_savedSeverity);
    }

private:
    boost::optional<logv2::LogSeverity> _get() {
        if (hasMinimumLogSeverity(_component))
            return getMinimumLogSeverity(_component);
        return boost::none;
    }

    void _put(boost::optional<logv2::LogSeverity> severity) {
        if (severity) {
            setMinimumLoggedSeverity(_component, *severity);
        } else {
            clearMinimumLoggedSeverity(_component);
        }
    }

    logv2::LogComponent _component;
    boost::optional<logv2::LogSeverity> _savedSeverity;
};

}  // namespace mongo
