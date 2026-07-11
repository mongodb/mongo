// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace [[MONGO_MOD_PUBLIC]] mongo {

template <class Derived, ErrorCodes::Error Code>
class GeoKeyExtractionFailureInfoBase : public ErrorExtraInfo {
public:
    static constexpr auto code = Code;

    GeoKeyExtractionFailureInfoBase(std::string failingPath,
                                    Status underlyingStatus,
                                    BSONObj failingElement)
        : _failingPath(std::move(failingPath)),
          _underlyingStatus(std::move(underlyingStatus)),
          _failingElement(std::move(failingElement)) {}

    void serialize(BSONObjBuilder*) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

    std::string_view failingPath() const {
        return _failingPath;
    }
    const Status& underlyingStatus() const {
        return _underlyingStatus;
    }
    const BSONObj& failingElement() const {
        return _failingElement;
    }

private:
    std::string _failingPath;
    Status _underlyingStatus;
    BSONObj _failingElement;
};

class GeoKeyExtractionFailureInfo final
    : public GeoKeyExtractionFailureInfoBase<GeoKeyExtractionFailureInfo,
                                             ErrorCodes::GeoKeyExtractionFailed> {
public:
    using GeoKeyExtractionFailureInfoBase::GeoKeyExtractionFailureInfoBase;
};

class GeoKeyExtractionFailureTimeseriesInfo final
    : public GeoKeyExtractionFailureInfoBase<GeoKeyExtractionFailureTimeseriesInfo,
                                             ErrorCodes::GeoKeyExtractionFailedTimeseries> {
public:
    using GeoKeyExtractionFailureInfoBase::GeoKeyExtractionFailureInfoBase;
};

}  // namespace mongo
