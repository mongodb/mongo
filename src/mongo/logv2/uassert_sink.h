// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/bson_formatter.h"
#include "mongo/logv2/plain_formatter.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/sinks.hpp>

namespace mongo::logv2 {
class [[MONGO_MOD_NEEDS_REPLACEMENT]] UserAssertSink
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
                      std::string_view(buffer.data(), buffer.size()));
        }
    }
};
}  // namespace mongo::logv2
