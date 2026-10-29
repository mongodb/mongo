// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index/geo/geo_key_extraction_failure_info.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

constexpr auto kErrInfoFieldNameForTest = "errInfo"sv;
const BSONObj kFailingElement = BSON("type" << "Point" << "coordinates" << BSON_ARRAY("bad" << 0));

TEST(GeoKeyExtractionFailureInfo, SerializeNestsFieldsUnderErrInfo) {
    // Drivers surface writeErrors[].errInfo to users as details, so the structured fields must
    // live under an errInfo sub-object rather than at the top level.
    GeoKeyExtractionFailureInfo info(
        "features.geometry", Status(ErrorCodes::BadValue, "boom"), kFailingElement);
    BSONObjBuilder builder;
    info.serialize(&builder);
    BSONObj obj = builder.obj();

    ASSERT(obj.hasField(kErrInfoFieldNameForTest)) << obj;
    ASSERT(obj[kErrInfoFieldNameForTest].isABSONObj()) << obj;
    // Nothing leaks to the top level alongside errInfo.
    ASSERT_FALSE(obj.hasField("failingPath")) << obj;

    BSONObj errInfo = obj[kErrInfoFieldNameForTest].Obj();
    ASSERT_EQ(errInfo["failingPath"].str(), "features.geometry");
    ASSERT_EQ(errInfo["underlyingCode"].numberInt(), static_cast<int>(ErrorCodes::BadValue));
    ASSERT_EQ(errInfo["underlyingReason"].str(), "boom");
    ASSERT_BSONOBJ_EQ(errInfo["failingElement"].Obj(), kFailingElement);
}

TEST(GeoKeyExtractionFailureInfo, ParseToleratesMissingErrInfo) {
    // A missing errInfo must yield empty fields, not throw: parse() runs while rebuilding a
    // Status from the wire, where a throw would crash the receiving process.
    auto parsed = std::dynamic_pointer_cast<const GeoKeyExtractionFailureInfo>(
        GeoKeyExtractionFailureInfo::parse(BSONObj{}));
    ASSERT(parsed);
    ASSERT_EQ(parsed->failingPath(), "");
    ASSERT_TRUE(parsed->failingElement().isEmpty());
}

TEST(GeoKeyExtractionFailureInfo, ParseToleratesMalformedErrInfo) {
    // Same crash-safety contract for a corrupt shape: a wrong-typed errInfo, or a valid errInfo
    // with wrong-typed inner fields, must yield empty fields rather than throw.
    const BSONObj cases[] = {
        BSON(kErrInfoFieldNameForTest << 5),
        BSON(kErrInfoFieldNameForTest << "scalar"),
        BSON(kErrInfoFieldNameForTest
             << BSON("failingPath" << 42 << "underlyingCode" << "nope"
                                   << "underlyingReason" << 7 << "failingElement" << "scalar")),
    };
    for (const auto& obj : cases) {
        auto parsed = std::dynamic_pointer_cast<const GeoKeyExtractionFailureInfo>(
            GeoKeyExtractionFailureInfo::parse(obj));
        ASSERT(parsed) << obj;
        ASSERT_EQ(parsed->failingPath(), "") << obj;
        ASSERT_EQ(parsed->underlyingStatus().code(), ErrorCodes::OK) << obj;
        ASSERT_TRUE(parsed->failingElement().isEmpty()) << obj;
    }
}

TEST(GeoKeyExtractionFailureInfo, ReconstructsThroughRegisteredParser) {
    // Exercise the production path: serialize the whole Status and rebuild it by error code
    // through the registered parser.
    Status original(GeoKeyExtractionFailureInfo(
                        "features.geometry", Status(ErrorCodes::BadValue, "boom"), kFailingElement),
                    "could not extract geo keys");
    BSONObjBuilder bob;
    original.serialize(&bob);

    Status roundTripped(
        ErrorCodes::GeoKeyExtractionFailed, "could not extract geo keys", bob.obj());
    auto info = roundTripped.extraInfo<GeoKeyExtractionFailureInfo>();
    ASSERT(info);
    ASSERT_EQ(info->failingPath(), "features.geometry");
    ASSERT_EQ(info->underlyingStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(info->underlyingStatus().reason(), "boom");
    ASSERT_BSONOBJ_EQ(info->failingElement(), kFailingElement);
}

TEST(GeoKeyExtractionFailureTimeseriesInfo, ReconstructsThroughRegisteredParser) {
    // The timeseries variant registers a separate parser, so confirm code 511 maps to it.
    Status original(GeoKeyExtractionFailureTimeseriesInfo(
                        "data.loc", Status(ErrorCodes::BadValue, "boom"), kFailingElement),
                    "could not extract geo keys");
    BSONObjBuilder bob;
    original.serialize(&bob);

    Status roundTripped(
        ErrorCodes::GeoKeyExtractionFailedTimeseries, "could not extract geo keys", bob.obj());
    auto info = roundTripped.extraInfo<GeoKeyExtractionFailureTimeseriesInfo>();
    ASSERT(info);
    ASSERT_EQ(info->failingPath(), "data.loc");
    ASSERT_BSONOBJ_EQ(info->failingElement(), kFailingElement);
}

}  // namespace
}  // namespace mongo
