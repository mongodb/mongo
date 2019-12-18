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

namespace mongo::logv2 {
namespace {

struct TextValueExtractor {
    void operator()(StringData name, CustomAttributeValue const& val) {
        std::string unescapedStr;
        if (val.stringSerialize) {
            fmt::memory_buffer buffer;
            val.stringSerialize(buffer);
            unescapedStr = fmt::to_string(buffer);
        } else {
            unescapedStr = val.toString();
        }
        _storage.push_back(str::escapeForText(unescapedStr));
        operator()(name, _storage.back());
    }

    void operator()(StringData name, const BSONObj* val) {
        _storage.push_back(val->jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0));
        operator()(name, _storage.back());
    }

    void operator()(StringData name, const BSONArray* val) {
        _storage.push_back(val->jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true));
        operator()(name, _storage.back());
    }

    void operator()(StringData name, StringData val) {
        _storage.push_back(str::escapeForText(val));
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

}  // namespace

void PlainFormatter::operator()(boost::log::record_view const& rec,
                                boost::log::formatting_ostream& strm) const {
    using namespace boost::log;

    StringData message = extract<StringData>(attributes::message(), rec).get();
    const auto& attrs = extract<TypeErasedAttributeStorage>(attributes::attributes(), rec).get();

    TextValueExtractor extractor;
    extractor.args.reserve(attrs.size());
    attrs.apply(extractor);
    fmt::memory_buffer buffer;
    fmt::vformat_to(
        buffer,
        to_string_view(message),
        fmt::basic_format_args<fmt::format_context>(extractor.args.data(), extractor.args.size()));
    strm.write(buffer.data(), buffer.size());
}

}  // namespace mongo::logv2
