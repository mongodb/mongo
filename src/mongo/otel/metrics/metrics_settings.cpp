// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/metrics_settings.h"

#include "mongo/bson/json.h"
#include "mongo/otel/metrics/metrics_settings_gen.h"


namespace mongo::otel::metrics {

namespace {
HttpHeaderMap& httpExportHeaders() {
    static HttpHeaderMap headers;
    return headers;
}
}  // namespace

const HttpHeaderMap& getMetricsHttpExportHeaders() {
    return httpExportHeaders();
}

Status OpenTelemetryMetricsHttpExportHeaders::set(const BSONElement& newValueElement,
                                                  const boost::optional<TenantId>&) {
    auto new_headers = parseHttpHeadersFromBson(newValueElement);
    if (!new_headers.isOK()) {
        // Status is refcounted so this is "cheap" (though a move API would be great)
        return new_headers.getStatus();
    }

    httpExportHeaders() = std::move(new_headers.getValue());
    return Status::OK();
}

Status OpenTelemetryMetricsHttpExportHeaders::setFromString(
    std::string_view s, const boost::optional<TenantId>& tenant) {
    try {
        auto b = BSON("v" << fromjson(s));
        return set(b.firstElement(), tenant);
    } catch (std::exception& e) {
        return Status(ErrorCodes::BadValue,
                      fmt::format("Failed to convert string to BSON object: {}", e.what()));
    }
}
}  // namespace mongo::otel::metrics
