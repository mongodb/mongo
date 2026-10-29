// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
