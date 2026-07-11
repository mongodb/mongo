// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/make_shared.hpp>

namespace mongo {
namespace logv2 {

/*
 * LogLineListener is a wrapper class used in the LogCaptureBackend that defines what to do with
 * log lines upon consumption.
 */
class [[MONGO_MOD_OPEN]] LogLineListener {
public:
    virtual ~LogLineListener() = default;
    virtual void accept(const std::string& line) = 0;
};

class [[MONGO_MOD_PUBLIC]] LogCaptureBackend
    : public boost::log::sinks::
          basic_formatted_sink_backend<char, boost::log::sinks::concurrent_feeding> {
public:
    LogCaptureBackend(std::unique_ptr<LogLineListener> logListener, bool stripEol)
        : _logListener{std::move(logListener)}, _stripEol(stripEol) {}

    static boost::shared_ptr<boost::log::sinks::unlocked_sink<LogCaptureBackend>> create(
        std::unique_ptr<LogLineListener> logListener, bool stripEol) {
        return boost::make_shared<boost::log::sinks::unlocked_sink<LogCaptureBackend>>(
            boost::make_shared<LogCaptureBackend>(std::move(logListener), stripEol));
    }

    void consume(boost::log::record_view const& rec, string_type const& formatted_string) {
        if (!_enabled.load())
            return;
        if (_stripEol.load() && !formatted_string.empty() &&
            formatted_string[formatted_string.size() - 1] == '\n') {
            _logListener->accept(formatted_string.substr(0, formatted_string.size() - 1));
        } else {
            _logListener->accept(formatted_string);
        }
    }

    void setEnabled(bool b) {
        MONGO_COMPILER_DIAGNOSTIC_PUSH
        MONGO_COMPILER_DIAGNOSTIC_WORKAROUND_ATOMIC_WRITE
        _enabled.store(b);
        MONGO_COMPILER_DIAGNOSTIC_POP
    }

private:
    std::unique_ptr<LogLineListener> _logListener;
    Atomic<bool> _enabled{true};
    Atomic<bool> _stripEol;
};
}  // namespace logv2
}  // namespace mongo
