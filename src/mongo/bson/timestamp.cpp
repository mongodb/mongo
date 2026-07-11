// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/timestamp.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/time_support.h"

#include <iostream>
#include <limits>

namespace mongo {

const Timestamp Timestamp::kAllowUnstableCheckpointsSentinel = Timestamp(0, 1);

Timestamp Timestamp::max() {
    unsigned int t = static_cast<unsigned int>(std::numeric_limits<uint32_t>::max());
    unsigned int i = std::numeric_limits<uint32_t>::max();
    return Timestamp(t, i);
}

std::string Timestamp::toStringPretty() const {
    std::stringstream ss;
    ss << time_t_to_String_short(secs) << ':' << i;
    return ss.str();
}

std::string Timestamp::toString() const {
    return fmt::format("{}", *this);
}

BSONObj Timestamp::toBSON() const {
    BSONObjBuilder bldr;
    bldr.append("", *this);
    return bldr.obj();
}
}  // namespace mongo
