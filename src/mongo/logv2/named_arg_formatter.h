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

#pragma once

#include <boost/optional.hpp>
#include <fmt/format.h>

namespace mongo::logv2::detail {

class NamedArgFormatter : public fmt::arg_formatter<fmt::internal::buffer_range<char>> {
public:
    typedef fmt::arg_formatter<fmt::internal::buffer_range<char>> arg_formatter;

    NamedArgFormatter(fmt::format_context& ctx,
                      fmt::basic_parse_context<char>* parse_ctx = nullptr,
                      fmt::format_specs* spec = nullptr)
        : arg_formatter(ctx, parse_ctx, nullptr) {
        // Pretend that we have no format_specs, but store so we can re-construct the format
        // specifier
        if (spec) {
            spec_ = *spec;
        }
    }


    using arg_formatter::operator();

    auto operator()(fmt::string_view name) {
        write("{");
        arg_formatter::operator()(name);
        // If formatting spec was provided, reconstruct the string. Libfmt does not provide this
        // unfortunately
        if (spec_) {
            char str[2] = {'\0', '\0'};
            write(":");
            if (spec_->fill() != ' ') {
                str[0] = static_cast<char>(spec_->fill());
                write(str);
            }

            switch (spec_->align()) {
                case fmt::ALIGN_LEFT:
                    write("<");
                    break;
                case fmt::ALIGN_RIGHT:
                    write(">");
                    break;
                case fmt::ALIGN_CENTER:
                    write("^");
                    break;
                case fmt::ALIGN_NUMERIC:
                    write("=");
                    break;
                default:
                    break;
            };

            if (spec_->has(fmt::PLUS_FLAG))
                write("+");
            else if (spec_->has(fmt::MINUS_FLAG))
                write("-");
            else if (spec_->has(fmt::SIGN_FLAG))
                write(" ");
            else if (spec_->has(fmt::HASH_FLAG))
                write("#");

            if (spec_->align() == fmt::ALIGN_NUMERIC && spec_->fill() == 0)
                write("0");

            if (spec_->width() > 0)
                write(std::to_string(spec_->width()).c_str());

            if (spec_->has_precision()) {
                write(".");
                write(std::to_string(spec_->precision).c_str());
            }
            str[0] = spec_->type;
            write(str);
        }
        write("}");
        return out();
    }

private:
    boost::optional<fmt::format_specs> spec_;
};

}  // namespace mongo::logv2::detail
