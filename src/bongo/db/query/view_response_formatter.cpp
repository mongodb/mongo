/**
*    Copyright (C) 2016 BongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "bongo/platform/basic.h"

#include "bongo/db/query/view_response_formatter.h"

#include "bongo/base/status.h"
#include "bongo/bson/bsonobj.h"
#include "bongo/bson/bsonobjbuilder.h"
#include "bongo/db/query/cursor_response.h"

namespace bongo {

const char ViewResponseFormatter::kCountField[] = "n";
const char ViewResponseFormatter::kDistinctField[] = "values";
const char ViewResponseFormatter::kOkField[] = "ok";

ViewResponseFormatter::ViewResponseFormatter(BSONObj aggregationResponse)
    : _response(std::move(aggregationResponse)) {}

Status ViewResponseFormatter::appendAsCountResponse(BSONObjBuilder* resultBuilder) {
    auto cursorResponse = CursorResponse::parseFromBSON(_response);
    if (!cursorResponse.isOK())
        return cursorResponse.getStatus();

    auto cursorFirstBatch = cursorResponse.getValue().getBatch();
    if (cursorFirstBatch.empty()) {
        // There were no results from aggregation, so the count is zero.
        resultBuilder->append(kCountField, 0);
    } else {
        invariant(cursorFirstBatch.size() == 1);
        auto countObj = cursorFirstBatch.back();
        resultBuilder->append(kCountField, countObj["count"].Int());
    }
    resultBuilder->append(kOkField, 1);
    return Status::OK();
}

Status ViewResponseFormatter::appendAsDistinctResponse(BSONObjBuilder* resultBuilder) {
    auto cursorResponse = CursorResponse::parseFromBSON(_response);
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

    resultBuilder->append(kOkField, 1);
    return Status::OK();
}
}  // namespace bongo
