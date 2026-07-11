// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/attributes.h"
#include "mongo/logv2/domain_filter.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/util/modules.h"

#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/attributes/value_extraction.hpp>

namespace mongo::logv2 {

// Boost::log filter that enables logging if Tag exists with Severity over threshold
class TaggedSeverityFilter : public DomainFilter<TaggedSeverityFilter> {
public:
    TaggedSeverityFilter(const LogDomain& domain, LogTag tag, LogSeverity severity)
        : DomainFilter(domain), _tag(tag), _severity(severity) {}
    TaggedSeverityFilter(const LogDomain::Internal& domain, LogTag tag, LogSeverity severity)
        : DomainFilter(domain), _tag(tag), _severity(severity) {}
    bool filter(boost::log::attribute_value_set const& attrs) const {
        using boost::log::extract;

        return _tag.has(extract<LogTag>(attributes::tags(), attrs).get()) &&
            extract<LogSeverity>(attributes::severity(), attrs).get() >= _severity;
    }

private:
    LogTag _tag;
    LogSeverity _severity;
};

}  // namespace mongo::logv2
