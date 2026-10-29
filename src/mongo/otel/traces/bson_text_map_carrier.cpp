// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/bson_text_map_carrier.h"

#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace otel {
namespace traces {

BSONTextMapCarrier::BSONTextMapCarrier(const BSONObj& bson) {
    for (const auto& field : bson) {
        if (field.type() != BSONType::string) {
            continue;
        }
        _values[field.fieldName()] = field.String();
    }
}

BSONTextMapCarrier::BSONTextMapCarrier(const TelemetryContextSection& telemetryContext) {
    _values[kTraceParentKey] = telemetryContext.getOtel().getTraceparent();
}

OtelStringView BSONTextMapCarrier::Get(OtelStringView key) const noexcept {
    auto it = _values.find(key);
    if (it == _values.end()) {
        return kMissingKeyReturnValue;
    }
    return it->second;
}

void BSONTextMapCarrier::Set(OtelStringView key, OtelStringView value) noexcept {
    _values[key] = value;
}

bool BSONTextMapCarrier::Keys(function_ref<bool(OtelStringView)> callback) const noexcept {
    for (const auto& [key, _] : _values) {
        if (!callback(key)) {
            return false;
        }
    }
    return true;
}

BSONObj BSONTextMapCarrier::toBSON() const {
    BSONObjBuilder bob;
    for (const auto& [key, value] : _values) {
        bob.append(key, value);
    }
    return bob.obj();
}

}  // namespace traces
}  // namespace otel
}  // namespace mongo
