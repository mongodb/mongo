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

#pragma once

#include <boost/log/attributes/constant.hpp>
#include <boost/log/attributes/function.hpp>
#include <boost/log/attributes/mutable_constant.hpp>
#include <boost/log/keywords/channel.hpp>
#include <boost/log/keywords/severity.hpp>
#include <boost/log/sources/basic_logger.hpp>
#include <boost/log/sources/threading_models.hpp>

#include "mongo/logv2/attributes.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/logv2/log_truncation.h"
#include "mongo/util/time_support.h"

namespace mongo::logv2 {

// Custom logging source that automatically add our set of attributes
class LogSource : public boost::log::sources::
                      basic_logger<char, LogSource, boost::log::sources::single_thread_model> {
private:
    using Base = boost::log::sources::
        basic_logger<char, LogSource, boost::log::sources::single_thread_model>;

public:
    explicit LogSource(const LogDomain::Internal* domain, bool isShutdown)
        : _domain(domain),
          _severity(LogSeverity::Log()),
          _component(LogComponent::kDefault),
          _tags(LogTag::kNone),
          _truncation(constants::kDefaultTruncation),
          _uassertErrorCode(ErrorCodes::OK),
          _id(-1) {
        add_attribute_unlocked(attributes::domain(), _domain);
        add_attribute_unlocked(attributes::severity(), _severity);
        add_attribute_unlocked(attributes::component(), _component);
        add_attribute_unlocked(attributes::tags(), _tags);
        add_attribute_unlocked(attributes::truncation(), _truncation);
        add_attribute_unlocked(attributes::userassert(), _uassertErrorCode);
        add_attribute_unlocked(attributes::id(), _id);
        add_attribute_unlocked(attributes::timeStamp(), boost::log::attributes::make_function([]() {
                                   return Date_t::now();
                               }));
        add_attribute_unlocked(attributes::threadName(),
                               boost::log::attributes::make_function([isShutdown]() {
                                   return isShutdown ? "shutdown"_sd : getThreadName();
                               }));
    }

    explicit LogSource(const LogDomain::Internal* domain) : LogSource(domain, false) {}

    boost::log::record open_record(int32_t id,
                                   LogSeverity severity,
                                   LogComponent component,
                                   LogTag tags,
                                   LogTruncation truncation,
                                   int32_t userassertErrorCode) {
        // Perform a quick check first
        if (this->core()->get_logging_enabled()) {
            _severity.set(severity);
            _component.set(component);
            _tags.set(tags);
            _truncation.set(truncation);
            _uassertErrorCode.set(userassertErrorCode);
            _id.set(id);
            return Base::open_record_unlocked();
        } else
            return boost::log::record();
    }

    void push_record(BOOST_RV_REF(boost::log::record) rec) {
        Base::push_record_unlocked(boost::move(rec));
        _severity.set(LogSeverity::Log());
        _component.set(LogComponent::kDefault);
        _tags.set(LogTag::kNone);
        _truncation.set(constants::kDefaultTruncation);
        _uassertErrorCode.set(ErrorCodes::OK);
        _id.set(-1);
    }

private:
    boost::log::attributes::constant<const LogDomain::Internal*> _domain;
    boost::log::attributes::mutable_constant<LogSeverity> _severity;
    boost::log::attributes::mutable_constant<LogComponent> _component;
    boost::log::attributes::mutable_constant<LogTag> _tags;
    boost::log::attributes::mutable_constant<LogTruncation> _truncation;
    boost::log::attributes::mutable_constant<int32_t> _uassertErrorCode;
    boost::log::attributes::mutable_constant<int32_t> _id;
};

}  // namespace mongo::logv2
