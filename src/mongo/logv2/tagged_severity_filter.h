/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/attributes/value_extraction.hpp>

#include "mongo/logv2/attributes.h"
#include "mongo/logv2/domain_filter.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"

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
