// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

void stringOrNullSerializeToBSON(const boost::optional<std::string>& fields,
                                 std::string_view fieldName,
                                 BSONObjBuilder* bob);

boost::optional<std::string> stringOrNullParseFromBSON(const BSONElement& elem);

}  // namespace mongo
