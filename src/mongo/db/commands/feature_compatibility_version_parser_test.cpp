// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

static FeatureCompatibilityVersionDocument makeFCVDoc(FCV version,
                                                      boost::optional<FCV> targetVersion,
                                                      boost::optional<FCV> previousVersion,
                                                      boost::optional<SetFCVPhaseEnum> phase) {
    FeatureCompatibilityVersionDocument doc;
    doc.setVersion(version);
    doc.setTargetVersion(targetVersion);
    doc.setPreviousVersion(previousVersion);
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
                          return makeFCVDoc(GenericFCV::kLatest,
                                            GenericFCV::kLatest,
                                            GenericFCV::kLastLTS,
                                            SetFCVPhaseEnum::kEnableTargetFeatures);
                      },
                      GenericFCV::kLatest},
        PhaseTestCase{"UpgradeLastLTSToLatestCommitAddedFeatures",
                      []() {
                          return makeFCVDoc(GenericFCV::kLatest,
                                            GenericFCV::kLatest,
                                            GenericFCV::kLastLTS,
                                            SetFCVPhaseEnum::kCommitAddedFeatures);
                      },
                      GenericFCV::kLatest},
        PhaseTestCase{"UpgradeLastContinuousToLatestEnableTargetFeatures",
                      []() {
                          return makeFCVDoc(GenericFCV::kLatest,
                                            GenericFCV::kLatest,
                                            GenericFCV::kLastContinuous,
                                            SetFCVPhaseEnum::kEnableTargetFeatures);
                      },
                      GenericFCV::kLatest},
        PhaseTestCase{"UpgradeLastContinuousToLatestCommitAddedFeatures",
                      []() {
                          return makeFCVDoc(GenericFCV::kLatest,
                                            GenericFCV::kLatest,
                                            GenericFCV::kLastContinuous,
                                            SetFCVPhaseEnum::kCommitAddedFeatures);
                      },
                      GenericFCV::kLatest},
        PhaseTestCase{"UpgradeLastLTSToLastContinuousEnableTargetFeatures",
                      []() {
                          return makeFCVDoc(GenericFCV::kLastContinuous,
                                            GenericFCV::kLastContinuous,
                                            GenericFCV::kLastLTS,
                                            SetFCVPhaseEnum::kEnableTargetFeatures);
                      },
                      GenericFCV::kLastContinuous},
        PhaseTestCase{"UpgradeLastLTSToLastContinuousCommitAddedFeatures",
                      []() {
                          return makeFCVDoc(GenericFCV::kLastContinuous,
                                            GenericFCV::kLastContinuous,
                                            GenericFCV::kLastLTS,
                                            SetFCVPhaseEnum::kCommitAddedFeatures);
                      },
                      GenericFCV::kLastContinuous},
        PhaseTestCase{"DowngradeLastLTSEnableTargetFeatures",
                      []() {
                          return makeFCVDoc(GenericFCV::kLastLTS,
                                            GenericFCV::kLastLTS,
                                            GenericFCV::kLatest,
                                            SetFCVPhaseEnum::kEnableTargetFeatures);
                      },
                      GenericFCV::kLastLTS},
        PhaseTestCase{"DowngradeLastLTSCommitAddedFeatures",
                      []() {
                          return makeFCVDoc(GenericFCV::kLastLTS,
                                            GenericFCV::kLastLTS,
                                            GenericFCV::kLatest,
                                            SetFCVPhaseEnum::kCommitAddedFeatures);
                      },
                      GenericFCV::kLastLTS},
        PhaseTestCase{"DowngradeLastContinuousEnableTargetFeatures",
                      []() {
                          return makeFCVDoc(GenericFCV::kLastContinuous,
                                            GenericFCV::kLastContinuous,
                                            GenericFCV::kLatest,
                                            SetFCVPhaseEnum::kEnableTargetFeatures);
                      },
                      GenericFCV::kLastContinuous},
        PhaseTestCase{"DowngradeLastContinuousCommitAddedFeatures",
                      []() {
                          return makeFCVDoc(GenericFCV::kLastContinuous,
                                            GenericFCV::kLastContinuous,
                                            GenericFCV::kLatest,
                                            SetFCVPhaseEnum::kCommitAddedFeatures);
                      },
                      GenericFCV::kLastContinuous},
        // Phases that project to the transitional FCV.
        PhaseTestCase{"UpgradeStart",
                      []() {
                          return makeFCVDoc(GenericFCV::kLastLTS,
                                            GenericFCV::kLatest,
                                            boost::none,
                                            SetFCVPhaseEnum::kStart);
                      },
                      GenericFCV::kUpgradingFromLastLTSToLatest},
        PhaseTestCase{"UpgradePrepare",
                      []() {
                          return makeFCVDoc(GenericFCV::kLastLTS,
                                            GenericFCV::kLatest,
                                            boost::none,
                                            SetFCVPhaseEnum::kPrepare);
                      },
                      GenericFCV::kUpgradingFromLastLTSToLatest},
        PhaseTestCase{"UpgradeComplete",
                      []() {
                          return makeFCVDoc(GenericFCV::kLastLTS,
                                            GenericFCV::kLatest,
                                            boost::none,
                                            SetFCVPhaseEnum::kComplete);
                      },
                      GenericFCV::kUpgradingFromLastLTSToLatest},
        PhaseTestCase{"DowngradeStart",
                      []() {
                          return makeFCVDoc(GenericFCV::kLastLTS,
                                            GenericFCV::kLastLTS,
                                            GenericFCV::kLatest,
                                            SetFCVPhaseEnum::kStart);
                      },
                      GenericFCV::kDowngradingFromLatestToLastLTS},
        PhaseTestCase{"DowngradeComplete",
                      []() {
                          return makeFCVDoc(GenericFCV::kLastLTS,
                                            GenericFCV::kLastLTS,
                                            GenericFCV::kLatest,
                                            SetFCVPhaseEnum::kComplete);
                      },
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
                               return makeFCVDoc(GenericFCV::kLastLTS,
                                                 boost::none,
                                                 GenericFCV::kLastLTS,
                                                 SetFCVPhaseEnum::kEnableTargetFeatures);
                           },
                           11948501},
        PhaseErrorTestCase{"MissingPreviousVersion",
                           []() {
                               return makeFCVDoc(GenericFCV::kLastLTS,
                                                 GenericFCV::kLatest,
                                                 boost::none,
                                                 SetFCVPhaseEnum::kEnableTargetFeatures);
                           },
                           11948502},
        PhaseErrorTestCase{"InvalidTransitionalShape",
                           []() {
                               // kLatest is not a valid downgrade target.
                               return makeFCVDoc(GenericFCV::kLatest,
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
