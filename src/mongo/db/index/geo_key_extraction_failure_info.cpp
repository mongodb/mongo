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

#include "mongo/db/index/geo_key_extraction_failure_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <utility>

namespace mongo {
namespace {

constexpr auto kFailingPathFieldName = "failingPath"_sd;
constexpr auto kUnderlyingCodeFieldName = "underlyingCode"_sd;
constexpr auto kUnderlyingReasonFieldName = "underlyingReason"_sd;
constexpr auto kFailingElementFieldName = "failingElement"_sd;

}  // namespace

template <class Derived, ErrorCodes::Error Code>
void GeoKeyExtractionFailureInfoBase<Derived, Code>::serialize(BSONObjBuilder* builder) const {
    builder->append(kFailingPathFieldName, _failingPath);
    builder->append(kUnderlyingCodeFieldName, static_cast<int>(_underlyingStatus.code()));
    builder->append(kUnderlyingReasonFieldName, _underlyingStatus.reason());
    builder->append(kFailingElementFieldName, _failingElement);
}

template <class Derived, ErrorCodes::Error Code>
std::shared_ptr<const ErrorExtraInfo> GeoKeyExtractionFailureInfoBase<Derived, Code>::parse(
    const BSONObj& obj) {
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
