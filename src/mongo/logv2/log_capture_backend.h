/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/atomic_word.h"

#include <string>
#include <vector>

#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/make_shared.hpp>

namespace mongo::logv2 {

/*
 * LogLineListener is a wrapper class used in the LogCaptureBackend that defines what to do with
 * log lines upon consumption.
 */
class LogLineListener {
public:
    virtual ~LogLineListener() = default;
    virtual void accept(const std::string& line) = 0;
};

class LogCaptureBackend
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
    AtomicWord<bool> _enabled{true};
    AtomicWord<bool> _stripEol;
};
}  // namespace mongo::logv2
