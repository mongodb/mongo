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

TEST(VersionContextTest, DefaultConstructorInitializesOFCVToUnset) {
    VersionContext vCtx;
    ASSERT_FALSE(vCtx.getOperationFCV().isVersionInitialized());
}

TEST(VersionContextTest, FCVConstructorInitializesOFCVToLatest) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx(GenericFCV::kLatest);
    ASSERT_TRUE(vCtx.getOperationFCV().isVersionInitialized());
    ASSERT_EQ(vCtx.getOperationFCV().getVersion(), GenericFCV::kLatest);
}

TEST(VersionContextTest, FCVSnapshotConstructorInitializesOFCVToUnsetWhenSnapshotUnset) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx(ServerGlobalParams::FCVSnapshot{FCV::kUnsetDefaultLastLTSBehavior});
    ASSERT_FALSE(vCtx.getOperationFCV().isVersionInitialized());
}

TEST(VersionContextTest, FCVSnapshotConstructorInitializesOFCVToLatest) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx(ServerGlobalParams::FCVSnapshot{GenericFCV::kLatest});
    ASSERT_TRUE(vCtx.getOperationFCV().isVersionInitialized());
    ASSERT_EQ(vCtx.getOperationFCV().getVersion(), GenericFCV::kLatest);
}

TEST(VersionContextTest, CopyConstructorInitializesOFCV) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx{GenericFCV::kLatest};
    VersionContext vCtxCopy{vCtx};
    ASSERT_TRUE(vCtxCopy.getOperationFCV().isVersionInitialized());
    ASSERT_EQ(vCtxCopy.getOperationFCV().getVersion(), GenericFCV::kLatest);
}

TEST(VersionContextTest, AssignmentOperatorSetsOFCV) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtxA{GenericFCV::kLatest};
    VersionContext vCtxB;
    vCtxB = vCtxA;
    ASSERT_TRUE(vCtxB.getOperationFCV().isVersionInitialized());
    ASSERT_EQ(vCtxB.getOperationFCV().getVersion(), GenericFCV::kLatest);
}

TEST(VersionContextTest, AssignmentOperatorThrowsWhenAlreadyInitialized) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx{GenericFCV::kLatest};
    ASSERT_THROWS_CODE(vCtx = VersionContext{GenericFCV::kLatest},
                       AssertionException,
                       ErrorCodes::AlreadyInitialized);
}

TEST(VersionContextTest, SetOFCVWithFCV) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx;
    vCtx.setOperationFCV(GenericFCV::kLatest);
    ASSERT_TRUE(vCtx.getOperationFCV().isVersionInitialized());
    ASSERT_EQ(vCtx.getOperationFCV().getVersion(), GenericFCV::kLatest);
}

TEST(VersionContextTest, SetOFCVWithUnsetFCVSnapshot) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx;
    vCtx.setOperationFCV(ServerGlobalParams::FCVSnapshot{FCV::kUnsetDefaultLastLTSBehavior});
    ASSERT_FALSE(vCtx.getOperationFCV().isVersionInitialized());
}

TEST(VersionContextTest, SetOFCVWithLatestFCVSnapshot) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx;
    vCtx.setOperationFCV(ServerGlobalParams::FCVSnapshot{GenericFCV::kLatest});
    ASSERT_TRUE(vCtx.getOperationFCV().isVersionInitialized());
    ASSERT_EQ(vCtx.getOperationFCV().getVersion(), GenericFCV::kLatest);
}

TEST(VersionContextTest, SetOFCVWithFCVThrowsWhenAlreadyInitialized) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx{GenericFCV::kLatest};
    ASSERT_THROWS_CODE(vCtx.setOperationFCV(GenericFCV::kLastLTS),
                       AssertionException,
                       ErrorCodes::AlreadyInitialized);
}

TEST(VersionContextTest, SetOFCVWithFCVSnapshotThrowsWhenAlreadyInitialized) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx{GenericFCV::kLatest};
    ASSERT_THROWS_CODE(vCtx.setOperationFCV(ServerGlobalParams::FCVSnapshot{GenericFCV::kLatest}),
                       AssertionException,
                       ErrorCodes::AlreadyInitialized);
}

TEST(VersionContextTest, SerializeDeserialize) {
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    // Verify that stable as well as transitory FCV states can be serialized and deserialized.
    const std::vector<FCV> fcvs{GenericFCV::kLatest, GenericFCV::kUpgradingFromLastLTSToLatest};
    for (const auto fcv : fcvs) {
        VersionContext vCtxA{fcv};
        VersionContext vCtxB{vCtxA.toBSON()};
        ASSERT_TRUE(vCtxB.getOperationFCV().isVersionInitialized());
        ASSERT_EQ(vCtxB.getOperationFCV().getVersion(), fcv);
    }
}

}  // namespace
}  // namespace mongo
