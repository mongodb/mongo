// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/version_context.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo {
// (Generic FCV reference): used for testing, should exist across LTS binary versions
using GenericFCV = multiversion::GenericFCV;
using FCV = multiversion::FeatureCompatibilityVersion;

class VersionContextTest : public unittest::Test {
public:
    boost::optional<ServerGlobalParams::FCVSnapshot> getOFCV(const VersionContext& vCtx) const {
        return vCtx.getOperationFCV(VersionContext::Passkey());
    }
};

TEST_F(VersionContextTest, DefaultConstructorDoesNotInitializeOFCV) {
    VersionContext vCtx;
    ASSERT_FALSE(getOFCV(vCtx).has_value());
}

TEST_F(VersionContextTest, NoVersionContextHasNoOperationFCV) {
    ASSERT_FALSE(getOFCV(kNoVersionContext).has_value());
}

TEST_F(VersionContextTest, VersionContextIgnoredHasNoOperationFCV) {
    ASSERT_FALSE(getOFCV(kVersionContextIgnored_UNSAFE).has_value());
}

TEST_F(VersionContextTest, FCVConstructorInitializesOFCVToLatest) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx(GenericFCV::kLatest);
    ASSERT_TRUE(getOFCV(vCtx).has_value());
    ASSERT_EQ(getOFCV(vCtx)->getVersion(), GenericFCV::kLatest);
}

TEST_F(VersionContextTest, FCVConstructorInitializesOFCVToUninitialized) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx{multiversion::FeatureCompatibilityVersion::kUnsetDefaultLastLTSBehavior};
    ASSERT_TRUE(getOFCV(vCtx).has_value());
    ASSERT_FALSE(getOFCV(vCtx)->isVersionInitialized());
}

TEST_F(VersionContextTest, FCVSnapshotConstructorInitializesOFCVToLatest) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx(ServerGlobalParams::FCVSnapshot{GenericFCV::kLatest});
    ASSERT_TRUE(getOFCV(vCtx).has_value());
    ASSERT_EQ(getOFCV(vCtx)->getVersion(), GenericFCV::kLatest);
}

TEST_F(VersionContextTest, CopyConstructorInitializesOFCV) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx{GenericFCV::kLatest};
    VersionContext vCtxCopy{vCtx};
    ASSERT_TRUE(getOFCV(vCtxCopy).has_value());
    ASSERT_EQ(getOFCV(vCtxCopy)->getVersion(), GenericFCV::kLatest);
}

TEST_F(VersionContextTest, AssignmentOperatorSetsOFCV) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtxA{GenericFCV::kLatest};
    VersionContext vCtxB;
    vCtxB = vCtxA;
    ASSERT_TRUE(getOFCV(vCtxB).has_value());
    ASSERT_EQ(getOFCV(vCtxB)->getVersion(), GenericFCV::kLatest);
}

TEST_F(VersionContextTest, SetOFCVWithFCV) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx;
    vCtx.setOperationFCV(GenericFCV::kLatest);
    ASSERT_TRUE(getOFCV(vCtx).has_value());
    ASSERT_EQ(getOFCV(vCtx)->getVersion(), GenericFCV::kLatest);
}

TEST_F(VersionContextTest, SetOFCVWithLatestFCVSnapshot) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx;
    vCtx.setOperationFCV(ServerGlobalParams::FCVSnapshot{GenericFCV::kLatest});
    ASSERT_TRUE(getOFCV(vCtx).has_value());
    ASSERT_EQ(getOFCV(vCtx)->getVersion(), GenericFCV::kLatest);
}

TEST_F(VersionContextTest, CheckForEquality) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    // Metadata
    ASSERT_TRUE(VersionContext{GenericFCV::kLatest} == VersionContext{GenericFCV::kLatest});
    ASSERT_FALSE(VersionContext{GenericFCV::kLatest} == VersionContext{GenericFCV::kLastLTS});
    // Tags
    ASSERT_TRUE(VersionContext{VersionContext::OutsideOperationTag{}} ==
                VersionContext{VersionContext::OutsideOperationTag{}});
    ASSERT_FALSE(VersionContext{VersionContext::OutsideOperationTag{}} ==
                 VersionContext{VersionContext::IgnoreOFCVTag{}});
    // Metadata + Tag
    ASSERT_FALSE(VersionContext{GenericFCV::kLatest} ==
                 VersionContext{VersionContext::OutsideOperationTag{}});
}

TEST_F(VersionContextTest, UpdatingIsIdempotent) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx{GenericFCV::kLatest};
    ASSERT_DOES_NOT_THROW(vCtx = VersionContext{GenericFCV::kLatest});
    ASSERT_EQ(getOFCV(vCtx)->getVersion(), GenericFCV::kLatest);
    ASSERT_DOES_NOT_THROW(vCtx.setOperationFCV(GenericFCV::kLatest));
    ASSERT_EQ(getOFCV(vCtx)->getVersion(), GenericFCV::kLatest);
    ASSERT_DOES_NOT_THROW(
        vCtx.setOperationFCV(ServerGlobalParams::FCVSnapshot{GenericFCV::kLatest}));
    ASSERT_EQ(getOFCV(vCtx)->getVersion(), GenericFCV::kLatest);
}

TEST_F(VersionContextTest, UpdatingThrowsWhenAlreadyInitializedWithDifferentValue) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx{GenericFCV::kLastLTS};
    ASSERT_THROWS_CODE(vCtx = VersionContext{GenericFCV::kLatest},
                       AssertionException,
                       ErrorCodes::AlreadyInitialized);
    ASSERT_EQ(getOFCV(vCtx)->getVersion(), GenericFCV::kLastLTS);
    ASSERT_THROWS_CODE(vCtx.setOperationFCV(GenericFCV::kLatest),
                       AssertionException,
                       ErrorCodes::AlreadyInitialized);
    ASSERT_EQ(getOFCV(vCtx)->getVersion(), GenericFCV::kLastLTS);
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    ASSERT_THROWS_CODE(vCtx.setOperationFCV(ServerGlobalParams::FCVSnapshot{GenericFCV::kLatest}),
                       AssertionException,
                       ErrorCodes::AlreadyInitialized);
    ASSERT_EQ(getOFCV(vCtx)->getVersion(), GenericFCV::kLastLTS);
}

TEST_F(VersionContextTest, SerializeDeserialize) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    // Verify that stable, transitory, as well as uninitialized FCV states can be serialized and
    // deserialized.
    const std::vector<FCV> fcvs{
        GenericFCV::kLatest,
        GenericFCV::kUpgradingFromLastLTSToLatest,
        multiversion::FeatureCompatibilityVersion::kUnsetDefaultLastLTSBehavior};
    for (const auto fcv : fcvs) {
        VersionContext vCtxA{fcv};
        VersionContext vCtxB{vCtxA.toBSON()};
        ASSERT_TRUE(getOFCV(vCtxB).has_value());
        ASSERT_EQ(vCtxB, vCtxA);
    }
}

// (Generic FCV reference): used for testing, should exist across LTS binary version
constexpr auto kLastLTSFCVString = multiversion::toString(GenericFCV::kLastLTS);
constexpr auto kLastContinuousFCVString = multiversion::toString(GenericFCV::kLastContinuous);
constexpr auto kLatestFCVString = multiversion::toString(GenericFCV::kLatest);
constexpr auto kUninitializedFCVString =
    multiversion::toString(multiversion::FeatureCompatibilityVersion::kUnsetDefaultLastLTSBehavior);

VersionContext makeFromOFCVString(std::string_view ofcvString) {
    return VersionContext{BSON(VersionContextMetadata::kOFCVFieldName << ofcvString)};
}
VersionContext makeFromUpgradingOFCVString(std::string_view from, std::string_view to) {
    return makeFromOFCVString(fmt::format("upgrading from {} to {}", from, to));
}
VersionContext makeFromDowngradingOFCVString(std::string_view from, std::string_view to) {
    return makeFromOFCVString(fmt::format("downgrading from {} to {}", from, to));
}

TEST_F(VersionContextTest, DeserializeFromValidDocument) {
    // (Generic FCV reference): used for testing, should exist across LTS binary version
    ASSERT_EQ(VersionContext{GenericFCV::kLastLTS}, makeFromOFCVString(kLastLTSFCVString));
    ASSERT_EQ(VersionContext{GenericFCV::kLastContinuous},
              makeFromOFCVString(kLastContinuousFCVString));
    ASSERT_EQ(VersionContext{GenericFCV::kLatest}, makeFromOFCVString(kLatestFCVString));
    ASSERT_EQ(
        VersionContext{multiversion::FeatureCompatibilityVersion::kUnsetDefaultLastLTSBehavior},
        makeFromOFCVString(kUninitializedFCVString));

    // (Generic FCV reference): used for testing, should exist across LTS binary version
    ASSERT_EQ(VersionContext{GenericFCV::kUpgradingFromLastLTSToLatest},
              makeFromUpgradingOFCVString(kLastLTSFCVString, kLatestFCVString));
    ASSERT_EQ(VersionContext{GenericFCV::kUpgradingFromLastContinuousToLatest},
              makeFromUpgradingOFCVString(kLastContinuousFCVString, kLatestFCVString));
    if constexpr (multiversion::GenericFCV::kLastLTS != multiversion::GenericFCV::kLastContinuous) {
        ASSERT_EQ(VersionContext{GenericFCV::kUpgradingFromLastLTSToLastContinuous},
                  makeFromUpgradingOFCVString(kLastLTSFCVString, kLastContinuousFCVString));
    }

    // (Generic FCV reference): used for testing, should exist across LTS binary version
    ASSERT_EQ(VersionContext{GenericFCV::kDowngradingFromLatestToLastLTS},
              makeFromDowngradingOFCVString(kLatestFCVString, kLastLTSFCVString));
    ASSERT_EQ(VersionContext{GenericFCV::kDowngradingFromLatestToLastContinuous},
              makeFromDowngradingOFCVString(kLatestFCVString, kLastContinuousFCVString));

    // Parsing is not strict, so unknown fields are tolerated
    ASSERT_EQ(VersionContext{GenericFCV::kLatest},
              VersionContext{BSON(VersionContextMetadata::kOFCVFieldName << kLatestFCVString
                                                                         << "dummy" << true)});
}

#define ASSERT_THROWS_BAD_VALUE(EXPRESSION) \
    ASSERT_THROWS_CODE(EXPRESSION, DBException, ErrorCodes::BadValue)

// Tests that deserializing an invalid input fails gracefully by throwing an exception.
// This is important since VersionContext is exposed as a generic request argument.
TEST_F(VersionContextTest, DeserializeFromInvalidDocument) {
    ASSERT_THROWS_CODE(VersionContext{BSONObj()}, DBException, ErrorCodes::IDLFailedToParse);
    ASSERT_THROWS_CODE(
        VersionContext{BSON("dummy" << true)}, DBException, ErrorCodes::IDLFailedToParse);

    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString(""));
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString(" "));
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString("xyzzy"));
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString(fmt::format("{:x^1000000}", "")));  // Long string
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString(u8"今日は"_as_char_ptr));
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString(u8"😊"_as_char_ptr));
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString("2.0"));
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString("7.0"));
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString("99999999999999999999999999999999.0"));

    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString("invalid"));
    ASSERT_THROWS_BAD_VALUE(
        makeFromOFCVString(fmt::format(std::string_view("{}\0", 3), kLastLTSFCVString)));
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString(
        fmt::format(std::string_view("{}\0{}", 5), kLastLTSFCVString, kLatestFCVString)));
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString(fmt::format(" {}", kLastLTSFCVString)));
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString(fmt::format("{} ", kLatestFCVString)));
    ASSERT_THROWS_BAD_VALUE(makeFromUpgradingOFCVString(kLatestFCVString, kLastLTSFCVString));
    ASSERT_THROWS_BAD_VALUE(makeFromDowngradingOFCVString(kLastLTSFCVString, kLatestFCVString));
    ASSERT_THROWS_BAD_VALUE(makeFromUpgradingOFCVString(kLastLTSFCVString, kLastLTSFCVString));
    ASSERT_THROWS_BAD_VALUE(makeFromDowngradingOFCVString(kLatestFCVString, kLatestFCVString));
}

// Tests the behavior of the in-memory flag for propagation of VersionContext across shards
TEST_F(VersionContextTest, PropagationAcrossShardsFlag) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    const auto vCtx = VersionContext(GenericFCV::kLatest);
    const auto vCtxWithPropagation = vCtx.withPropagationAcrossShards_UNSAFE();

    // The flag is disabled by default
    ASSERT_FALSE(VersionContext().canPropagateAcrossShards());
    ASSERT_FALSE(kNoVersionContext.canPropagateAcrossShards());
    ASSERT_FALSE(kVersionContextIgnored_UNSAFE.canPropagateAcrossShards());
    ASSERT_FALSE(vCtx.canPropagateAcrossShards());

    // Can create a VersionContext with the flag enabled
    ASSERT_TRUE(vCtxWithPropagation.canPropagateAcrossShards());

    // Copy and assignment conserve the flag
    ASSERT_TRUE(VersionContext{vCtxWithPropagation}.canPropagateAcrossShards());

    {
        VersionContext myVCtx;
        myVCtx = vCtxWithPropagation;
        ASSERT_TRUE(myVCtx.canPropagateAcrossShards());
        myVCtx = vCtx;
        ASSERT_FALSE(myVCtx.canPropagateAcrossShards());
    }

    // Resetting VersionContext disables the flag
    {
        VersionContext myVCtx{vCtxWithPropagation};
        myVCtx.resetToOperationWithoutOFCV();
        ASSERT_FALSE(myVCtx.canPropagateAcrossShards());
    }

    // Equality comparison ignores the flag
    ASSERT_EQ(vCtx, vCtxWithPropagation);

    // The flag is similarly not serialized or deserialized
    ASSERT_BSONOBJ_EQ(vCtx.toBSON(), vCtxWithPropagation.toBSON());
    ASSERT_FALSE(VersionContext{vCtxWithPropagation.toBSON()}.canPropagateAcrossShards());
}

}  // namespace mongo
