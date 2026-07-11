// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo::unittest {

namespace log_test_detail {

using logv2::LogComponent;
using logv2::LogSeverity;

inline auto& _settings() {
    return logv2::LogManager::global().getGlobalSettings();
}

inline bool hasMinimumLogSeverity(LogComponent component) {
    return _settings().hasMinimumLogSeverity(component);
}

inline LogSeverity getMinimumLogSeverity(LogComponent component) {
    return _settings().getMinimumLogSeverity(component);
}

inline LogSeverity getMinimumLogSeverity() {
    return getMinimumLogSeverity(LogComponent::kDefault);
}

inline void setMinimumLoggedSeverity(LogComponent component, LogSeverity severity) {
    return _settings().setMinimumLoggedSeverity(component, severity);
}

inline void setMinimumLoggedSeverity(LogSeverity severity) {
    return setMinimumLoggedSeverity(LogComponent::kDefault, severity);
}

inline void clearMinimumLoggedSeverity(LogComponent component) {
    return _settings().clearMinimumLoggedSeverity(component);
}

}  // namespace log_test_detail

using log_test_detail::clearMinimumLoggedSeverity;
using log_test_detail::getMinimumLogSeverity;
using log_test_detail::hasMinimumLogSeverity;
using log_test_detail::setMinimumLoggedSeverity;

/**
 * Configure a LogComponent`s MinimumLoggedSeverity, saving the old state and restoring it
 * when this guard object dies. There can be no severity mapping for a LogComponent, so
 * the logged severity 'state' is read and written as a boost::optional.
 */
class [[MONGO_MOD_PUBLIC]] MinimumLoggedSeverityGuard {
public:
    /** Just save and restore: do not change the severity at ctor time. */
    explicit MinimumLoggedSeverityGuard(logv2::LogComponent component)
        : _component{component}, _savedSeverity{_get()} {}

    /** Change the `component` to have `severity`. */
    MinimumLoggedSeverityGuard(logv2::LogComponent component,
                               boost::optional<logv2::LogSeverity> severity)
        : MinimumLoggedSeverityGuard{component} {
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

}  // namespace mongo::unittest
