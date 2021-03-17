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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/platform/basic.h"

#include <fmt/format.h>

#include "mongo/logv2/attributes.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_domain_internal.h"
#include "mongo/logv2/log_options.h"
#include "mongo/logv2/log_source.h"
#include "mongo/util/testing_proctor.h"

namespace mongo::logv2::detail {

struct UnstructuredValueExtractor {
    void operator()(StringData name, CustomAttributeValue const& val) {
        // Prefer string serialization over BSON if available.
        if (val.stringSerialize) {
            fmt::memory_buffer buffer;
            val.stringSerialize(buffer);
            _storage.push_back(fmt::to_string(buffer));
            operator()(name, _storage.back());
        } else if (val.toString) {
            _storage.push_back(val.toString());
            operator()(name, _storage.back());
        } else if (val.BSONAppend) {
            BSONObjBuilder builder;
            val.BSONAppend(builder, name);
            BSONElement element = builder.done().getField(name);
            _storage.push_back(element.toString(false));
            operator()(name, _storage.back());
        } else if (val.BSONSerialize) {
            BSONObjBuilder builder;
            val.BSONSerialize(builder);
            operator()(name, builder.done());
        } else if (val.toBSONArray) {
            operator()(name, val.toBSONArray());
        }
    }

    void operator()(StringData name, const BSONObj& val) {
        StringBuilder ss;
        val.toString(ss, false);
        _storage.push_back(ss.str());
        operator()(name, _storage.back());
    }

    void operator()(StringData name, const BSONArray& val) {
        StringBuilder ss;
        val.toString(ss, true);
        _storage.push_back(ss.str());
        operator()(name, _storage.back());
    }

    template <typename Period>
    void operator()(StringData name, const Duration<Period>& val) {
        _storage.push_back(val.toString());
        operator()(name, _storage.back());
    }

    template <typename T>
    void operator()(StringData name, const T& val) {
        args.push_back(fmt::internal::make_arg<fmt::format_context>(val));
    }

    boost::container::small_vector<fmt::basic_format_arg<fmt::format_context>,
                                   constants::kNumStaticAttrs>
        args;

private:
    std::deque<std::string> _storage;
};

static void checkUniqueAttrs(int32_t id, const TypeErasedAttributeStorage& attrs) {
    if (attrs.size() <= 1)
        return;
    // O(N^2), but N is small and this avoids alloc, sort, and operator<.
    auto first = attrs.begin();
    auto last = attrs.end();
    while (first != last) {
        auto it = first;
        ++first;
        if (std::find_if(first, last, [&](auto&& a) { return a.name == it->name; }) == last)
            continue;
        StringData sep;
        std::string msg;
        for (auto&& a : attrs) {
            msg.append(sep.rawData(), sep.size())
                .append("\"")
                .append(a.name.rawData(), a.name.size())
                .append("\"");
            sep = ","_sd;
        }
        uasserted(4793301, format(FMT_STRING("LOGV2 (id={}) attribute collision: [{}]"), id, msg));
    }
}

void doLogImpl(int32_t id,
               LogSeverity const& severity,
               LogOptions const& options,
               StringData message,
               TypeErasedAttributeStorage const& attrs) {
    dassert(options.component() != LogComponent::kNumLogComponents);
    // TestingProctor isEnabled cannot be called before it has been
    // initialized. But log statements occurring earlier than that still need
    // to be checked. Log performance isn't as important at startup, so until
    // the proctor is initialized, we check everything.
    if (const auto& tp = TestingProctor::instance(); !tp.isInitialized() || tp.isEnabled()) {
        checkUniqueAttrs(id, attrs);
    }

    auto& source = options.domain().internal().source();
    auto record = source.open_record(id,
                                     severity,
                                     options.component(),
                                     options.tags(),
                                     options.truncation(),
                                     options.uassertErrorCode());
    if (record) {
        record.attribute_values().insert(
            attributes::message(),
            boost::log::attribute_value(
                new boost::log::attributes::attribute_value_impl<StringData>(message)));

        record.attribute_values().insert(
            attributes::attributes(),
            boost::log::attribute_value(
                new boost::log::attributes::attribute_value_impl<TypeErasedAttributeStorage>(
                    attrs)));

        source.push_record(std::move(record));
    }
}

void doUnstructuredLogImpl(LogSeverity const& severity,  // NOLINT
                           LogOptions const& options,
                           StringData message,
                           TypeErasedAttributeStorage const& attrs) {

    UnstructuredValueExtractor extractor;
    extractor.args.reserve(attrs.size());
    attrs.apply(extractor);
    auto formatted = fmt::vformat(
        to_string_view(message),
        fmt::basic_format_args<fmt::format_context>(extractor.args.data(), extractor.args.size()));

    doLogImpl(0, severity, options, formatted, TypeErasedAttributeStorage());
}

}  // namespace mongo::logv2::detail
