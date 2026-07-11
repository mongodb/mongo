// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <string_view>

namespace mongo::crypto {

/**
 * BSON deserialization support.
 * Parses from a BSON int32/int64 assuming the duration type.
 * e.g. parseDateFromDurationSinceEpoch<Seconds> will parse a Unix Epoch value.
 *      parseDateFromDurationSinceEpoch<Milliseconds> will parse an ECMAScript epoch value.
 */
inline Date_t parseUnixEpoch(const BSONElement& elem) {
    uassert(ErrorCodes::BadValue,
            str::stream() << "Epoch value must be numeric, got: " << typeName(elem.type()),
            elem.isNumber());
    return Date_t::fromDurationSinceEpoch(Seconds{elem.exactNumberLong()});
}

/**
 * BSON serialization support.
 * Serializes a Date_t to a BSON int32/int64 since the unix epoch.
 */
inline void serializeUnixEpoch(const Date_t& date,
                               std::string_view fieldName,
                               BSONObjBuilder* builder) {
    builder->append(fieldName, durationCount<Seconds>(date.toDurationSinceEpoch()));
}

}  // namespace mongo::crypto
