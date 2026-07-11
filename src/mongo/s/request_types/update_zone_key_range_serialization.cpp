// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/request_types/update_zone_key_range_serialization.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

boost::optional<std::string> stringOrNullParseFromBSON(const BSONElement& elem) {
    if (elem.isNull()) {
        return boost::none;
    }
    if (elem.type() == BSONType::string) {
        return elem.str();
    }

    uasserted(ErrorCodes::TypeMismatch,
              fmt::format("Could not deserialize StringOrNull with type {}", elem.type()));
}

void stringOrNullSerializeToBSON(const boost::optional<std::string>& stringOrNull,
                                 std::string_view fieldName,
                                 BSONObjBuilder* bob) {
    if (stringOrNull) {
        bob->append(fieldName, *stringOrNull);
    } else {
        bob->appendNull(fieldName);
    }
}

}  // namespace mongo
