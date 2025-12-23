/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/ifr_flag_retry_info.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

constexpr StringData kDisabledFlagName = "disabledFlagName";

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
