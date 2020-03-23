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

#include "mongo/base/status.h"
#include "mongo/logger/appender.h"
#include "mongo/logger/log_version_util.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_options.h"
#include "mongo/logv2/log_tag.h"

namespace mongo {
namespace logger {


namespace {

auto findTeeTag(StringData teeName) {
    static constexpr std::pair<StringData, logv2::LogTag::Value> kTees[] = {
        {"rs"_sd, logv2::LogTag::kRS},
        {"startupWarnings"_sd, logv2::LogTag::kStartupWarnings},
    };
    if (teeName.empty())
        return logv2::LogTag::kNone;
    for (auto&& e : kTees)
        if (e.first == teeName)
            return e.second;
    MONGO_UNREACHABLE;
}

}  // namespace

/**
 * Appender for writing to a logv2 domain
 */
template <typename Event>
class LogV2Appender : public Appender<Event> {
public:
    LogV2Appender(const LogV2Appender&) = delete;
    LogV2Appender& operator=(const LogV2Appender&) = delete;

    explicit LogV2Appender(logv2::LogDomain* domain,
                           bool warnOnLargeMessages,
                           logv2::LogTag extraTag = logv2::LogTag::kNone)
        : _domain(domain), _tag(extraTag), _warnOnLargeMessages(warnOnLargeMessages) {}

    Status append(const Event& event) override {

        auto logTagValue = findTeeTag(event.getTeeName());
        auto message = event.getMessage();
        if (message.endsWith("\n")) {
            message = message.substr(0, message.size() - 1);
        }
        size_t maxSizeKB = MessageEventDetailsEncoder::getMaxLogSizeKB();

        if (_warnOnLargeMessages && event.isTruncatable() && message.size() > maxSizeKB * 1024) {
            logv2::detail::doLog(
                0,
                logv2::LogSeverity::cast(event.getSeverity().toInt()),
                logv2::LogOptions{
                    logComponentV1toV2(event.getComponent()),
                    _domain,
                    logv2::LogTag{static_cast<logv2::LogTag::Value>(
                        static_cast<std::underlying_type_t<logv2::LogTag::Value>>(logTagValue) |
                        static_cast<std::underlying_type_t<logv2::LogTag::Value>>(_tag))}},

                "warning: log line attempted ({}kB) over max size ({}kB)",
                "size"_attr = message.size() / 1024,
                "maxSize"_attr = maxSizeKB);
        }

        logv2::detail::doLog(
            0,
            // We need to cast from the v1 logging severity to the equivalent v2 severity
            logv2::LogSeverity::cast(event.getSeverity().toInt()),

            // Similarly, we need to transcode the options. They don't offer a cast
            // operator, so we need to do some metaprogramming on the types.
            logv2::LogOptions{
                logComponentV1toV2(event.getComponent()),
                _domain,
                logv2::LogTag{static_cast<logv2::LogTag::Value>(
                    static_cast<std::underlying_type_t<logv2::LogTag::Value>>(logTagValue) |
                    static_cast<std::underlying_type_t<logv2::LogTag::Value>>(_tag))},
                event.isTruncatable() ? logv2::LogTruncation::Enabled
                                      : logv2::LogTruncation::Disabled,
                logv2::FatalMode::kContinue},

            "{}",
            "message"_attr = message);
        return Status::OK();
    }

private:
    logv2::LogDomain* _domain;
    logv2::LogTag _tag;
    bool _warnOnLargeMessages;
};

}  // namespace logger
}  // namespace mongo
