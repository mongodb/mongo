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

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
// (Generic FCV reference): used for testing, should exist across LTS binary versions
using GenericFCV = multiversion::GenericFCV;
using FCV = multiversion::FeatureCompatibilityVersion;

TEST(VersionContextTest, DefaultConstructorDoesNotInitializeOFCV) {
    VersionContext vCtx;
    ASSERT_FALSE(vCtx.getOperationFCV().has_value());
}

TEST(VersionContextTest, NoVersionContextHasNoOperationFCV) {
    ASSERT_FALSE(kNoVersionContext.getOperationFCV().has_value());
}

TEST(VersionContextTest, VersionContextIgnoredHasNoOperationFCV) {
    ASSERT_FALSE(kVersionContextIgnored.getOperationFCV().has_value());
}

TEST(VersionContextTest, FCVConstructorInitializesOFCVToLatest) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx(GenericFCV::kLatest);
    ASSERT_TRUE(vCtx.getOperationFCV().has_value());
    ASSERT_EQ(vCtx.getOperationFCV()->getVersion(), GenericFCV::kLatest);
}

TEST(VersionContextTest, FCVSnapshotConstructorInitializesOFCVToLatest) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx(ServerGlobalParams::FCVSnapshot{GenericFCV::kLatest});
    ASSERT_TRUE(vCtx.getOperationFCV().has_value());
    ASSERT_EQ(vCtx.getOperationFCV()->getVersion(), GenericFCV::kLatest);
}

TEST(VersionContextTest, CopyConstructorInitializesOFCV) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx{GenericFCV::kLatest};
    VersionContext vCtxCopy{vCtx};
    ASSERT_TRUE(vCtxCopy.getOperationFCV().has_value());
    ASSERT_EQ(vCtxCopy.getOperationFCV()->getVersion(), GenericFCV::kLatest);
}

TEST(VersionContextTest, AssignmentOperatorSetsOFCV) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtxA{GenericFCV::kLatest};
    VersionContext vCtxB;
    vCtxB = vCtxA;
    ASSERT_TRUE(vCtxB.getOperationFCV().has_value());
    ASSERT_EQ(vCtxB.getOperationFCV()->getVersion(), GenericFCV::kLatest);
}

TEST(VersionContextTest, SetOFCVWithFCV) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx;
    vCtx.setOperationFCV(GenericFCV::kLatest);
    ASSERT_TRUE(vCtx.getOperationFCV().has_value());
    ASSERT_EQ(vCtx.getOperationFCV()->getVersion(), GenericFCV::kLatest);
}

TEST(VersionContextTest, SetOFCVWithLatestFCVSnapshot) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx;
    vCtx.setOperationFCV(ServerGlobalParams::FCVSnapshot{GenericFCV::kLatest});
    ASSERT_TRUE(vCtx.getOperationFCV().has_value());
    ASSERT_EQ(vCtx.getOperationFCV()->getVersion(), GenericFCV::kLatest);
}

TEST(VersionContextTest, CheckForEquality) {
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

TEST(VersionContextTest, UpdatingIsIdempotent) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx{GenericFCV::kLatest};
    ASSERT_DOES_NOT_THROW(vCtx = VersionContext{GenericFCV::kLatest});
    ASSERT_EQ(vCtx.getOperationFCV()->getVersion(), GenericFCV::kLatest);
    ASSERT_DOES_NOT_THROW(vCtx.setOperationFCV(GenericFCV::kLatest));
    ASSERT_EQ(vCtx.getOperationFCV()->getVersion(), GenericFCV::kLatest);
    ASSERT_DOES_NOT_THROW(
        vCtx.setOperationFCV(ServerGlobalParams::FCVSnapshot{GenericFCV::kLatest}));
    ASSERT_EQ(vCtx.getOperationFCV()->getVersion(), GenericFCV::kLatest);
}

TEST(VersionContextTest, UpdatingThrowsWhenAlreadyInitializedWithDifferentValue) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx{GenericFCV::kLastLTS};
    ASSERT_THROWS_CODE(vCtx = VersionContext{GenericFCV::kLatest},
                       AssertionException,
                       ErrorCodes::AlreadyInitialized);
    ASSERT_EQ(vCtx.getOperationFCV()->getVersion(), GenericFCV::kLastLTS);
    ASSERT_THROWS_CODE(vCtx.setOperationFCV(GenericFCV::kLatest),
                       AssertionException,
                       ErrorCodes::AlreadyInitialized);
    ASSERT_EQ(vCtx.getOperationFCV()->getVersion(), GenericFCV::kLastLTS);
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    ASSERT_THROWS_CODE(vCtx.setOperationFCV(ServerGlobalParams::FCVSnapshot{GenericFCV::kLatest}),
                       AssertionException,
                       ErrorCodes::AlreadyInitialized);
    ASSERT_EQ(vCtx.getOperationFCV()->getVersion(), GenericFCV::kLastLTS);
}

TEST(VersionContextTest, SerializeDeserialize) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    // Verify that an uninitialized VersionContext can be serialized and deserialized.
    VersionContext vCtxA;
    VersionContext vCtxB{vCtxA.toBSON()};
    ASSERT_FALSE(vCtxB.getOperationFCV().has_value());
    // Verify that stable as well as transitory FCV states can be serialized and deserialized.
    const std::vector<FCV> fcvs{GenericFCV::kLatest, GenericFCV::kUpgradingFromLastLTSToLatest};
    for (const auto fcv : fcvs) {
        VersionContext vCtxA{fcv};
        VersionContext vCtxB{vCtxA.toBSON()};
        ASSERT_TRUE(vCtxB.getOperationFCV().has_value());
        ASSERT_EQ(vCtxB.getOperationFCV()->getVersion(), fcv);
    }
}

}  // namespace
}  // namespace mongo
