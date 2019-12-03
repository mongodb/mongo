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

#include "mongo/logv2/json_formatter.h"

#include <boost/container/small_vector.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/expressions/message.hpp>
#include <boost/log/utility/formatting_ostream.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/attributes.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/logv2/named_arg_formatter.h"
#include "mongo/util/time_support.h"

#include <fmt/format.h>

namespace mongo::logv2 {
namespace {
struct JSONValueExtractor {
    void operator()(StringData name, CustomAttributeValue const& val) {
        if (val.BSONAppend) {
            BSONObjBuilder builder;
            val.BSONAppend(builder, name);
            // This is a JSON subobject, no quotes needed
            storeUnquoted(name,
                          builder.obj().getField(name).jsonString(JsonStringFormat::Strict, false));
        } else if (val.toBSON) {
            // This is a JSON subobject, no quotes needed
            storeUnquoted(name, val.toBSON().jsonString());
        } else {
            // This is a string, surround value with quotes
            storeQuoted(name, val.toString());
        }
    }

    void operator()(StringData name, const BSONObj* val) {
        // This is a JSON subobject, no quotes needed
        storeUnquoted(name, val->jsonString());
    }

    void operator()(StringData name, StringData value) {
        storeQuoted(name, value);
    }

    template <typename T>
    void operator()(StringData name, const T& value) {
        storeUnquoted(name, value);
    }

    fmt::memory_buffer buffer;
    boost::container::small_vector<fmt::basic_format_arg<fmt::format_context>,
                                   constants::kNumStaticAttrs>
        nameArgs;

private:
    template <typename T>
    void storeUnquoted(StringData name, const T& value) {
        // The first {} is for the member separator, added by storeImpl()
        storeImpl(R"({}"{}":{})", name, value);
    }


    template <typename T>
    void storeQuoted(StringData name, const T& value) {
        // The first {} is for the member separator, added by storeImpl()
        storeImpl(R"({}"{}":"{}")", name, value);
    }

    template <typename T>
    void storeImpl(const char* fmt_str, StringData name, const T& value) {
        nameArgs.push_back(fmt::internal::make_arg<fmt::format_context>(name));
        fmt::format_to(buffer, fmt_str, _separator, name, value);
        _separator = ","_sd;
    }

    StringData _separator = ""_sd;
};
}  // namespace

void JSONFormatter::operator()(boost::log::record_view const& rec,
                               boost::log::formatting_ostream& strm) const {
    using namespace boost::log;

    // Build a JSON object for the user attributes.
    const auto& attrs = extract<TypeErasedAttributeStorage>(attributes::attributes(), rec).get();

    JSONValueExtractor extractor;
    attrs.apply(extractor);

    std::string id;
    auto stable_id = extract<StringData>(attributes::stableId(), rec).get();
    if (!stable_id.empty()) {
        id = fmt::format("\"{}\":\"{}\",", constants::kStableIdFieldName, stable_id);
    }

    std::string message;
    fmt::memory_buffer buffer;
    fmt::vformat_to<detail::NamedArgFormatter, char>(
        buffer,
        extract<StringData>(attributes::message(), rec).get().toString(),
        fmt::basic_format_args<fmt::format_context>(extractor.nameArgs.data(),
                                                    extractor.nameArgs.size()));
    message = fmt::to_string(buffer);

    StringData severity =
        extract<LogSeverity>(attributes::severity(), rec).get().toStringDataCompact();
    StringData component =
        extract<LogComponent>(attributes::component(), rec).get().getNameForLog();
    std::string tag;
    LogTag tags = extract<LogTag>(attributes::tags(), rec).get();
    if (tags != LogTag::kNone) {
        tag = fmt::format(",\"{}\":{}",
                          constants::kTagsFieldName,
                          tags.toBSON().jsonString(JsonStringFormat::Strict, 0, true));
    }

    strm << fmt::format(
        R"({{)"
        R"("{}":{{"$date":"{}"}},)"  // timestamp
        R"("{}":"{}"{: <{}})"        // severity with padding for the comma
        R"("{}":"{}"{: <{}})"        // component with padding for the comma
        R"("{}":"{}",)"              // context
        R"({})"                      // optional stable id
        R"("{}":"{}")"               // message
        R"({})",                     // optional attribute key
                                     // timestamp
        constants::kTimestampFieldName,
        dateToISOStringUTC(extract<Date_t>(attributes::timeStamp(), rec).get()),
        // severity, left align the comma and add padding to create fixed column width
        constants::kSeverityFieldName,
        severity,
        ",",
        3 - severity.size(),
        // component, left align the comma and add padding to create fixed column width
        constants::kComponentFieldName,
        component,
        ",",
        9 - component.size(),
        // context
        constants::kContextFieldName,
        extract<StringData>(attributes::threadName(), rec).get(),
        // stable id
        id,
        // message
        constants::kMessageFieldName,
        message,
        // attribute field name and opening brace
        attrs.empty() ? "" : fmt::format(R"(,"{}":{{)", constants::kAttributesFieldName));

    if (!attrs.empty()) {
        // comma separated list of attributes
        strm << fmt::to_string(extractor.buffer);
    }

    strm << fmt::format(R"({})"  // optional attribute closing
                        R"({})"  // optional tags
                        R"(}})",
                        // closing brace
                        attrs.empty() ? "" : "}",
                        // tags
                        tag);
}

}  // namespace mongo::logv2
