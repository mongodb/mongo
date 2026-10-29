// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ifr_unrecognized_flag_info.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/idl/ifr_sender_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

// Builds an IFRSenderVersion from its numeric components.
IFRSenderVersion version(int major, int minor, int patch = 0, int extra = 0) {
    IFRSenderVersion v;
    v.setMajor(major);
    v.setMinor(minor);
    v.setPatch(patch);
    v.setExtra(extra);
    return v;
}

// The BSON sub-object shape a serialized IFRSenderVersion takes.
BSONObj versionBson(int major, int minor, int patch = 0, int extra = 0) {
    return BSON("major" << major << "minor" << minor << "patch" << patch << "extra" << extra);
}

void assertVersionEq(const IFRSenderVersion& actual, const IFRSenderVersion& expected) {
    ASSERT_EQ(actual.getMajor(), expected.getMajor());
    ASSERT_EQ(actual.getMinor(), expected.getMinor());
    ASSERT_EQ(actual.getPatch(), expected.getPatch());
    ASSERT_EQ(actual.getExtra(), expected.getExtra());
}

BSONObj serializeToBson(const UnrecognizedIFRFlagInfo& info) {
    BSONObjBuilder bob;
    info.serialize(&bob);
    return bob.obj();
}

std::shared_ptr<const UnrecognizedIFRFlagInfo> parseFromBson(BSONObj obj) {
    std::shared_ptr<const ErrorExtraInfo> basic = UnrecognizedIFRFlagInfo::parse(obj);
    ASSERT(basic);
    auto info = std::dynamic_pointer_cast<const UnrecognizedIFRFlagInfo>(std::move(basic));
    ASSERT(info);
    return info;
}

TEST(UnrecognizedIFRFlagInfo, SingleFlagConstructorAndGetters) {
    UnrecognizedIFRFlagInfo info({{"myFlag", true}}, version(9, 0));
    ASSERT_EQ(info.getFlags().size(), 1u);
    ASSERT_EQ(info.getFlags().at("myFlag"), true);
    assertVersionEq(info.getSenderVersion(), version(9, 0));
    ASSERT_EQ(info.code, ErrorCodes::UnrecognizedIFRFlag);
}

TEST(UnrecognizedIFRFlagInfo, MultiFlagConstructorAndGetters) {
    UnrecognizedIFRFlagInfo info({{"flagA", true}, {"flagB", false}}, version(9, 0));
    ASSERT_EQ(info.getFlags().size(), 2u);
    ASSERT_EQ(info.getFlags().at("flagA"), true);
    ASSERT_EQ(info.getFlags().at("flagB"), false);
    assertVersionEq(info.getSenderVersion(), version(9, 0));
}

TEST(UnrecognizedIFRFlagInfo, SerializeSingleFlag) {
    auto obj =
        serializeToBson(UnrecognizedIFRFlagInfo({{"testFlag", false}}, version(8, 1, 2, -3)));
    ASSERT(obj.hasField("flags"));
    ASSERT_EQ(obj["flags"].type(), BSONType::object);
    auto flagsObj = obj["flags"].Obj();
    ASSERT_EQ(flagsObj.nFields(), 1);
    ASSERT_EQ(flagsObj["testFlag"].Bool(), false);
    ASSERT_EQ(obj["senderVersion"].type(), BSONType::object);
    auto versionObj = obj["senderVersion"].Obj();
    ASSERT_EQ(versionObj["major"].Int(), 8);
    ASSERT_EQ(versionObj["minor"].Int(), 1);
    ASSERT_EQ(versionObj["patch"].Int(), 2);
    ASSERT_EQ(versionObj["extra"].Int(), -3);
}

TEST(UnrecognizedIFRFlagInfo, SerializeMultipleFlags) {
    auto obj = serializeToBson(
        UnrecognizedIFRFlagInfo({{"flagA", true}, {"flagB", false}}, version(9, 0)));
    auto flagsObj = obj["flags"].Obj();
    ASSERT_EQ(flagsObj.nFields(), 2);
    ASSERT_EQ(flagsObj["flagA"].Bool(), true);
    ASSERT_EQ(flagsObj["flagB"].Bool(), false);
    ASSERT_EQ(obj["senderVersion"].Obj()["major"].Int(), 9);
}

TEST(UnrecognizedIFRFlagInfo, ParseSingleFlag) {
    BSONObj obj =
        BSON("flags" << BSON("parsedFlag" << true) << "senderVersion" << versionBson(9, 0));
    auto info = parseFromBson(obj);
    ASSERT_EQ(info->getFlags().size(), 1u);
    ASSERT_EQ(info->getFlags().at("parsedFlag"), true);
    assertVersionEq(info->getSenderVersion(), version(9, 0));
}

TEST(UnrecognizedIFRFlagInfo, ParseMultipleFlags) {
    BSONObj obj = BSON("flags" << BSON("flagA" << true << "flagB" << false) << "senderVersion"
                               << versionBson(9, 0));
    auto info = parseFromBson(obj);
    ASSERT_EQ(info->getFlags().size(), 2u);
    ASSERT_EQ(info->getFlags().at("flagA"), true);
    ASSERT_EQ(info->getFlags().at("flagB"), false);
}

TEST(UnrecognizedIFRFlagInfo, ParseDuplicateFlagNamesDeduped) {
    // BSON may carry a repeated field name; parsing must collapse it to a single entry.
    BSONObj obj = BSON("flags" << BSON("dup" << true << "dup" << false) << "senderVersion"
                               << versionBson(9, 0));
    auto info = parseFromBson(obj);
    ASSERT_EQ(info->getFlags().size(), 1u);
    ASSERT_EQ(info->getFlags().at("dup"), false);
}

TEST(UnrecognizedIFRFlagInfo, ParseMissingFlagsField) {
    ASSERT_THROWS_CODE(UnrecognizedIFRFlagInfo::parse(BSON("senderVersion" << versionBson(9, 0))),
                       AssertionException,
                       13024000);
}

TEST(UnrecognizedIFRFlagInfo, ParseMissingSenderVersion) {
    ASSERT_THROWS_CODE(UnrecognizedIFRFlagInfo::parse(BSON("flags" << BSON("f" << true))),
                       AssertionException,
                       13024001);
}

TEST(UnrecognizedIFRFlagInfo, ParseFlagsFieldWrongType) {
    ASSERT_THROWS_CODE(
        UnrecognizedIFRFlagInfo::parse(BSON("flags" << 42 << "senderVersion" << versionBson(9, 0))),
        AssertionException,
        13024004);
}

TEST(UnrecognizedIFRFlagInfo, ParseSenderVersionWrongType) {
    ASSERT_THROWS_CODE(
        UnrecognizedIFRFlagInfo::parse(BSON("flags" << BSON("f" << true) << "senderVersion" << 42)),
        AssertionException,
        13024002);
}

TEST(UnrecognizedIFRFlagInfo, ParseFlagEntryWrongValueType) {
    ASSERT_THROWS_CODE(UnrecognizedIFRFlagInfo::parse(
                           BSON("flags" << BSON("f" << 1) << "senderVersion" << versionBson(9, 0))),
                       AssertionException,
                       13024003);
}

TEST(UnrecognizedIFRFlagInfo, ParseEmptyObject) {
    ASSERT_THROWS_CODE(UnrecognizedIFRFlagInfo::parse(BSONObj()), AssertionException, 13024000);
}

TEST(UnrecognizedIFRFlagInfo, RoundTrip) {
    UnrecognizedIFRFlagInfo original({{"roundTripA", true}, {"roundTripB", false}},
                                     version(9, 0, 1, -2));
    auto bson = serializeToBson(original);
    auto parsed = parseFromBson(bson);
    ASSERT(parsed->getFlags() == original.getFlags());
    assertVersionEq(parsed->getSenderVersion(), original.getSenderVersion());
    ASSERT_BSONOBJ_EQ(serializeToBson(*parsed), bson);
}

TEST(UnrecognizedIFRFlagInfo, StatusIntegration) {
    Status status(UnrecognizedIFRFlagInfo({{"statusFlag", false}}, version(9, 0)),
                  "Test error message");
    ASSERT_EQ(status.code(), ErrorCodes::UnrecognizedIFRFlag);
    ASSERT(status.extraInfo());
    auto info = status.extraInfo<UnrecognizedIFRFlagInfo>();
    ASSERT(info);
    ASSERT_EQ(info->getFlags().size(), 1u);
    ASSERT_EQ(info->getFlags().at("statusFlag"), false);
    assertVersionEq(info->getSenderVersion(), version(9, 0));
}

TEST(UnrecognizedIFRFlagInfo, MakeStatusSingleFlag) {
    auto status = makeUnrecognizedIFRFlagStatus({{"flagX", true}}, version(9, 0));
    ASSERT_EQ(status.code(), ErrorCodes::UnrecognizedIFRFlag);
    auto info = status.extraInfo<UnrecognizedIFRFlagInfo>();
    ASSERT(info);
    ASSERT_EQ(info->getFlags().size(), 1u);
    ASSERT_EQ(info->getFlags().at("flagX"), true);
    assertVersionEq(info->getSenderVersion(), version(9, 0));
    ASSERT_STRING_CONTAINS(status.reason(), "Received unknown IFR flag 'flagX'");
    ASSERT_STRING_CONTAINS(status.reason(), "version 9.0.0");
}

TEST(UnrecognizedIFRFlagInfo, MakeStatusMultipleFlagsUsesPluralWording) {
    auto status = makeUnrecognizedIFRFlagStatus({{"flagA", true}, {"flagB", false}}, version(8, 1));
    ASSERT_EQ(status.code(), ErrorCodes::UnrecognizedIFRFlag);
    auto info = status.extraInfo<UnrecognizedIFRFlagInfo>();
    ASSERT(info);
    ASSERT_EQ(info->getFlags().size(), 2u);
    ASSERT_EQ(info->getFlags().at("flagA"), true);
    ASSERT_EQ(info->getFlags().at("flagB"), false);
    ASSERT_STRING_CONTAINS(status.reason(), "Received 2 unknown IFR flags");
    ASSERT_STRING_CONTAINS(status.reason(), "version 8.1.0");
}

}  // namespace
}  // namespace mongo
