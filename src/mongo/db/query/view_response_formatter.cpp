/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/view_response_formatter.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

const char ViewResponseFormatter::kCountField[] = "n";
const char ViewResponseFormatter::kDistinctField[] = "values";
const char ViewResponseFormatter::kOkField[] = "ok";

ViewResponseFormatter::ViewResponseFormatter(BSONObj aggregationResponse)
    : _response(std::move(aggregationResponse)) {}

long long ViewResponseFormatter::getCountValue(boost::optional<TenantId> tenantId,
                                               const SerializationContext& serializationCtxt) {
    auto cursorResponse =
        CursorResponse::parseFromBSON(_response, nullptr, tenantId, serializationCtxt);
    uassertStatusOK(cursorResponse.getStatus());

    auto cursorFirstBatch = cursorResponse.getValue().getBatch();
    if (cursorFirstBatch.empty()) {
        // There were no results from aggregation, so the count is zero.
        return 0;
    } else {
        invariant(cursorFirstBatch.size() == 1);
        auto countObj = cursorFirstBatch.back();
        auto countElem = countObj["count"];
        tassert(9384400,
                str::stream() << "the 'count' should be of number type, but found " << countElem,
                countElem.isNumber());
        return countElem.safeNumberLong();
    }
}

void ViewResponseFormatter::appendAsCountResponse(BSONObjBuilder* resultBuilder,
                                                  boost::optional<TenantId> tenantId,
                                                  const SerializationContext& serializationCtxt) {
    // Note: getCountValue() uasserts upon errors.
    long long countResult = getCountValue(tenantId, serializationCtxt);
    // Append either BSON int32 or int64, depending on the value of countResult.
    // This is required so that drivers can continue to use a BSON int32 for count
    // values < 2 ^ 31, which is what some client applications may still depend on.
    // int64 is only used when the count value exceeds 2 ^ 31.
    resultBuilder->appendNumber(kCountField, countResult);
    resultBuilder->append(kOkField, 1);
}

Status ViewResponseFormatter::appendAsDistinctResponse(BSONObjBuilder* resultBuilder,
                                                       boost::optional<TenantId> tenantId,
                                                       boost::optional<BSONObj> metrics) {
    auto cursorResponse = CursorResponse::parseFromBSON(_response, nullptr, tenantId);
    if (!cursorResponse.isOK())
        return cursorResponse.getStatus();

    auto cursorFirstBatch = cursorResponse.getValue().getBatch();
    if (cursorFirstBatch.empty()) {
        // It's possible for the aggregation to return no document if the query to distinct matches
        // zero documents.
        resultBuilder->appendArray(kDistinctField, BSONObj());
    } else {
        invariant(cursorFirstBatch.size() == 1);
        auto distinctObj = cursorFirstBatch.back();
        resultBuilder->appendArray(kDistinctField, distinctObj["distinct"].embeddedObject());
    }

    if (metrics) {
        resultBuilder->append("metrics", metrics.value());
    }

    resultBuilder->append(kOkField, 1);
    return Status::OK();
}
}  // namespace mongo
