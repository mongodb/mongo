/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/index/geo/geo_key_extraction_failure_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <string_view>
#include <utility>

namespace mongo {
namespace {

// Nested under errInfo so drivers surface these fields to users as details; fields placed
// elsewhere on the writeError are dropped before reaching the user.
constexpr std::string_view kErrInfoFieldName = "errInfo";
constexpr std::string_view kFailingPathFieldName = "failingPath";
constexpr std::string_view kUnderlyingCodeFieldName = "underlyingCode";
constexpr std::string_view kUnderlyingReasonFieldName = "underlyingReason";
constexpr std::string_view kFailingElementFieldName = "failingElement";

}  // namespace

template <class Derived, ErrorCodes::Error Code>
void GeoKeyExtractionFailureInfoBase<Derived, Code>::serialize(BSONObjBuilder* builder) const {
    BSONObjBuilder errInfo(builder->subobjStart(kErrInfoFieldName));
    errInfo.append(kFailingPathFieldName, _failingPath);
    errInfo.append(kUnderlyingCodeFieldName, static_cast<int>(_underlyingStatus.code()));
    errInfo.append(kUnderlyingReasonFieldName, _underlyingStatus.reason());
    errInfo.append(kFailingElementFieldName, _failingElement);
}

template <class Derived, ErrorCodes::Error Code>
std::shared_ptr<const ErrorExtraInfo> GeoKeyExtractionFailureInfoBase<Derived, Code>::parse(
    const BSONObj& outer) {
    // Read from errInfo to match serialize(). This runs while rebuilding a Status from the wire,
    // where a throw would crash the receiving process, so fall back to an empty object instead of
    // failing on a missing or malformed errInfo.
    const auto errInfoElem = outer[kErrInfoFieldName];
    const auto obj = errInfoElem.isABSONObj() ? errInfoElem.Obj() : BSONObj{};

    auto failingPath = obj[kFailingPathFieldName].str();
    auto code = static_cast<ErrorCodes::Error>(obj[kUnderlyingCodeFieldName].safeNumberInt());
    auto reason = obj[kUnderlyingReasonFieldName].str();
    const auto failingElementElem = obj[kFailingElementFieldName];
    auto failingElement =
        failingElementElem.isABSONObj() ? failingElementElem.Obj().getOwned() : BSONObj{};
    return std::make_shared<const Derived>(
        std::move(failingPath), Status(code, std::move(reason)), std::move(failingElement));
}

template class GeoKeyExtractionFailureInfoBase<GeoKeyExtractionFailureInfo,
                                               ErrorCodes::GeoKeyExtractionFailed>;
template class GeoKeyExtractionFailureInfoBase<GeoKeyExtractionFailureTimeseriesInfo,
                                               ErrorCodes::GeoKeyExtractionFailedTimeseries>;

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(GeoKeyExtractionFailureInfo);
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(GeoKeyExtractionFailureTimeseriesInfo);

}  // namespace mongo
