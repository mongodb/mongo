/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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


#include "mongo/otel/utils/bson_to_http_headers.h"

#include "mongo/bson/bsonobj.h"

namespace mongo::otel {

namespace {
bool isHttpHeaderStringValid(std::string_view s) {
    return s.find_first_of("\r\n"_sd) == std::string_view::npos;
}
}  // namespace

StatusWith<HttpHeaderMap> parseHttpHeadersFromBson(const BSONElement& headerDocument) {
    if (headerDocument.type() != BSONType::object) {
        return Status(ErrorCodes::BadValue,
                      fmt::format("Element must be a BSON document, actual type: {}",
                                  typeName(headerDocument.type())));
    }

    HttpHeaderMap headers;

    for (const auto& elem : headerDocument.Obj()) {
        auto name = elem.fieldNameStringData();
        if (name.empty()) {
            return Status(ErrorCodes::BadValue, fmt::format("Document contains an empty key"));
        }
        if (!isHttpHeaderStringValid(name)) {
            return Status(ErrorCodes::BadValue,
                          fmt::format("Document contains a key with invalid characters"));
        }

        auto [it, inserted] = headers.try_emplace(name);
        if (!inserted) {
            return Status(ErrorCodes::BadValue,
                          fmt::format("Document contains duplicate key: '{}'", name));
        }

        auto& values = it->second;

        switch (elem.type()) {
            case BSONType::string: {
                auto s = elem.String();
                if (!isHttpHeaderStringValid(s)) {
                    return Status(
                        ErrorCodes::BadValue,
                        fmt::format("Key '{}' contains a value with invalid characters", name));
                }
                values.push_back(std::move(s));
                break;
            }
            case BSONType::array: {
                auto arr = elem.Array();
                values.reserve(arr.size());
                for (const auto& val : arr) {
                    if (val.type() != BSONType::string) {
                        return Status(ErrorCodes::BadValue,
                                      fmt::format("BSON array values must be strings, found "
                                                  "element type {} instead",
                                                  typeName(val.type())));
                    }

                    auto s = val.String();
                    if (!isHttpHeaderStringValid(s)) {
                        return Status(
                            ErrorCodes::BadValue,
                            fmt::format("Key '{}' contains a value with invalid characters", name));
                    }
                    values.push_back(std::move(s));
                }
                break;
            }
            default: {
                return Status(
                    ErrorCodes::BadValue,
                    fmt::format("Document values must be strings or arrays of strings, found "
                                "element type {} instead",
                                typeName(elem.type())));
            }
        }
    }

    return StatusWith(std::move(headers));
}
}  // namespace mongo::otel
