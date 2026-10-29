// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

/**
 * BSON deserialization support.
 * Parses from a BSON int32/int64 assuming the duration type.
 * e.g. parseDurationFromCount<Seconds> will parse a number of seconds as a Duration.
 */
template <typename Duration>
[[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]] Duration parseDurationFromCount(
    const BSONElement& elem) {
    uassert(ErrorCodes::BadValue,
            str::stream() << "Duration value must be numeric, got: " << typeName(elem.type()),
            elem.isNumber());

    return Duration{elem.exactNumberLong()};
}

/**
 * BSON serialization support.
 * Serializes a Duration to a BSON int32/int64 for the specified units.
 */
template <typename ToDuration, typename Period>
[[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]] void serializeDurationToCount(
    const Duration<Period>& duration, std::string_view fieldName, BSONObjBuilder* builder) {
    builder->append(fieldName, durationCount<ToDuration>(duration));
}

}  // namespace mongo
