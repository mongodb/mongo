/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/count_request.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/query_request.h"

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

long long countParseSkip(const BSONElement& element) {
    uassert(ErrorCodes::BadValue, "skip value is not a valid number", element.isNumber());
    auto skip = uassertStatusOK(element.parseIntegerElementToNonNegativeLong());
    return skip;
}

BSONObj countParseHint(const BSONElement& element) {
    if (element.type() == BSONType::String) {
        return BSON("$hint" << element.valueStringData());
    } else if (element.type() == BSONType::Object) {
        return element.Obj();
    } else {
        uasserted(31012, "Hint must be a string or an object");
    }
    MONGO_UNREACHABLE;
}

long long countParseMaxTime(const BSONElement& element) {
    auto maxTimeVal = uassertStatusOK(QueryRequest::parseMaxTimeMS(element));
    return static_cast<long long>(maxTimeVal);
}
}  // namespace count_request
}  // namespace mongo
