// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/otel/utils/bson_to_http_headers.h"

#include "mongo/bson/bsonobj.h"

namespace mongo::otel {

namespace {
using namespace std::literals::string_view_literals;
bool isHttpHeaderStringValid(std::string_view s) {
    return s.find_first_of("\r\n"sv) == std::string_view::npos;
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
