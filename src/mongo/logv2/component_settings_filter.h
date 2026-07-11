// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/attributes.h"
#include "mongo/logv2/domain_filter.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/util/modules.h"

#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/attributes/value_extraction.hpp>

namespace mongo::logv2 {

// Boost::log filter that enables logging if Component+Severity match current settings
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ComponentSettingsFilter
    : public DomainFilter<ComponentSettingsFilter> {
public:
    ComponentSettingsFilter(const LogDomain& domain, const LogComponentSettings& settings)
        : DomainFilter(domain), _settings(settings) {}
    ComponentSettingsFilter(const LogDomain::Internal& domain, const LogComponentSettings& settings)
        : DomainFilter(domain), _settings(settings) {}
    bool filter(boost::log::attribute_value_set const& attrs) const {
        using boost::log::extract;

        return _settings.shouldLog(extract<LogComponent>(attributes::component(), attrs).get(),
                                   extract<LogSeverity>(attributes::severity(), attrs).get());
    }

private:
    const LogComponentSettings& _settings;
};

}  // namespace mongo::logv2
