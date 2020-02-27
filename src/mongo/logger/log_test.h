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

#include <boost/log/core.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/logger/appender.h"
#include "mongo/logger/log_severity.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/logv2_appender.h"
#include "mongo/logger/message_log_domain.h"
#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/log_capture_backend.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log_global_settings.h"

namespace mongo {

inline logger::LogSeverity getMinimumLogSeverity() {
    if (logV2Enabled())
        return logSeverityV2toV1(
            logv2::LogManager::global().getGlobalSettings().getMinimumLogSeverity(
                mongo::logv2::LogComponent::kDefault));
    return logger::globalLogDomain()->getMinimumLogSeverity();
}

inline logger::LogSeverity getMinimumLogSeverity(logger::LogComponent component) {
    if (logV2Enabled())
        return logSeverityV2toV1(
            logv2::LogManager::global().getGlobalSettings().getMinimumLogSeverity(
                logComponentV1toV2(component)));
    return logger::globalLogDomain()->getMinimumLogSeverity(component);
}

inline void setMinimumLoggedSeverity(logger::LogSeverity severity) {
    if (logV2Enabled())
        return logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            mongo::logv2::LogComponent::kDefault, mongo::logSeverityV1toV2(severity));
    logger::globalLogDomain()->setMinimumLoggedSeverity(severity);
}

inline void setMinimumLoggedSeverity(logger::LogComponent component, logger::LogSeverity severity) {
    if (logV2Enabled())
        return logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            logComponentV1toV2(component), mongo::logSeverityV1toV2(severity));
    logger::globalLogDomain()->setMinimumLoggedSeverity(component, severity);
}

inline void clearMinimumLoggedSeverity(logger::LogComponent component) {
    if (logV2Enabled())
        return logv2::LogManager::global().getGlobalSettings().clearMinimumLoggedSeverity(
            logComponentV1toV2(component));
    logger::globalLogDomain()->clearMinimumLoggedSeverity(component);
}

inline bool hasMinimumLogSeverity(logger::LogComponent component) {
    if (logV2Enabled())
        return logv2::LogManager::global().getGlobalSettings().hasMinimumLogSeverity(
            logComponentV1toV2(component));
    return logger::globalLogDomain()->hasMinimumLogSeverity(component);
}


namespace logger {


// Used for testing logging framework only.
// TODO(schwerin): Have logger write to a different log from the global log, so that tests can
// redirect their global log output for examination.
template <typename MessageEventEncoder, typename LOGV2Formatter>
class LogTest : public unittest::Test {
    friend class LogTestAppender;

public:
    LogTest() : _severityOld(getMinimumLogSeverity()) {
        globalLogDomain()->clearAppenders();
        if (logV2Enabled()) {
            _appenderHandle = globalLogDomain()->attachAppender(
                std::make_unique<LogV2Appender<MessageEventEphemeral>>(
                    &logv2::LogManager::global().getGlobalDomain(), true));

            if (!_captureSink) {
                _captureSink = logv2::LogCaptureBackend::create(_logLines);
                _captureSink->set_filter(logv2::ComponentSettingsFilter(
                    logv2::LogManager::global().getGlobalDomain(),
                    logv2::LogManager::global().getGlobalSettings()));
                _captureSink->set_formatter(LOGV2Formatter());
            }
            boost::log::core::get()->add_sink(_captureSink);
        } else {
            _appenderHandle =
                globalLogDomain()->attachAppender(std::make_unique<LogTestAppender>(this));
        }
    }

    virtual ~LogTest() {
        globalLogDomain()->detachAppender(_appenderHandle);
        if (logV2Enabled()) {
            boost::log::core::get()->remove_sink(_captureSink);
        }
        setMinimumLoggedSeverity(_severityOld);
    }

protected:
    std::vector<std::string> _logLines;
    LogSeverity _severityOld;

private:
    class LogTestAppender : public MessageLogDomain::EventAppender {
    public:
        explicit LogTestAppender(LogTest* ltest) : _ltest(ltest) {}
        virtual ~LogTestAppender() {}
        virtual Status append(const MessageLogDomain::Event& event) {
            std::ostringstream _os;
            if (!_encoder.encode(event, _os))
                return Status(ErrorCodes::LogWriteFailed, "Failed to append to LogTestAppender.");
            _ltest->_logLines.push_back(_os.str());
            return Status::OK();
        }

    private:
        LogTest* _ltest;
        MessageEventEncoder _encoder;
    };

    MessageLogDomain::AppenderHandle _appenderHandle;
    boost::shared_ptr<boost::log::sinks::synchronous_sink<logv2::LogCaptureBackend>> _captureSink;
};

}  // namespace logger
}  // namespace mongo
