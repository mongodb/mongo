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

#include <functional>

#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/utility/formatting_ostream.hpp>

#include "mongo/base/status.h"
#include "mongo/logv2/attributes.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"

namespace mongo {
namespace embedded {

/**
 * Sink backend for writing to callbacks registered with the embedded C API.
 */
class EmbeddedLogBackend
    : public boost::log::sinks::
          basic_formatted_sink_backend<char, boost::log::sinks::synchronized_feeding> {
public:
    EmbeddedLogBackend(
        std::function<void(void*, const char*, const char*, const char*, int)> callback,
        void* callbackUserData)
        : _callback(std::move(callback)), _callbackUserData(callbackUserData) {}

    void consume(boost::log::record_view const& rec, string_type const& formatted_string) {
        using boost::log::extract;

        auto severity = extract<logv2::LogSeverity>(logv2::attributes::severity(), rec).get();
        auto component = extract<logv2::LogComponent>(logv2::attributes::component(), rec).get();
        auto context = extract<StringData>(logv2::attributes::threadName(), rec).get();

        _callback(_callbackUserData,
                  formatted_string.c_str(),
                  component.getShortName().c_str(),
                  context.toString().c_str(),
                  severity.toInt());
    }

private:
    std::function<void(void*, const char*, const char*, const char*, int)> _callback;
    void* const _callbackUserData;
};

}  // namespace embedded
}  // namespace mongo
