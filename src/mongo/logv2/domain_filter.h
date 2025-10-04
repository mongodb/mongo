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

#include "mongo/logv2/attributes.h"
#include "mongo/logv2/log_domain.h"

#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/attributes/value_extraction.hpp>

namespace mongo::logv2 {

// Boost::log filter that enables logging if domain match. Using CRTP, users should inherit from
// this and provide the concrete type as the template argument to this class.
template <class Filter>
class DomainFilter {
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

class AllLogsFilter : public DomainFilter<AllLogsFilter> {
public:
    using DomainFilter::DomainFilter;

    bool filter(boost::log::attribute_value_set const& attrs) const {
        return true;
    }
};

}  // namespace mongo::logv2
