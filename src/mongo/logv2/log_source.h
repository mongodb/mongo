// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/attributes.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_service.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/logv2/log_truncation.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <boost/log/attributes/constant.hpp>
#include <boost/log/attributes/function.hpp>
#include <boost/log/attributes/mutable_constant.hpp>
#include <boost/log/keywords/channel.hpp>
#include <boost/log/keywords/severity.hpp>
#include <boost/log/sources/basic_logger.hpp>
#include <boost/log/sources/threading_models.hpp>
#include <boost/move/utility_core.hpp>

namespace mongo::logv2 {
using namespace std::literals::string_view_literals;

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
          _service(LogService::defer),
          _tags(LogTag::kNone),
          _truncation(constants::kDefaultTruncation),
          _uassertErrorCode(ErrorCodes::OK),
          _id(-1) {
        add_attribute_unlocked(attributes::domain(), _domain);
        add_attribute_unlocked(attributes::severity(), _severity);
        add_attribute_unlocked(attributes::component(), _component);
        add_attribute_unlocked(attributes::service(), _service);
        add_attribute_unlocked(attributes::tags(), _tags);
        add_attribute_unlocked(attributes::truncation(), _truncation);
        add_attribute_unlocked(attributes::userassert(), _uassertErrorCode);
        add_attribute_unlocked(attributes::id(), _id);
        add_attribute_unlocked(attributes::timeStamp(), boost::log::attributes::make_function([]() {
                                   return Date_t::now();
                               }));
        add_attribute_unlocked(attributes::threadName(),
                               boost::log::attributes::make_function([isShutdown]() {
                                   return isShutdown ? "shutdown"sv : getThreadName();
                               }));
    }

    explicit LogSource(const LogDomain::Internal* domain) : LogSource(domain, false) {}

    boost::log::record open_record(int32_t id,
                                   LogSeverity severity,
                                   LogComponent component,
                                   LogService service,
                                   LogTag tags,
                                   LogTruncation truncation,
                                   int32_t userassertErrorCode) {
        // Perform a quick check first
        if (this->core()->get_logging_enabled()) {
            _severity.set(severity);
            _component.set(component);
            _service.set(service == LogService::defer ? getLogService() : service);
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
        _service.set(LogService::defer);
        _tags.set(LogTag::kNone);
        _truncation.set(constants::kDefaultTruncation);
        _uassertErrorCode.set(ErrorCodes::OK);
        _id.set(-1);
    }

private:
    boost::log::attributes::constant<const LogDomain::Internal*> _domain;
    boost::log::attributes::mutable_constant<LogSeverity> _severity;
    boost::log::attributes::mutable_constant<LogComponent> _component;
    boost::log::attributes::mutable_constant<LogService> _service;
    boost::log::attributes::mutable_constant<LogTag> _tags;
    boost::log::attributes::mutable_constant<LogTruncation> _truncation;
    boost::log::attributes::mutable_constant<int32_t> _uassertErrorCode;
    boost::log::attributes::mutable_constant<int32_t> _id;
};

}  // namespace mongo::logv2
