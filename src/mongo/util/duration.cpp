// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/duration.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <fmt/format.h>

namespace mongo {

template <typename Period>
BSONObj Duration<Period>::toBSON() const {
    BSONObjBuilder builder;
    builder.append(fmt::format("duration{}", mongoUnitSuffix()), count());
    return builder.obj();
}

template BSONObj Nanoseconds::toBSON() const;
template BSONObj Microseconds::toBSON() const;
template BSONObj Milliseconds::toBSON() const;
template BSONObj Seconds::toBSON() const;
template BSONObj Minutes::toBSON() const;
template BSONObj Hours::toBSON() const;
template BSONObj Days::toBSON() const;

}  // namespace mongo
