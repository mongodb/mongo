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

#ifndef _WIN32  // TODO(schwerin): Should be #if MONGO_CONFIG_HAVE_SYSLOG_H?

#include <sstream>
#include <syslog.h>

#include "mongo/base/status.h"
#include "mongo/logger/appender.h"
#include "mongo/logger/encoder.h"

namespace mongo {
namespace logger {

/**
 * Appender for writing to syslog.  Users must have separately called openlog().
 */
template <typename Event>
class SyslogAppender : public Appender<Event> {
    SyslogAppender(const SyslogAppender&) = delete;
    SyslogAppender& operator=(const SyslogAppender&) = delete;

public:
    typedef Encoder<Event> EventEncoder;

    // TODO: Remove this ctor once raw pointer use is gone
    explicit SyslogAppender(EventEncoder* encoder) : _encoder(encoder) {}
    explicit SyslogAppender(std::unique_ptr<EventEncoder> encoder) : _encoder(std::move(encoder)) {}
    virtual Status append(const Event& event) {
        std::ostringstream os;
        _encoder->encode(event, os);
        if (!os)
            return Status(ErrorCodes::LogWriteFailed, "Error writing log message to syslog.");
        syslog(getSyslogPriority(event.getSeverity()), "%s", os.str().c_str());
        return Status::OK();
    }

private:
    int getSyslogPriority(logv2::LogSeverity severity) {
        if (severity <= logv2::LogSeverity::Debug(1))
            return LOG_DEBUG;
        if (severity == logv2::LogSeverity::Warning())
            return LOG_WARNING;
        if (severity == logv2::LogSeverity::Error())
            return LOG_ERR;
        if (severity >= logv2::LogSeverity::Severe())
            return LOG_CRIT;
        // Info() and Log().
        return LOG_INFO;
    }
    std::unique_ptr<EventEncoder> _encoder;
};

}  // namespace logger
}  // namespace mongo

#endif
