/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/lite_parsed_document_source.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("test.liteParsedDocSource"_sd);

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(Mock);
ALLOCATE_STAGE_PARAMS_ID(mock, MockStageParams::id);

// Mock parser functions for testing
std::unique_ptr<LiteParsedDocumentSource> createMockParser(const NamespaceString& nss,
                                                           const BSONElement& spec,
                                                           const LiteParserOptions& options) {
    return std::make_unique<MockLiteParsed>(spec);
}

}  // namespace

class LiteParserRegistrationTest : public unittest::Test {
protected:
    void setUp() override {
        primaryParser = {
            createMockParser, AllowedWithApiStrict::kAlways, AllowedWithClientType::kAny};
        fallbackParser = {createMockParser,
                          AllowedWithApiStrict::kNeverInVersion1,
                          AllowedWithClientType::kInternal};
    }

    void assertParserIsPrimary(const LiteParsedDocumentSource::LiteParserInfo& parserInfo) {
        ASSERT_EQ(parserInfo.allowedWithApiStrict, AllowedWithApiStrict::kAlways);
        ASSERT_EQ(parserInfo.allowedWithClientType, AllowedWithClientType::kAny);
    }

    void assertParserIsFallback(const LiteParsedDocumentSource::LiteParserInfo& parserInfo) {
        ASSERT_EQ(parserInfo.allowedWithApiStrict, AllowedWithApiStrict::kNeverInVersion1);
        ASSERT_EQ(parserInfo.allowedWithClientType, AllowedWithClientType::kInternal);
    }

    LiteParsedDocumentSource::LiteParserInfo primaryParser;
    LiteParsedDocumentSource::LiteParserInfo fallbackParser;
};

TEST_F(LiteParserRegistrationTest, SetPrimaryParser) {
    LiteParsedDocumentSource::LiteParserRegistration registration;
    ASSERT_FALSE(registration.isPrimarySet());
    registration.setPrimaryParser(std::move(primaryParser));
    ASSERT_TRUE(registration.isPrimarySet());
}

TEST_F(LiteParserRegistrationTest, SetFallbackParser) {
    LiteParsedDocumentSource::LiteParserRegistration registration;

    // Create a mock IncrementalRolloutFeatureFlag.
    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, false);
    registration.setFallbackParser(std::move(fallbackParser), &mockFlag);

    // Verify primary is still not set.
    ASSERT_FALSE(registration.isPrimarySet());
    ASSERT_TRUE(registration.isFallbackSet());
}

TEST_F(LiteParserRegistrationTest, GetParserWithoutFeatureFlag) {
    LiteParsedDocumentSource::LiteParserRegistration registration;

    registration.setPrimaryParser(std::move(primaryParser));

    // When there's no feature flag, should return primary parser.
    const auto& parserInfo = registration.getParserInfo();
    assertParserIsPrimary(parserInfo);
}

TEST_F(LiteParserRegistrationTest, GetParserWithFeatureFlagEnabled) {
    LiteParsedDocumentSource::LiteParserRegistration registration;

    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, true);
    registration.setFallbackParser(std::move(fallbackParser), &mockFlag);
    registration.setPrimaryParser(std::move(primaryParser));

    // When feature flag is enabled, should return primary parser.
    const auto& parserInfo = registration.getParserInfo();
    assertParserIsPrimary(parserInfo);
}

TEST_F(LiteParserRegistrationTest, GetParserWithFeatureFlagDisabled) {
    LiteParsedDocumentSource::LiteParserRegistration registration;

    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, false);
    registration.setFallbackParser(std::move(fallbackParser), &mockFlag);
    registration.setPrimaryParser(std::move(primaryParser));

    // When feature flag is disabled, should return fallback parser.
    const auto& parserInfo = registration.getParserInfo();
    assertParserIsFallback(parserInfo);
}

TEST_F(LiteParserRegistrationTest, GetParserWithChangingFeatureFlag) {
    LiteParsedDocumentSource::LiteParserRegistration registration;

    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, false);
    registration.setFallbackParser(std::move(fallbackParser), &mockFlag);
    registration.setPrimaryParser(std::move(primaryParser));

    // When feature flag is disabled, should return fallback parser.
    assertParserIsFallback(registration.getParserInfo());

    // Enable the feature flag and check that the primary parser is chosen.
    mockFlag.setForServerParameter(true);
    assertParserIsPrimary(registration.getParserInfo());

    // Disable it and check that the fallback parser is chosen.
    mockFlag.setForServerParameter(false);
    assertParserIsFallback(registration.getParserInfo());
}

class LiteParsedDocumentSourceParseTest : public unittest::Test {
protected:
    void registerPrimaryParser() {
        LiteParsedDocumentSource::registerParser(_stageName,
                                                 createMockParser,
                                                 AllowedWithApiStrict::kAlways,
                                                 AllowedWithClientType::kAny);
    }

    void registerFallbackParser(FeatureFlag* ff) {
        LiteParsedDocumentSource::registerFallbackParser(_stageName,
                                                         createMockParser,
                                                         ff,
                                                         AllowedWithApiStrict::kNeverInVersion1,
                                                         AllowedWithClientType::kInternal);
    }

    void tearDown() override {
        LiteParsedDocumentSource::unregisterParser_forTest(_stageName);
    }

    const LiteParsedDocumentSource::LiteParserInfo& getParserInfo() {
        return LiteParsedDocumentSource::getParserInfo_forTest(_stageName);
    }

    // Note that _stageName should be unique between every test and set at the beginning. This is
    // because we have no way of clearing out the global variable aggStageCounters.
    std::string _stageName;
};

TEST_F(LiteParsedDocumentSourceParseTest, CanRegisterBothPrimaryAndFallback) {
    _stageName = "$canRegisterBothPrimaryAndFallback";

    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, false);
    registerFallbackParser(&mockFlag);
    registerPrimaryParser();
}

DEATH_TEST_F(LiteParsedDocumentSourceParseTest, MustRegisterPrimaryAfterFallback, "11395100") {
    _stageName = "$mustRegisterPrimaryAfterFallback";

    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, false);
    registerPrimaryParser();
    registerFallbackParser(&mockFlag);
}

TEST_F(LiteParsedDocumentSourceParseTest, FirstFallbackParserTakesPrecedence) {
    _stageName = "$firstFallbackParserTakesPrecedence";

    // Disable the feature flag such that the parser returns the fallback parser.
    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, false);
    registerFallbackParser(&mockFlag);
    registerPrimaryParser();

    // Try creating another fallback parser.
    LiteParsedDocumentSource::registerFallbackParser(_stageName,
                                                     createMockParser,
                                                     &mockFlag,
                                                     AllowedWithApiStrict::kNeverInVersion1,
                                                     AllowedWithClientType::kAny);

    // Ensure that the parser info is the original fallback parser.
    auto parserInfo = getParserInfo();
    ASSERT_EQ(parserInfo.allowedWithApiStrict, AllowedWithApiStrict::kNeverInVersion1);
    ASSERT_EQ(parserInfo.allowedWithClientType, AllowedWithClientType::kInternal);
}

TEST_F(LiteParsedDocumentSourceParseTest, FirstFallbackParserTakesPrecedenceWithNoPrimary) {
    _stageName = "$firstFallbackParserTakesPrecedenceWithNoPrimary";

    // Disable the feature flag such that the parser returns the fallback parser.
    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, false);
    registerFallbackParser(&mockFlag);

    // Try creating another fallback parser.
    LiteParsedDocumentSource::registerFallbackParser(_stageName,
                                                     createMockParser,
                                                     &mockFlag,
                                                     AllowedWithApiStrict::kNeverInVersion1,
                                                     AllowedWithClientType::kAny);

    // Ensure that the parser info is the original fallback parser.
    auto parserInfo = getParserInfo();
    ASSERT_EQ(parserInfo.allowedWithApiStrict, AllowedWithApiStrict::kNeverInVersion1);
    ASSERT_EQ(parserInfo.allowedWithClientType, AllowedWithClientType::kInternal);
}

// TODO SERVER-114028 Remove the following test when fallback parsing supports all feature flags.
DEATH_TEST_F(LiteParsedDocumentSourceParseTest, IFRFlagIsRequired, "11395101") {
    _stageName = "$IFRFlagIsRequired";

    BinaryCompatibleFeatureFlag mockFlag(false);
    registerFallbackParser(&mockFlag);
}

/**
 * A dummy test stage parameters class used for testing. It just allocates an ID.
 */
DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(Test);
ALLOCATE_STAGE_PARAMS_ID(test, TestStageParams::id);

/**
 * A dummy LiteParsedDocumentSource that implements just enough functionality to return custom
 * derived StageParams.
 */
class TestLiteParsed final : public LiteParsedDocumentSource {
public:
    TestLiteParsed(const BSONElement& originalBson) : LiteParsedDocumentSource(originalBson) {}

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return stdx::unordered_set<NamespaceString>();
    }

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return {};
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<TestStageParams>(_originalBson);
    }
};

/**
 * Verifies that LiteParsedDocumentSource can return StageParams via getStageParams().
 *
 * This test ensures that:
 *   1. getStageParams() returns a valid StageParams object
 *   2. The returned object is of the correct derived type (TestStageParams in this case)
 *   3. The StageParams has a properly allocated, non-zero ID
 *
 * This validates the integration between LiteParsedDocumentSource and the StageParams
 * type system.
 */
TEST(StageParams, CanGetStageParamsFromLiteParsed) {
    BSONObj spec = BSON("$testStage" << BSONObj());
    auto lp = TestLiteParsed(spec.firstElement());
    auto baseParams = lp.getStageParams();
    ASSERT_TRUE(dynamic_cast<TestStageParams*>(baseParams.get()) != nullptr);
    ASSERT_NE(baseParams->getId(), 0);
}

}  // namespace mongo

