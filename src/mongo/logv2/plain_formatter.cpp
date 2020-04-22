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
#include "mongo/util/str_escape.h"

#include <boost/container/small_vector.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/expressions/message.hpp>
#include <boost/log/utility/formatting_ostream.hpp>

#include <deque>
#include <fmt/format.h>
#include <forward_list>

namespace mongo::logv2 {
namespace {

struct TextValueExtractor {
    void operator()(StringData name, CustomAttributeValue const& val) {
        if (val.stringSerialize) {
            fmt::memory_buffer buffer;
            val.stringSerialize(buffer);
            _storage.push_back(fmt::to_string(buffer));
            operator()(name, StringData(_storage.back()));
        } else if (val.toString) {
            _storage.push_back(val.toString());
            operator()(name, StringData(_storage.back()));
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
        _storage.push_back(val.jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0));
        operator()(name, StringData(_storage.back()));
    }

    void operator()(StringData name, const BSONArray& val) {
        _storage.push_back(val.jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true));
        operator()(name, StringData(_storage.back()));
    }

    template <typename Period>
    void operator()(StringData name, const Duration<Period>& val) {
        _storage.push_back(val.toString());
        operator()(name, StringData(_storage.back()));
    }

    void operator()(StringData name, bool val) {
        _namedBool.push_front(fmt::arg(fmt::string_view(name.rawData(), name.size()), val));
        add(_namedBool.front());
    }

    void operator()(StringData name, int val) {
        _namedInt.push_front(fmt::arg(fmt::string_view(name.rawData(), name.size()), val));
        add(_namedInt.front());
    }

    void operator()(StringData name, unsigned int val) {
        _namedUInt.push_front(fmt::arg(fmt::string_view(name.rawData(), name.size()), val));
        add(_namedUInt.front());
    }

    void operator()(StringData name, long long val) {
        _namedLL.push_front(fmt::arg(fmt::string_view(name.rawData(), name.size()), val));
        add(_namedLL.front());
    }

    void operator()(StringData name, unsigned long long val) {
        _namedULL.push_front(fmt::arg(fmt::string_view(name.rawData(), name.size()), val));
        add(_namedULL.front());
    }

    void operator()(StringData name, double val) {
        _namedDouble.push_front(fmt::arg(fmt::string_view(name.rawData(), name.size()), val));
        add(_namedDouble.front());
    }

    void operator()(StringData name, StringData val) {
        _namedStringData.push_front(fmt::arg(fmt::string_view(name.rawData(), name.size()), val));
        add(_namedStringData.front());
    }

    boost::container::small_vector<fmt::basic_format_arg<fmt::format_context>,
                                   constants::kNumStaticAttrs>
        args;

private:
    template <typename T>
    void add(const T& named) {
        args.push_back(fmt::internal::make_arg<fmt::format_context>(named));
    }

    std::deque<std::string> _storage;
    std::forward_list<fmt::internal::named_arg<bool, char>> _namedBool;
    std::forward_list<fmt::internal::named_arg<int, char>> _namedInt;
    std::forward_list<fmt::internal::named_arg<unsigned int, char>> _namedUInt;
    std::forward_list<fmt::internal::named_arg<long long, char>> _namedLL;
    std::forward_list<fmt::internal::named_arg<unsigned long long, char>> _namedULL;
    std::forward_list<fmt::internal::named_arg<double, char>> _namedDouble;
    std::forward_list<fmt::internal::named_arg<StringData, char>> _namedStringData;
};

}  // namespace

void PlainFormatter::operator()(boost::log::record_view const& rec,
                                fmt::memory_buffer& buffer) const {
    using namespace boost::log;

    StringData message = extract<StringData>(attributes::message(), rec).get();
    const auto& attrs = extract<TypeErasedAttributeStorage>(attributes::attributes(), rec).get();

    TextValueExtractor extractor;
    extractor.args.reserve(attrs.size());
    attrs.apply(extractor);
    fmt::vformat_to(
        buffer,
        to_string_view(message),
        fmt::basic_format_args<fmt::format_context>(extractor.args.data(), extractor.args.size()));

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
    using namespace boost::log;
    fmt::memory_buffer buffer;
    operator()(rec, buffer);
    strm.write(buffer.data(), buffer.size());
}

}  // namespace mongo::logv2
