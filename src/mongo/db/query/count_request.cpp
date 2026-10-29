// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/count_request.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"

#include <limits>

namespace mongo {
namespace count_request {

long long countParseLimit(const BSONElement& element) {
    uassert(ErrorCodes::BadValue, "limit value is not a valid number", element.isNumber());
    auto limit = uassertStatusOK(element.parseIntegerElementToLong());
    // The absolute value of the smallest long long is too large to be represented as a long
    // long, so we fail to parse such count commands.
    uassert(ErrorCodes::BadValue,
            "limit value for count cannot be min long",
            limit != std::numeric_limits<long long>::min());

    // For counts, limit and -limit mean the same thing.
    if (limit < 0) {
        limit = -limit;
    }
    return limit;
}

}  // namespace count_request
}  // namespace mongo
