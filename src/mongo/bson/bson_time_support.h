/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * BSON deserialization support.
 * Parses from a BSON int32/int64 assuming the duration type.
 * e.g. parseDateFromDurationSinceEpoch<Seconds> will parse a Unix Epoch value.
 *      parseDateFromDurationSinceEpoch<Milliseconds> will parse an ECMAScript epoch value.
 */
template <typename Duration>
Date_t parseDateFromDurationSinceEpoch(const BSONElement& elem) {
    uassert(ErrorCodes::BadValue,
            str::stream() << "Epoch value must be numeric, got: " << typeName(elem.type()),
            elem.isNumber());

    return Date_t::fromDurationSinceEpoch(Duration{elem.exactNumberLong()});
}

/**
 * BSON serialization support.
 * Serializes a Date_t to a BSON int32/int64 since the unix epoch.
 */
template <typename Duration>
void serializeDateToDurationSinceEpoch(const Date_t& date,
                                       StringData fieldName,
                                       BSONObjBuilder* builder) {
    builder->append(fieldName, durationCount<Duration>(date.toDurationSinceEpoch()));
}

}  // namespace mongo
