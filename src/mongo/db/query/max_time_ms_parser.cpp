/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/query/max_time_ms_parser.h"

#include <fmt/format.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

namespace mongo {

StatusWith<int> parseMaxTimeMS(BSONElement maxTimeMSElt) {
    if (!maxTimeMSElt.eoo() && !maxTimeMSElt.isNumber()) {
        return StatusWith<int>(
            ErrorCodes::BadValue,
            (StringBuilder() << maxTimeMSElt.fieldNameStringData() << " must be a number").str());
    }
    long long maxTimeMSLongLong = maxTimeMSElt.safeNumberLong();  // returns 0 on EOO

    const long long maxVal = maxTimeMSElt.fieldNameStringData() == kMaxTimeMSOpOnlyField
        ? (long long)(INT_MAX) + kMaxTimeMSOpOnlyMaxPadding
        : INT_MAX;

    using namespace fmt::literals;

    if (maxTimeMSLongLong < 0 || maxTimeMSLongLong > maxVal)
        return Status(ErrorCodes::BadValue,
                      "{} value for {} is out of range [{}, {}]"_format(
                          maxTimeMSLongLong, maxTimeMSElt.fieldNameStringData(), 0, maxVal));

    double maxTimeMSDouble = maxTimeMSElt.numberDouble();
    if (maxTimeMSElt.type() == mongo::NumberDouble && floor(maxTimeMSDouble) != maxTimeMSDouble) {
        return StatusWith<int>(
            ErrorCodes::BadValue,
            (StringBuilder() << maxTimeMSElt.fieldNameStringData() << " has non-integral value")
                .str());
    }
    return StatusWith<int>(static_cast<int>(maxTimeMSLongLong));
}

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
int32_t parseMaxTimeMSForIDL(BSONElement maxTimeMSElt) {
    return static_cast<int32_t>(uassertStatusOK(parseMaxTimeMS(maxTimeMSElt)));
}
}  // namespace mongo
