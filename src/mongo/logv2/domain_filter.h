// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/attributes.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/util/modules.h"

#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/attributes/value_extraction.hpp>

namespace mongo::logv2 {

// Boost::log filter that enables logging if domain match. Using CRTP, users should inherit from
// this and provide the concrete type as the template argument to this class.
template <class Filter>
class [[MONGO_MOD_PUBLIC]] DomainFilter {
public:
    explicit DomainFilter(const LogDomain& domain) : DomainFilter(domain.internal()) {}
    explicit DomainFilter(const LogDomain::Internal& domain) : _domain(&domain) {}

    bool operator()(boost::log::attribute_value_set const& attrs) {
        using boost::log::extract;

        return extract<const LogDomain::Internal*>(attributes::domain(), attrs).get() == _domain &&
            static_cast<const Filter*>(this)->filter(attrs);
    }

private:
    const LogDomain::Internal* _domain;
};

class [[MONGO_MOD_PUBLIC]] AllLogsFilter : public DomainFilter<AllLogsFilter> {
public:
    using DomainFilter::DomainFilter;

    bool filter(boost::log::attribute_value_set const& attrs) const {
        return true;
    }
};

}  // namespace mongo::logv2
