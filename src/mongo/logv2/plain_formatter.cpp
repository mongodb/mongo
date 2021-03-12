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

#include "mongo/logv2/plain_formatter.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/attributes.h"
#include "mongo/logv2/constants.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/str_escape.h"

#include <boost/container/small_vector.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/expressions/message.hpp>
#include <boost/log/utility/formatting_ostream.hpp>

#include <any>
#include <deque>
#include <fmt/format.h>

namespace mongo::logv2 {
namespace {

struct TextValueExtractor {
    void operator()(const char* name, CustomAttributeValue const& val) {
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
            operator()(name, builder.done());
        } else if (val.toBSONArray) {
            operator()(name, val.toBSONArray());
        }
    }

    void operator()(const char* name, const BSONObj& val) {
        _addString(name, val.jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0));
    }

    void operator()(const char* name, const BSONArray& val) {
        _addString(name, val.jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true));
    }

    template <typename Period>
    void operator()(const char* name, const Duration<Period>& val) {
        _addString(name, val.toString());
    }

    template <typename T>
    void operator()(const char* name, const T& val) {
        _add(name, val);
    }

    const auto& args() const {
        return _args;
    }

    void reserve(std::size_t sz) {
        _args.reserve(sz, sz);
    }

private:
    /**
     * Workaround for `dynamic_format_arg_store`'s desire to copy string
     * values and user-defined values.
     */
    static auto _wrapValue(StringData val) {
        return std::string_view{val.rawData(), val.size()};
    }

    template <typename T>
    static auto _wrapValue(const T& val) {
        return std::cref(val);
    }

    template <typename T>
    void _add(const char* name, const T& val) {
        // Store our own fmt::arg results in a container of std::any,
        // and give reference_wrappers to _args. This avoids a string
        // copy of the 'name' inside _args.
        _args.push_back(std::cref(_store(fmt::arg(name, _wrapValue(val)))));
    }

    void _addString(const char* name, std::string&& val) {
        _add(name, StringData{_store(std::move(val))});
    }

    template <typename T>
    const T& _store(T&& val) {
        return std::any_cast<const T&>(_storage.emplace_back(std::forward<T>(val)));
    }

    std::deque<std::any> _storage;
    fmt::dynamic_format_arg_store<fmt::format_context> _args;
};

}  // namespace

void PlainFormatter::operator()(boost::log::record_view const& rec,
                                fmt::memory_buffer& buffer) const {
    using boost::log::extract;

    StringData message = extract<StringData>(attributes::message(), rec).get();
    const auto& attrs = extract<TypeErasedAttributeStorage>(attributes::attributes(), rec).get();

    // Log messages logged via logd are already formatted and have the id == 0
    if (attrs.empty()) {
        if (extract<int32_t>(attributes::id(), rec).get() == 0) {
            buffer.append(message.begin(), message.end());
            return;
        }
    }

    TextValueExtractor extractor;
    extractor.reserve(attrs.size());
    attrs.apply(extractor);
    fmt::vformat_to(buffer, std::string_view{message.rawData(), message.size()}, extractor.args());

    size_t attributeMaxSize = buffer.size();
    if (extract<LogTruncation>(attributes::truncation(), rec).get() == LogTruncation::Enabled) {
        if (_maxAttributeSizeKB)
            attributeMaxSize = _maxAttributeSizeKB->loadRelaxed() * 1024;
        else
            attributeMaxSize = constants::kDefaultMaxAttributeOutputSizeKB * 1024;
    }

    buffer.resize(std::min(attributeMaxSize, buffer.size()));
    if (StringData sd(buffer.data(), buffer.size()); sd.endsWith("\n"_sd))
        buffer.resize(buffer.size() - 1);
}

void PlainFormatter::operator()(boost::log::record_view const& rec,
                                boost::log::formatting_ostream& strm) const {
    fmt::memory_buffer buffer;
    operator()(rec, buffer);
    strm.write(buffer.data(), buffer.size());
}

}  // namespace mongo::logv2
