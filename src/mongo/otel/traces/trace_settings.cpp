// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/trace_settings.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/otel/traces/trace_settings_gen.h"
#include "mongo/util/str.h"

#include <string_view>
#include <vector>

namespace mongo::otel::traces {
namespace {
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
    auto new_headers = parseHttpHeadersFromBson(newValueElement);
    if (!new_headers.isOK()) {
        // Status is refcounted so this is "cheap" (though a move API would be great)
        return new_headers.getStatus();
    }

    httpExportHeaders() = std::move(new_headers.getValue());
    return Status::OK();
}

Status OpenTelemetryTracingHttpExportHeaders::setFromString(
    std::string_view s, const boost::optional<TenantId>& tenant) {
    try {
        auto b = BSON("v" << fromjson(s));
        return set(b.firstElement(), tenant);
    } catch (std::exception& e) {
        return Status(ErrorCodes::BadValue,
                      fmt::format("Failed to convert string to BSON object: {}", e.what()));
    }
}

void OpenTelemetryTracingResourceAttributes::append(OperationContext*,
                                                    BSONObjBuilder* bob,
                                                    std::string_view name,
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

Status OpenTelemetryTracingResourceAttributes::setFromString(
    std::string_view s, const boost::optional<TenantId>& tenant) {
    try {
        auto b = BSON("v" << fromjson(s));
        return set(b.firstElement(), tenant);
    } catch (std::exception& e) {
        return Status(ErrorCodes::BadValue,
                      fmt::format("Failed to convert string to BSON object: {}", e.what()));
    }
}

}  // namespace mongo::otel::traces
