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
#include "mongo/logv2/name_extractor.h"
#include "mongo/logv2/named_arg_formatter.h"
#include "mongo/util/time_support.h"

#include <fmt/format.h>

namespace mongo::logv2 {
namespace {
struct JSONValueExtractor {
    JSONValueExtractor(fmt::memory_buffer& buffer) : _buffer(buffer) {}

    void operator()(StringData name, CustomAttributeValue const& val) {
        // Try to format as BSON first if available. Prefer BSONAppend if available as we might only
        // want the value and not the whole element.
        if (val.BSONAppend) {
            BSONObjBuilder builder;
            val.BSONAppend(builder, name);
            // This is a JSON subobject, no quotes needed
            storeUnquoted(name);
            builder.done().getField(name).jsonStringBuffer(
                JsonStringFormat::ExtendedRelaxedV2_0_0, false, 0, _buffer);
        } else if (val.BSONSerialize) {
            // This is a JSON subobject, no quotes needed
            storeUnquoted(name);
            BSONObjBuilder builder;
            val.BSONSerialize(builder);
            builder.done().jsonStringBuffer(
                JsonStringFormat::ExtendedRelaxedV2_0_0, 0, false, _buffer);
        } else if (val.toBSONArray) {
            // This is a JSON subarray, no quotes needed
            storeUnquoted(name);
            val.toBSONArray().jsonStringBuffer(
                JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true, _buffer);
        } else if (val.stringSerialize) {
            storeUnquoted(name);
            _buffer.push_back('"');
            val.stringSerialize(_buffer);
            _buffer.push_back('"');
        } else {
            // This is a string, surround value with quotes
            storeQuoted(name, val.toString());
        }
    }

    void operator()(StringData name, const BSONObj* val) {
        // This is a JSON subobject, no quotes needed
        storeUnquoted(name);
        val->jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0, 0, false, _buffer);
    }

    void operator()(StringData name, const BSONArray* val) {
        // This is a JSON subobject, no quotes needed
        storeUnquoted(name);
        val->jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true, _buffer);
    }

    void operator()(StringData name, StringData value) {
        storeQuoted(name, value);
    }

    template <typename T>
    void operator()(StringData name, const T& value) {
        storeUnquotedValue(name, value);
    }


private:
    void storeUnquoted(StringData name) {
        fmt::format_to(_buffer, R"({}"{}":)", _separator, name);
        _separator = ","_sd;
    }

    template <typename T>
    void storeUnquotedValue(StringData name, const T& value) {
        fmt::format_to(_buffer, R"({}"{}":{})", _separator, name, value);
        _separator = ","_sd;
    }

    template <typename T>
    void storeQuoted(StringData name, const T& value) {
        fmt::format_to(_buffer, R"({}"{}":"{}")", _separator, name, value);
        _separator = ","_sd;
    }

    fmt::memory_buffer& _buffer;
    StringData _separator = ""_sd;
};
}  // namespace

void JSONFormatter::operator()(boost::log::record_view const& rec,
                               boost::log::formatting_ostream& strm) const {
    using namespace boost::log;

    // Build a JSON object for the user attributes.
    const auto& attrs = extract<TypeErasedAttributeStorage>(attributes::attributes(), rec).get();

    std::string id;
    auto stable_id = extract<StringData>(attributes::stableId(), rec).get();
    if (!stable_id.empty()) {
        id = fmt::format("\"{}\":\"{}\",", constants::kStableIdFieldName, stable_id);
    }

    StringData severity =
        extract<LogSeverity>(attributes::severity(), rec).get().toStringDataCompact();
    StringData component =
        extract<LogComponent>(attributes::component(), rec).get().getNameForLog();
    std::string tag;
    LogTag tags = extract<LogTag>(attributes::tags(), rec).get();
    if (tags != LogTag::kNone) {
        tag = fmt::format(
            ",\"{}\":{}",
            constants::kTagsFieldName,
            tags.toBSONArray().jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true));
    }

    fmt::memory_buffer buffer;

    // Put all fields up until the message value
    fmt::format_to(buffer,
                   R"({{)"
                   R"("{}":{{"$date":"{}"}},)"  // timestamp
                   R"("{}":"{}"{: <{}})"        // severity with padding for the comma
                   R"("{}":"{}"{: <{}})"        // component with padding for the comma
                   R"("{}":"{}",)"              // context
                   R"({})"                      // optional stable id
                   R"("{}":")",                 // message
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
                   constants::kMessageFieldName);

    // Insert the attribute names back into the message string using a special formatter and format
    // into buffer
    detail::NameExtractor nameExtractor;
    attrs.apply(nameExtractor);

    fmt::vformat_to<detail::NamedArgFormatter, char>(
        buffer,
        extract<StringData>(attributes::message(), rec).get().toString(),
        fmt::basic_format_args<fmt::format_context>(nameExtractor.nameArgs.data(),
                                                    nameExtractor.nameArgs.size()));

    if (attrs.empty()) {
        // If no attributes we can just close the message string
        buffer.push_back('"');
    } else {
        // otherwise, add attribute field name and opening brace
        fmt::format_to(buffer, R"(","{}":{{)", constants::kAttributesFieldName);
    }

    if (!attrs.empty()) {
        // comma separated list of attributes (no opening/closing brace are added here)
        JSONValueExtractor extractor(buffer);
        attrs.apply(extractor);
    }

    // Add remaining fields
    fmt::format_to(buffer,
                   R"({})"  // optional attribute closing
                   R"({})"  // optional tags
                   R"(}})",
                   // closing brace
                   attrs.empty() ? "" : "}",
                   // tags
                   tag);

    // Write final JSON object to output stream
    strm.write(buffer.data(), buffer.size());
}

}  // namespace mongo::logv2
