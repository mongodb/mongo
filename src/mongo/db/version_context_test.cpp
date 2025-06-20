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
#include "mongo/db/version_context.h"

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"

#include "src/mongo/bson/bsonmisc.h"
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
    // Verify that an uninitialized VersionContext can be serialized and deserialized.
    VersionContext vCtxA;
    VersionContext vCtxB{vCtxA.toBSON()};
    ASSERT_FALSE(getOFCV(vCtxB).has_value());
    // Verify that stable as well as transitory FCV states can be serialized and deserialized.
    const std::vector<FCV> fcvs{GenericFCV::kLatest, GenericFCV::kUpgradingFromLastLTSToLatest};
    for (const auto fcv : fcvs) {
        VersionContext vCtxA{fcv};
        VersionContext vCtxB{vCtxA.toBSON()};
        ASSERT_TRUE(getOFCV(vCtxB).has_value());
        ASSERT_EQ(getOFCV(vCtxB)->getVersion(), fcv);
    }
}

// (Generic FCV reference): used for testing, should exist across LTS binary version
constexpr auto kLastLTSFCVString = multiversion::toString(GenericFCV::kLastLTS);
constexpr auto kLastContinuousFCVString = multiversion::toString(GenericFCV::kLastContinuous);
constexpr auto kLatestFCVString = multiversion::toString(GenericFCV::kLatest);

VersionContext makeFromOFCVString(StringData ofcvString) {
    return VersionContext{BSON(VersionContextMetadata::kOFCVFieldName << ofcvString)};
}
VersionContext makeFromUpgradingOFCVString(StringData from, StringData to) {
    return makeFromOFCVString(fmt::format("upgrading from {} to {}", from, to));
}
VersionContext makeFromDowngradingOFCVString(StringData from, StringData to) {
    return makeFromOFCVString(fmt::format("downgrading from {} to {}", from, to));
}

TEST_F(VersionContextTest, DeserializeFromValidDocument) {
    ASSERT_EQ(VersionContext{}, VersionContext{BSONObj()});

    // (Generic FCV reference): used for testing, should exist across LTS binary version
    ASSERT_EQ(VersionContext{GenericFCV::kLastLTS}, makeFromOFCVString(kLastLTSFCVString));
    ASSERT_EQ(VersionContext{GenericFCV::kLastContinuous},
              makeFromOFCVString(kLastContinuousFCVString));
    ASSERT_EQ(VersionContext{GenericFCV::kLatest}, makeFromOFCVString(kLatestFCVString));

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
    ASSERT_EQ(VersionContext{}, VersionContext{BSON("dummy" << true)});
    ASSERT_EQ(VersionContext{GenericFCV::kLatest},
              VersionContext{BSON(VersionContextMetadata::kOFCVFieldName << kLatestFCVString
                                                                         << "dummy" << true)});
}

#define ASSERT_THROWS_BAD_VALUE(EXPRESSION) \
    ASSERT_THROWS_CODE(EXPRESSION, DBException, ErrorCodes::BadValue)

// Tests that deserializing an invalid input fails gracefully by throwing an exception.
// This is important since VersionContext is exposed as a generic request argument.
TEST_F(VersionContextTest, DeserializeFromInvalidDocument) {
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
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString("unset"));
    ASSERT_THROWS_BAD_VALUE(
        makeFromOFCVString(fmt::format(StringData("{}\0", 3), kLastLTSFCVString)));
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString(
        fmt::format(StringData("{}\0{}", 5), kLastLTSFCVString, kLatestFCVString)));
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString(fmt::format(" {}", kLastLTSFCVString)));
    ASSERT_THROWS_BAD_VALUE(makeFromOFCVString(fmt::format("{} ", kLatestFCVString)));
    ASSERT_THROWS_BAD_VALUE(makeFromUpgradingOFCVString(kLatestFCVString, kLastLTSFCVString));
    ASSERT_THROWS_BAD_VALUE(makeFromDowngradingOFCVString(kLastLTSFCVString, kLatestFCVString));
    ASSERT_THROWS_BAD_VALUE(makeFromUpgradingOFCVString(kLastLTSFCVString, kLastLTSFCVString));
    ASSERT_THROWS_BAD_VALUE(makeFromDowngradingOFCVString(kLatestFCVString, kLatestFCVString));
}

}  // namespace mongo
