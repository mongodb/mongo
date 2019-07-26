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

#include <boost/log/attributes/function.hpp>
#include <boost/log/attributes/mutable_constant.hpp>
#include <boost/log/keywords/channel.hpp>
#include <boost/log/keywords/severity.hpp>
#include <boost/log/sources/basic_logger.hpp>
#include <boost/log/sources/threading_models.hpp>

#include "mongo/logv2/attributes.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace logv2 {

// Custom logging source that automatically add our set of attributes
class LogSource : public boost::log::sources::
                      basic_logger<char, LogSource, boost::log::sources::single_thread_model> {
private:
private:
    typedef boost::log::sources::
        basic_logger<char, LogSource, boost::log::sources::single_thread_model>
            base_type;

public:
    LogSource() : LogSource(boost::log::core::get()) {}

    LogSource(boost::log::core_ptr core)
        : base_type(core),
          _severity(LogSeverity::Log()),
          _component(LogComponent::kDefault),
          _tags(LogTag::kNone),
          _id(StringData{}) {
        add_attribute_unlocked(attributes::severity(), _severity);
        add_attribute_unlocked(attributes::component(), _component);
        add_attribute_unlocked(attributes::tags(), _tags);
        add_attribute_unlocked(attributes::stableId(), _id);
        add_attribute_unlocked(attributes::timeStamp(), boost::log::attributes::make_function([]() {
                                   return Date_t::now();
                               }));
        add_attribute_unlocked(
            attributes::threadName(),
            boost::log::attributes::make_function([]() { return getThreadName(); }));
    }

    boost::log::record open_record(LogSeverity severity,
                                   LogComponent component,
                                   LogTag tags,
                                   StringData stable_id) {
        // Perform a quick check first
        if (this->core()->get_logging_enabled()) {
            _severity.set(severity);
            _component.set(component);
            _tags.set(tags);
            _id.set(stable_id);
            return base_type::open_record_unlocked();
        } else
            return boost::log::record();
    }

    void push_record(BOOST_RV_REF(boost::log::record) rec) {
        base_type::push_record_unlocked(boost::move(rec));
        _severity.set(LogSeverity::Log());
        _component.set(LogComponent::kDefault);
        _tags.set(LogTag::kNone);
        _id.set(StringData{});
    }

private:
    boost::log::attributes::mutable_constant<LogSeverity> _severity;
    boost::log::attributes::mutable_constant<LogComponent> _component;
    boost::log::attributes::mutable_constant<LogTag> _tags;
    boost::log::attributes::mutable_constant<StringData> _id;
};


}  // namespace logv2
}  // namespace mongo
