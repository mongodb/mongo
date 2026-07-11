// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
