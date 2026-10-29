// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ifr_flag_retry_info.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {
namespace {

constexpr std::string_view kDisabledFlagName = "disabledFlagName";

BSONObj serializeIFRFlagRetryInfoToBson(const IFRFlagRetryInfo& info) {
    BSONObjBuilder bob;
    info.serialize(&bob);
    return bob.obj();
};

std::shared_ptr<const IFRFlagRetryInfo> parseIFRFlagRetryInfoFromBson(BSONObj obj) {
    std::shared_ptr<const ErrorExtraInfo> basic = IFRFlagRetryInfo::parse(obj);
    ASSERT(basic);
    auto info = std::dynamic_pointer_cast<const IFRFlagRetryInfo>(std::move(basic));
    ASSERT(info);
    return info;
}

TEST(IFRFlagRetryInfo, ConstructorAndGetter) {
    const std::string flagName = "testFlag";
    IFRFlagRetryInfo info(flagName);
    ASSERT_EQ(info.getDisabledFlagName(), flagName);
    ASSERT_EQ(info.code, ErrorCodes::IFRFlagRetry);
}

TEST(IFRFlagRetryInfo, Serialize) {
    const std::string flagName = "myIFRFlag";
    auto obj = serializeIFRFlagRetryInfoToBson(IFRFlagRetryInfo(flagName));
    auto el = obj.getField(kDisabledFlagName);
    ASSERT_FALSE(el.eoo());
    ASSERT_EQ(el.type(), BSONType::string);
    ASSERT_EQ(el.String(), flagName);
}

TEST(IFRFlagRetryInfo, ParseSuccess) {
    const std::string flagName = "parsedFlag";
    auto ifrInfo = parseIFRFlagRetryInfoFromBson(BSON(kDisabledFlagName << flagName));
    ASSERT_EQ(ifrInfo->getDisabledFlagName(), flagName);
}

TEST(IFRFlagRetryInfo, ParseMissingField) {
    ASSERT_THROWS_CODE(
        IFRFlagRetryInfo::parse(BSON("someOtherField" << "value")), AssertionException, 11577000);
}

TEST(IFRFlagRetryInfo, ParseEmptyObject) {
    ASSERT_THROWS_CODE(IFRFlagRetryInfo::parse(BSONObj()), AssertionException, 11577000);
}

TEST(IFRFlagRetryInfo, RoundTrip) {
    const std::string originalFlagName = "roundTripFlag";
    BSONObj originalBson = serializeIFRFlagRetryInfoToBson(IFRFlagRetryInfo(originalFlagName));
    auto ifrInfo = parseIFRFlagRetryInfoFromBson(originalBson);
    ASSERT_EQ(ifrInfo->getDisabledFlagName(), originalFlagName);
    ASSERT_BSONOBJ_EQ(serializeIFRFlagRetryInfoToBson(*ifrInfo), originalBson);
}

TEST(IFRFlagRetryInfo, StatusIntegration) {
    const std::string flagName = "statusFlag";
    Status status(
        ErrorCodes::IFRFlagRetry, "Test error message", BSON(kDisabledFlagName << flagName));
    ASSERT_EQ(status.code(), ErrorCodes::IFRFlagRetry);
    ASSERT(status.extraInfo());
    auto ifrInfo = status.extraInfo<IFRFlagRetryInfo>();
    ASSERT(ifrInfo);
    ASSERT_EQ(ifrInfo->getDisabledFlagName(), flagName);
}

TEST(IFRFlagRetryInfo, ParseWrongType) {
    ASSERT_THROWS(IFRFlagRetryInfo::parse(BSON(kDisabledFlagName << 12345)), DBException);
}

}  // namespace
}  // namespace mongo
