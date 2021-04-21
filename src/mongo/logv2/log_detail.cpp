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
    void operator()(const char* name, CustomAttributeValue const& val) {
        // Prefer string serialization over BSON if available.
        if (val.stringSerialize) {
            fmt::memory_buffer buffer;
            val.stringSerialize(buffer);
            _addString(name, fmt::to_string(buffer));
        } else if (val.toString) {
            _addString(name, val.toString());
        } else if (val.BSONAppend) {
            BSONObjBuilder builder;
            val.BSONAppend(builder, name);
            BSONElement element = builder.done().getField(name);
            _addString(name, element.toString(false));
        } else if (val.BSONSerialize) {
            BSONObjBuilder builder;
            val.BSONSerialize(builder);
            (*this)(name, builder.done());
        } else if (val.toBSONArray) {
            (*this)(name, val.toBSONArray());
        }
    }

    void operator()(const char* name, const BSONObj& val) {
        StringBuilder ss;
        val.toString(ss, false);
        _addString(name, ss.str());
    }

    void operator()(const char* name, const BSONArray& val) {
        StringBuilder ss;
        val.toString(ss, true);
        _addString(name, ss.str());
    }

    template <typename Period>
    void operator()(const char* name, const Duration<Period>& val) {
        _addString(name, val.toString());
    }

    template <typename T>
    void operator()(const char* name, const T& val) {
        _args.push_back(fmt::arg(name, std::cref(val)));
    }

    void reserve(size_t n) {
        _args.reserve(n, n);
    }

    const auto& args() const {
        return _args;
    }

private:
    void _addString(const char* name, std::string&& val) {
        (*this)(name, _storage.emplace_back(std::move(val)));
    }

    fmt::dynamic_format_arg_store<fmt::format_context> _args;
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
            msg.append(format(FMT_STRING(R"({}"{}")"), sep, a.name));
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
    extractor.reserve(attrs.size());
    attrs.apply(extractor);
    auto formatted =
        fmt::vformat(std::string_view{message.rawData(), message.size()}, extractor.args());

    doLogImpl(0, severity, options, formatted, TypeErasedAttributeStorage());
}

}  // namespace mongo::logv2::detail
