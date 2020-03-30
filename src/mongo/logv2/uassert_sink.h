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

#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/sinks.hpp>

#include "mongo/logv2/bson_formatter.h"
#include "mongo/util/assert_util.h"

namespace mongo::logv2 {
class UserAssertSink
    : public boost::log::sinks::
          basic_formatted_sink_backend<char, boost::log::sinks::concurrent_feeding> {
public:
    void consume(boost::log::record_view const& rec, string_type const& formatted_string) {
        using boost::log::extract;
        auto code = extract<int32_t>(attributes::userassert(), rec).get();
        if (code != ErrorCodes::OK) {
            fmt::memory_buffer buffer;
            PlainFormatter()(rec, buffer);
            uasserted(code == constants::kUserAssertWithLogID
                          ? extract<int32_t>(attributes::id(), rec).get()
                          : code,
                      StringData(buffer.data(), buffer.size()));
        }
    }
};
}  // namespace mongo::logv2
