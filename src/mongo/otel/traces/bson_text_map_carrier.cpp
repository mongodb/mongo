/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
