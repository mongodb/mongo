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

#include "mongo/otel/traces/trace_settings.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/otel/traces/trace_settings_gen.h"
#include "mongo/util/str.h"

#include <vector>

namespace mongo::otel::traces {
namespace {
using HttpHeaderMap = stdx::unordered_map<std::string, std::vector<std::string>>;
using AttributeMap = stdx::unordered_map<std::string, std::string>;

HttpHeaderMap& httpExportHeaders() {
    static HttpHeaderMap headers;
    return headers;
}
AttributeMap& attributeMap() {
    static AttributeMap attributes;
    return attributes;
}
}  // namespace

const HttpHeaderMap& getTracingHttpExportHeaders() {
    return httpExportHeaders();
}

const AttributeMap& getTracingResourceAttributes() {
    return attributeMap();
}

Status OpenTelemetryTracingHttpExportHeaders::set(const BSONElement& newValueElement,
                                                  const boost::optional<TenantId>&) {
    if (newValueElement.type() != BSONType::object) {
        return Status(ErrorCodes::BadValue,
                      "openTelemetryTracingHttpExportHeaders must be a BSON document");
    }

    HttpHeaderMap headers;

    for (const BSONElement& elem : newValueElement.Obj()) {
        auto [it, inserted] = headers.try_emplace(elem.fieldName());
        if (!inserted) {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                              << "openTelemetryTracingHttpExportHeaders contains duplicate key: '"
                              << elem.fieldName() << "'");
        }
        std::vector<std::string>& values = it->second;
        if (elem.type() == BSONType::string) {
            values.push_back(elem.String());
        } else if (elem.type() == BSONType::array) {
            for (BSONElement& val : elem.Array()) {
                if (val.type() != BSONType::string) {
                    return Status(
                        ErrorCodes::BadValue,
                        "openTelemetryTracingHttpExportHeaders array values must be strings");
                }
                values.push_back(val.String());
            }
        } else {
            return Status(ErrorCodes::BadValue,
                          "openTelemetryTracingHttpExportHeaders values must be strings or arrays "
                          "of strings");
        }
    }

    httpExportHeaders() = std::move(headers);
    return Status::OK();
}

Status OpenTelemetryTracingHttpExportHeaders::setFromString(StringData,
                                                            const boost::optional<TenantId>&) {
    return Status(ErrorCodes::BadValue,
                  "openTelemetryTracingHttpExportHeaders cannot be set via string; "
                  "provide a BSON document");
}

void OpenTelemetryTracingResourceAttributes::append(OperationContext*,
                                                    BSONObjBuilder* bob,
                                                    StringData name,
                                                    const boost::optional<TenantId>&) {
    BSONObjBuilder sub(bob->subobjStart(name));
    for (auto& [k, v] : attributeMap()) {
        sub.append(k, v);
    }
}

Status OpenTelemetryTracingResourceAttributes::set(const BSONElement& newValueElement,
                                                   const boost::optional<TenantId>&) {
    if (newValueElement.type() != BSONType::object) {
        return Status(ErrorCodes::BadValue,
                      "openTelemetryTracingResourceAttributes must be a BSON document");
    }

    AttributeMap attributes;
    for (const BSONElement& elem : newValueElement.Obj()) {
        if (elem.type() != BSONType::string) {
            return Status(ErrorCodes::BadValue,
                          "openTelemetryTracingResourceAttributes values must be strings");
        }
        if (!attributes.emplace(elem.fieldName(), elem.String()).second) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "openTelemetryTracingResourceAttributes contains "
                                           "duplicate key: '"
                                        << elem.fieldName() << "'");
        }
    }

    attributeMap() = std::move(attributes);
    return Status::OK();
}

Status OpenTelemetryTracingResourceAttributes::setFromString(StringData,
                                                             const boost::optional<TenantId>&) {
    return Status(ErrorCodes::BadValue,
                  "openTelemetryTracingResourceAttributes cannot be set via string; "
                  "provide a BSON document");
}

}  // namespace mongo::otel::traces
