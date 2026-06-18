/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/feature_compatibility_version_parser.h"

#include "mongo/db/feature_compatibility_version_document_gen.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/version/releases.h"

namespace mongo {
namespace {

using FCV = multiversion::FeatureCompatibilityVersion;
using GenericFCV = multiversion::GenericFCV;

// Test fixture for parser cases that exercise FCV documents with a 'phase' field set. Phase is
// only ever written under Symmetric FCV, so reading such a doc requires the flag enabled — the
// parser rejects it otherwise (see read-side guard in FeatureCompatibilityVersionParser::parse).
class FCVParserPhaseProjectionTest : public unittest::Test {
private:
    unittest::ServerParameterGuard _symmetricFCV{"featureFlagSymmetricFCV", true};
};

// Build a FeatureCompatibilityVersionDocument for an upgrade.
// For phases >= kEnableTargetFeatures, previousVersion is set to `version` to match
// what updateFeatureCompatibilityVersionDocument writes on disk (see
// feature_compatibility_version.cpp: setPreviousVersion(getTransitionFCVInfo(v).from)).
// TODO (SERVER-120670): The version becomes the targetVersion for phases >= kEnableTargetFeatures.
static FeatureCompatibilityVersionDocument makeUpgradeDoc(
    FCV version, FCV targetVersion, boost::optional<SetFCVPhaseEnum> phase = boost::none) {
    FeatureCompatibilityVersionDocument doc;
    doc.setVersion(version);
    doc.setTargetVersion(targetVersion);
    doc.setPhase(phase);
    if (phase && *phase >= SetFCVPhaseEnum::kEnableTargetFeatures) {
        doc.setPreviousVersion(version);
    }
    return doc;
}

// Build a FeatureCompatibilityVersionDocument for a downgrade (previousVersion = kLatest).
static FeatureCompatibilityVersionDocument makeDowngradeDoc(
    FCV version, boost::optional<SetFCVPhaseEnum> phase = boost::none) {
    FeatureCompatibilityVersionDocument doc;
    doc.setVersion(version);
    doc.setTargetVersion(version);
    doc.setPreviousVersion(GenericFCV::kLatest);
    doc.setPhase(phase);
    return doc;
}

// Build a FeatureCompatibilityVersionDocument with full control over each field, allowing fields
// to be omitted to exercise missing-field error paths in the parser.
static FeatureCompatibilityVersionDocument makeArbitraryFCVDoc(
    FCV version,
    boost::optional<FCV> targetVersion,
    boost::optional<FCV> previousVersion,
    boost::optional<SetFCVPhaseEnum> phase) {
    FeatureCompatibilityVersionDocument doc;
    doc.setVersion(version);
    if (targetVersion)
        doc.setTargetVersion(*targetVersion);
    if (previousVersion)
        doc.setPreviousVersion(*previousVersion);
    doc.setPhase(phase);
    return doc;
}

// ---------- Group 1: Phase-based projection tests ----------
//
// All cases share the same body: build a doc with a phase, parse it, check the result.
// kEnableTargetFeatures and kCommitAddedFeatures project to the target FCV; kStart, kPrepare,
// and kComplete project to the transitional FCV.

struct PhaseTestCase {
    std::string name;
    std::function<FeatureCompatibilityVersionDocument()> makeDoc;
    FCV expected;
};

class FCVParserPhaseTest : public FCVParserPhaseProjectionTest,
                           public ::testing::WithParamInterface<PhaseTestCase> {};

TEST_P(FCVParserPhaseTest, ParsesCorrectly) {
    const auto& p = GetParam();
    auto doc = p.makeDoc();
    auto result = FeatureCompatibilityVersionParser::parse(doc.toBSON());
    ASSERT_OK(result);
    ASSERT_EQ(result.getValue(), p.expected);
}

INSTANTIATE_TEST_SUITE_P(
    FCVParserPhaseProjection,
    FCVParserPhaseTest,
    ::testing::Values(
        // Phases that project to the target FCV.
        PhaseTestCase{"UpgradeLastLTSToLatestEnableTargetFeatures",
                      []() {
                          return makeUpgradeDoc(GenericFCV::kLastLTS,
                                                GenericFCV::kLatest,
                                                SetFCVPhaseEnum::kEnableTargetFeatures);
                      },
                      GenericFCV::kLatest},
        PhaseTestCase{"UpgradeLastLTSToLatestCommitAddedFeatures",
                      []() {
                          return makeUpgradeDoc(GenericFCV::kLastLTS,
                                                GenericFCV::kLatest,
                                                SetFCVPhaseEnum::kCommitAddedFeatures);
                      },
                      GenericFCV::kLatest},
        PhaseTestCase{"UpgradeLastContinuousToLatestEnableTargetFeatures",
                      []() {
                          return makeUpgradeDoc(GenericFCV::kLastContinuous,
                                                GenericFCV::kLatest,
                                                SetFCVPhaseEnum::kEnableTargetFeatures);
                      },
                      GenericFCV::kLatest},
        PhaseTestCase{"UpgradeLastContinuousToLatestCommitAddedFeatures",
                      []() {
                          return makeUpgradeDoc(GenericFCV::kLastContinuous,
                                                GenericFCV::kLatest,
                                                SetFCVPhaseEnum::kCommitAddedFeatures);
                      },
                      GenericFCV::kLatest},
        PhaseTestCase{"UpgradeLastLTSToLastContinuousEnableTargetFeatures",
                      []() {
                          return makeUpgradeDoc(GenericFCV::kLastLTS,
                                                GenericFCV::kLastContinuous,
                                                SetFCVPhaseEnum::kEnableTargetFeatures);
                      },
                      GenericFCV::kLastContinuous},
        PhaseTestCase{"UpgradeLastLTSToLastContinuousCommitAddedFeatures",
                      []() {
                          return makeUpgradeDoc(GenericFCV::kLastLTS,
                                                GenericFCV::kLastContinuous,
                                                SetFCVPhaseEnum::kCommitAddedFeatures);
                      },
                      GenericFCV::kLastContinuous},
        PhaseTestCase{"DowngradeLastLTSEnableTargetFeatures",
                      []() {
                          return makeDowngradeDoc(GenericFCV::kLastLTS,
                                                  SetFCVPhaseEnum::kEnableTargetFeatures);
                      },
                      GenericFCV::kLastLTS},
        PhaseTestCase{"DowngradeLastLTSCommitAddedFeatures",
                      []() {
                          return makeDowngradeDoc(GenericFCV::kLastLTS,
                                                  SetFCVPhaseEnum::kCommitAddedFeatures);
                      },
                      GenericFCV::kLastLTS},
        PhaseTestCase{"DowngradeLastContinuousEnableTargetFeatures",
                      []() {
                          return makeDowngradeDoc(GenericFCV::kLastContinuous,
                                                  SetFCVPhaseEnum::kEnableTargetFeatures);
                      },
                      GenericFCV::kLastContinuous},
        PhaseTestCase{"DowngradeLastContinuousCommitAddedFeatures",
                      []() {
                          return makeDowngradeDoc(GenericFCV::kLastContinuous,
                                                  SetFCVPhaseEnum::kCommitAddedFeatures);
                      },
                      GenericFCV::kLastContinuous},
        // Phases that project to the transitional FCV.
        PhaseTestCase{"UpgradeStart",
                      []() {
                          return makeUpgradeDoc(
                              GenericFCV::kLastLTS, GenericFCV::kLatest, SetFCVPhaseEnum::kStart);
                      },
                      GenericFCV::kUpgradingFromLastLTSToLatest},
        PhaseTestCase{"UpgradePrepare",
                      []() {
                          return makeUpgradeDoc(
                              GenericFCV::kLastLTS, GenericFCV::kLatest, SetFCVPhaseEnum::kPrepare);
                      },
                      GenericFCV::kUpgradingFromLastLTSToLatest},
        PhaseTestCase{"UpgradeComplete",
                      []() {
                          return makeUpgradeDoc(GenericFCV::kLastLTS,
                                                GenericFCV::kLatest,
                                                SetFCVPhaseEnum::kComplete);
                      },
                      GenericFCV::kUpgradingFromLastLTSToLatest},
        PhaseTestCase{
            "DowngradeStart",
            []() { return makeDowngradeDoc(GenericFCV::kLastLTS, SetFCVPhaseEnum::kStart); },
            GenericFCV::kDowngradingFromLatestToLastLTS},
        PhaseTestCase{
            "DowngradeComplete",
            []() { return makeDowngradeDoc(GenericFCV::kLastLTS, SetFCVPhaseEnum::kComplete); },
            GenericFCV::kDowngradingFromLatestToLastLTS}),
    [](const ::testing::TestParamInfo<PhaseTestCase>& info) { return info.param.name; });

// ---------- Group 2: No-phase tests (steady-state and legacy transitional) ----------

struct NoPhaseCaseInfo {
    std::string name;
    FCV version;
    boost::optional<FCV> targetVersion;
    bool withPreviousVersion;
    FCV expected;
};


class FCVParserNoPhaseTest : public FCVParserPhaseProjectionTest,
                             public ::testing::WithParamInterface<NoPhaseCaseInfo> {};

TEST_P(FCVParserNoPhaseTest, ParsesCorrectly) {
    const auto& p = GetParam();
    FeatureCompatibilityVersionDocument doc;
    doc.setVersion(p.version);
    if (p.targetVersion) {
        doc.setTargetVersion(*p.targetVersion);
    }
    if (p.withPreviousVersion) {
        doc.setPreviousVersion(GenericFCV::kLatest);
    }
    auto result = FeatureCompatibilityVersionParser::parse(doc.toBSON());
    ASSERT_OK(result);
    ASSERT_EQ(result.getValue(), p.expected);
}

INSTANTIATE_TEST_SUITE_P(
    FCVParserNoPhase,
    FCVParserNoPhaseTest,
    ::testing::Values(
        NoPhaseCaseInfo{
            "SteadyStateLatest", GenericFCV::kLatest, boost::none, false, GenericFCV::kLatest},
        // TODO: SERVER-127882: Once gFeatureFlagSymmetricFCV is permanently enabled (when 9.0
        // becomes lastLTS), the "legacy transitional" cases below become dead code, so let's remove
        // them
        NoPhaseCaseInfo{"UpgradingNoPhase",
                        GenericFCV::kLastLTS,
                        GenericFCV::kLatest,
                        false,
                        GenericFCV::kUpgradingFromLastLTSToLatest},
        NoPhaseCaseInfo{"DowngradingNoPhase",
                        GenericFCV::kLastLTS,
                        GenericFCV::kLastLTS,
                        true,
                        GenericFCV::kDowngradingFromLatestToLastLTS}),
    [](const ::testing::TestParamInfo<NoPhaseCaseInfo>& info) { return info.param.name; });

// ---------- Error paths for phase >= kEnableTargetFeatures ----------

struct PhaseErrorTestCase {
    std::string name;
    std::function<FeatureCompatibilityVersionDocument()> makeDoc;
    int expectedCode;
};

class FCVParserPhaseErrorTest : public FCVParserPhaseProjectionTest,
                                public ::testing::WithParamInterface<PhaseErrorTestCase> {};

TEST_P(FCVParserPhaseErrorTest, RejectsInvalidDocument) {
    const auto& p = GetParam();
    auto doc = p.makeDoc();
    auto result = FeatureCompatibilityVersionParser::parse(doc.toBSON());
    ASSERT_EQ(result.getStatus().code(), p.expectedCode);
}

INSTANTIATE_TEST_SUITE_P(
    FCVParserPhaseErrors,
    FCVParserPhaseErrorTest,
    ::testing::Values(
        PhaseErrorTestCase{"MissingTargetVersion",
                           []() {
                               return makeArbitraryFCVDoc(GenericFCV::kLastLTS,
                                                          boost::none,
                                                          GenericFCV::kLastLTS,
                                                          SetFCVPhaseEnum::kEnableTargetFeatures);
                           },
                           11948501},
        PhaseErrorTestCase{"MissingPreviousVersion",
                           []() {
                               return makeArbitraryFCVDoc(GenericFCV::kLastLTS,
                                                          GenericFCV::kLatest,
                                                          boost::none,
                                                          SetFCVPhaseEnum::kEnableTargetFeatures);
                           },
                           11948502},
        PhaseErrorTestCase{"InvalidTransitionalShape",
                           []() {
                               // kLatest is not a valid downgrade target.
                               return makeArbitraryFCVDoc(GenericFCV::kLatest,
                                                          GenericFCV::kLatest,
                                                          GenericFCV::kLatest,
                                                          SetFCVPhaseEnum::kEnableTargetFeatures);
                           },
                           11948503}),
    [](const ::testing::TestParamInfo<PhaseErrorTestCase>& info) { return info.param.name; });

// ---------- Read-side guard for phase + Symmetric FCV consistency ----------

// Note: not using the FCVParserPhaseProjectionTest fixture — this test asserts behavior with
// Symmetric FCV *disabled*, which is the default state.
TEST(FCVParserSymmetricFCVGuardTest, PhasePresentWithSymmetricDisabledIsRejected) {
    unittest::ServerParameterGuard disableSymmetricFCV{"featureFlagSymmetricFCV", false};
    FeatureCompatibilityVersionDocument doc;
    doc.setVersion(GenericFCV::kLastLTS);
    doc.setTargetVersion(GenericFCV::kLatest);
    doc.setPhase(SetFCVPhaseEnum::kEnableTargetFeatures);
    auto result = FeatureCompatibilityVersionParser::parse(doc.toBSON());
    ASSERT_EQ(result.getStatus().code(), 11948500);
}

}  // namespace
}  // namespace mongo
