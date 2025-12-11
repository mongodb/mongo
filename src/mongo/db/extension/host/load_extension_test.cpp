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

#include "mongo/db/extension/host/load_extension.h"

#include "mongo/db/extension/host/document_source_extension.h"
#include "mongo/db/extension/host/load_extension_test_util.h"
#include "mongo/db/extension/host/load_stub_parsers.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <filesystem>

namespace mongo::extension::host {

class LoadExtensionsTest : public unittest::Test {
protected:
    LoadExtensionsTest() : expCtx(make_intrusive<ExpressionContextForTest>()) {}

    static inline const std::string kTestFooStageName = "$testFoo";
    static inline const std::string kTestFooLibExtensionPath = "libfoo_mongo_extension.so";

    /**
     * Tests loading of desugar extensions.
     */
    static inline const std::string kMatchTopNStageName = "$matchTopN";
    static inline const std::string kMatchTopNLibExtensionPath = "libmatch_topN_mongo_extension.so";

    void tearDown() override {
        host::DocumentSourceExtension::unregisterParser_forTest(kTestFooStageName);
        ExtensionLoader::unload_forTest("foo");
        host::DocumentSourceExtension::unregisterParser_forTest(kMatchTopNStageName);
        ExtensionLoader::unload_forTest("matchTopN");
    }

    ExtensionConfig makeTestFooConfig() {
        return test_util::makeEmptyExtensionConfig(kTestFooLibExtensionPath);
    }

    ExtensionConfig makeMatchTopNConfig() {
        return test_util::makeEmptyExtensionConfig(kMatchTopNLibExtensionPath);
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;

    static inline NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "load_extension_test");

private:
    RAIIServerParameterControllerForTest _featureFlag{"featureFlagExtensionsAPI", true};
};

TEST_F(LoadExtensionsTest, LoadExtensionErrorCases) {
    // Ensure that the RUNFILES_DIR environment variable is set, which is required for this test.
    // This variable is typically set by the Bazel build system, so this test must be run using
    // Bazel.
    ASSERT(!test_util::runFilesDir.empty());

    // Test that various non-existent extension cases fail with the proper error code.
    ASSERT_THROWS_CODE(ExtensionLoader::load("src", test_util::makeEmptyExtensionConfig("src/")),
                       AssertionException,
                       10615500);
    ASSERT_THROWS_CODE(ExtensionLoader::load("notanextension",
                                             test_util::makeEmptyExtensionConfig("notanextension")),
                       AssertionException,
                       10615500);
    ASSERT_THROWS_CODE(
        ExtensionLoader::load(
            "extension", test_util::makeEmptyExtensionConfig("path/to/nonexistent/extension.so")),
        AssertionException,
        10615500);

    ASSERT_THROWS_CODE(
        ExtensionLoader::load("notanextension",
                              test_util::makeEmptyExtensionConfig("libnotanextension.so")),
        AssertionException,
        10615500);

    // no_symbol_bad_extension is missing the get_mongodb_extension symbol definition.
    ASSERT_THROWS_CODE(
        ExtensionLoader::load("no_symbol_bad_extension",
                              test_util::makeEmptyExtensionConfig("libno_symbol_bad_extension.so")),
        AssertionException,
        10615501);

    // null_mongo_extension_bad_extension returns null from get_mongodb_extension.
    ASSERT_THROWS_CODE(ExtensionLoader::load("null_mongo_extension_bad_extension",
                                             test_util::makeEmptyExtensionConfig(
                                                 "libnull_mongo_extension_bad_extension.so")),
                       AssertionException,
                       10615503);

    // major_version_too_high_bad_extension has an incompatible major version (plus 1).
    ASSERT_THROWS_CODE(ExtensionLoader::load("major_version_too_high_bad_extension",
                                             test_util::makeEmptyExtensionConfig(
                                                 "libmajor_version_too_high_bad_extension.so")),
                       AssertionException,
                       10615504);

    // major_version_too_low_bad_extension has an incompatible major version (minus 1).
    ASSERT_THROWS_CODE(ExtensionLoader::load("major_version_too_low_bad_extension",
                                             test_util::makeEmptyExtensionConfig(
                                                 "libmajor_version_too_low_bad_extension.so")),
                       AssertionException,
                       10615504);

    // minor_version_too_high_bad_extension has an incompatible minor version.
    ASSERT_THROWS_CODE(ExtensionLoader::load("minor_version_too_high_bad_extension",
                                             test_util::makeEmptyExtensionConfig(
                                                 "libminor_version_too_high_bad_extension.so")),
                       AssertionException,
                       10615505);

    // major_version_max_int_bad_extension has the maximum uint32_t value as its major version.
    ASSERT_THROWS_CODE(ExtensionLoader::load("major_version_max_int_bad_extension",
                                             test_util::makeEmptyExtensionConfig(
                                                 "libmajor_version_max_int_bad_extension.so")),
                       AssertionException,
                       10615504);

    // duplicate_stage_descriptor_bad_extension attempts to register the same stage descriptor
    // multiple times.
    ASSERT_THROWS_CODE(ExtensionLoader::load("duplicate_stage_descriptor_bad_extension",
                                             test_util::makeEmptyExtensionConfig(
                                                 "libduplicate_stage_descriptor_bad_extension.so")),
                       AssertionException,
                       10696402);

    ASSERT_THROWS_CODE(ExtensionLoader::load("duplicate_version_bad_extension",
                                             test_util::makeEmptyExtensionConfig(
                                                 "libduplicate_version_bad_extension.so")),
                       AssertionException,
                       10930201);

    ASSERT_THROWS_CODE(ExtensionLoader::load("no_compatible_version_bad_extension",
                                             test_util::makeEmptyExtensionConfig(
                                                 "libno_compatible_version_bad_extension.so")),
                       AssertionException,
                       10930202);
}

// null_initialize_function_bad_extension has a null initialization function.
using LoadExtensionsTestDeathTest = LoadExtensionsTest;
DEATH_TEST_F(LoadExtensionsTestDeathTest, LoadExtensionNullInitialize, "10930101") {
    ExtensionLoader::load(
        "null_initialize_function_bad_extension",
        test_util::makeEmptyExtensionConfig("libnull_initialize_function_bad_extension.so"));
}

TEST_F(LoadExtensionsTest, RepetitivelyLoadingTheSameExtensionFails) {
    // We should be able to load the extension once.
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load("foo", makeTestFooConfig()));

    // We should not be able to load the extension twice.
    ASSERT_THROWS_CODE(
        ExtensionLoader::load("foo", makeTestFooConfig()), AssertionException, 10845400);
}

// Tests successful extension loading and verifies stage registration works in pipelines.
// The libfoo_mongo_extension.so adds a "$testFoo" stage for testing.
TEST_F(LoadExtensionsTest, LoadExtensionSucceeds) {
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load("foo", makeTestFooConfig()));

    // Verify the initialization function registered a stage.
    BSONObj stageSpec = BSON(kTestFooStageName << BSONObj());

    auto sourceList = DocumentSource::parse(expCtx, stageSpec);
    ASSERT_EQUALS(sourceList.size(), 1U);

    auto extensionStage = dynamic_cast<DocumentSourceExtension*>(sourceList.front().get());
    ASSERT_TRUE(extensionStage != nullptr);
    ASSERT_EQUALS(std::string(extensionStage->getSourceName()), kTestFooStageName);

    // Verify the stage can be used in a pipeline with other existing stages.
    std::vector<BSONObj> pipeline = {BSON(kTestFooStageName << BSONObj()), BSON("$limit" << 1)};

    auto parsedPipeline = Pipeline::parse(pipeline, expCtx);
    ASSERT_TRUE(parsedPipeline != nullptr);
    ASSERT_EQUALS(parsedPipeline->getSources().size(), 2U);

    auto firstStage =
        dynamic_cast<DocumentSourceExtension*>(parsedPipeline->getSources().front().get());
    ASSERT_TRUE(firstStage != nullptr);
    ASSERT_EQUALS(std::string(firstStage->getSourceName()), std::string(kTestFooStageName));
}

// Tests successful desugar extension loading and verifies stage registration works in pipelines.
// The libmatch_topN_mongo_extension.so adds a "$matchTopN" stage for testing.
TEST_F(LoadExtensionsTest, LoadMatchTopNDesugarExtensionSucceeds) {
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load("matchTopN", makeMatchTopNConfig()));

    // Verify the initialization function registered a stage.
    BSONObj stageSpec =
        BSON(kMatchTopNStageName << BSON("filter" << BSON("x" << 1) << "sort" << BSON("x" << 1)
                                                  << "limit" << 5));

    // LiteParsed expansion
    {
        auto liteParsed = LiteParsedDocumentSource::parse(nss, stageSpec);
        auto liteParsedExpandable =
            dynamic_cast<DocumentSourceExtension::LiteParsedExpandable*>(liteParsed.get());
        ASSERT_TRUE(liteParsedExpandable != nullptr);
        const auto& expanded = liteParsedExpandable->getExpandedPipeline();
        ASSERT_EQ(expanded.size(), 3U);

        const auto it0 = expanded.begin();
        const auto it1 = std::next(expanded.begin(), 1);
        const auto it2 = std::next(expanded.begin(), 2);

        auto* first = dynamic_cast<LiteParsedDocumentSource*>(it0->get());
        ASSERT(first);
        ASSERT_EQ(first->getParseTimeName(), DocumentSourceMatch::kStageName);

        auto* second = dynamic_cast<LiteParsedDocumentSource*>(it1->get());
        ASSERT(second);
        ASSERT_EQ(second->getParseTimeName(), DocumentSourceSort::kStageName);

        auto* third = dynamic_cast<LiteParsedDocumentSource*>(it2->get());
        ASSERT(third);
        ASSERT_EQ(third->getParseTimeName(), DocumentSourceLimit::kStageName);
    }

    auto sourceList = DocumentSource::parse(expCtx, stageSpec);
    ASSERT_EQUALS(sourceList.size(), 1U);

    auto extensionStage = dynamic_cast<DocumentSourceExtension*>(sourceList.front().get());
    ASSERT(extensionStage);
    ASSERT_EQUALS(std::string(extensionStage->getSourceName()), kMatchTopNStageName);

    // Verify the stage can be used in a pipeline with other existing stages.
    std::vector<BSONObj> pipeline = {stageSpec, BSON("$skip" << 1)};

    auto parsedPipeline = Pipeline::parse(pipeline, expCtx);
    ASSERT(parsedPipeline);
    ASSERT_EQUALS(parsedPipeline->getSources().size(), 2U);

    auto firstStage =
        dynamic_cast<DocumentSourceExtension*>(parsedPipeline->getSources().front().get());
    ASSERT(firstStage);
    ASSERT_EQUALS(std::string(firstStage->getSourceName()), kMatchTopNStageName);

    // Full Parse expansion
    {
        Desugarer(parsedPipeline.get())();
        ASSERT_EQ(parsedPipeline->size(), 4U);

        auto it = parsedPipeline->getSources().begin();
        ASSERT(dynamic_cast<DocumentSourceMatch*>(it->get()));

        ++it;
        ASSERT(dynamic_cast<DocumentSourceSort*>(it->get()));

        ++it;
        ASSERT(dynamic_cast<DocumentSourceLimit*>(it->get()));

        ++it;
        ASSERT(dynamic_cast<DocumentSourceSkip*>(it->get()));
    }
}

// Tests that extension initialization properly populates the parser map before and after loading.
TEST_F(LoadExtensionsTest, InitializationFunctionPopulatesParserMap) {
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load("foo", makeTestFooConfig()));

    BSONObj stageSpec = BSON(kTestFooStageName << BSONObj());

    auto sourceList = DocumentSource::parse(expCtx, stageSpec);
    ASSERT_EQUALS(sourceList.size(), 1U);

    auto extensionStage = dynamic_cast<DocumentSourceExtension*>(sourceList.front().get());
    ASSERT_TRUE(extensionStage != nullptr);
    ASSERT_EQUALS(std::string(extensionStage->getSourceName()), std::string(kTestFooStageName));
}

TEST_F(LoadExtensionsTest, LoadExtensionHostVersionParameterSucceeds) {
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load(
        "hostVersionSucceeds",
        test_util::makeEmptyExtensionConfig("libhost_version_succeeds_mongo_extension.so")));
}

TEST_F(LoadExtensionsTest, LoadExtensionHostVersionParameterFails) {
    ASSERT_THROWS_CODE(ExtensionLoader::load("host_version_fails_bad_extension",
                                             test_util::makeEmptyExtensionConfig(
                                                 "libhost_version_fails_bad_extension.so")),
                       AssertionException,
                       10615503);
}

TEST_F(LoadExtensionsTest, LoadExtensionInitializeVersionFails) {
    ASSERT_THROWS_CODE(ExtensionLoader::load("initialize_version_fails_bad_extension",
                                             test_util::makeEmptyExtensionConfig(
                                                 "libinitialize_version_fails_bad_extension.so")),
                       AssertionException,
                       10726600);
}

DEATH_TEST_F(LoadExtensionsTestDeathTest, LoadExtensionNullStageDescriptor, "10596400") {
    ExtensionLoader::load(
        "null_stage_descriptor_bad_extension",
        test_util::makeEmptyExtensionConfig("libnull_stage_descriptor_bad_extension.so"));
}

TEST_F(LoadExtensionsTest, LoadExtensionTwoStagesSucceeds) {
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load(
        "loadTwoStages",
        test_util::makeEmptyExtensionConfig("libload_two_stages_mongo_extension.so")));

    std::vector<BSONObj> pipeline = {BSON("$foo" << BSONObj()), BSON("$bar" << BSONObj())};
    auto parsedPipeline = Pipeline::parse(pipeline, expCtx);
    ASSERT_TRUE(parsedPipeline != nullptr);
    ASSERT_EQUALS(parsedPipeline->getSources().size(), 2U);

    auto fooStage =
        dynamic_cast<DocumentSourceExtension*>(parsedPipeline->getSources().front().get());
    auto barStage =
        dynamic_cast<DocumentSourceExtension*>(parsedPipeline->getSources().back().get());

    ASSERT_TRUE(fooStage != nullptr && barStage != nullptr);
    ASSERT_EQUALS(std::string(fooStage->getSourceName()), "$foo");
    ASSERT_EQUALS(std::string(barStage->getSourceName()), "$bar");
}

TEST_F(LoadExtensionsTest, LoadHighestCompatibleVersionSucceeds) {
    ASSERT_DOES_NOT_THROW(
        ExtensionLoader::load("loadHighestCompatibleVersion",
                              test_util::makeEmptyExtensionConfig(
                                  "libload_highest_compatible_version_mongo_extension.so")));

    std::vector<BSONObj> pipeline = {BSON("$extensionV1" << BSONObj())};
    auto parsedPipeline = Pipeline::parse(pipeline, expCtx);
    ASSERT_TRUE(parsedPipeline != nullptr);
    ASSERT_EQUALS(parsedPipeline->getSources().size(), 1U);

    auto extensionStage =
        dynamic_cast<DocumentSourceExtension*>(parsedPipeline->getSources().front().get());

    ASSERT_TRUE(extensionStage != nullptr);
    ASSERT_EQUALS(std::string(extensionStage->getSourceName()), "$extensionV1");

    // Assert that the other extension versions registered aren't available.
    pipeline = {BSON("$extensionV2" << BSONObj())};
    ASSERT_THROWS_CODE(Pipeline::parse(pipeline, expCtx), AssertionException, 40324);
    pipeline = {BSON("$extensionV3" << BSONObj())};
    ASSERT_THROWS_CODE(Pipeline::parse(pipeline, expCtx), AssertionException, 40324);
}

TEST_F(LoadExtensionsTest, LoadExtensionBothOptionsSucceed) {
    const auto extOptions = YAML::Load("optionA: true\n");
    const ExtensionConfig config = {
        .sharedLibraryPath =
            test_util::getExtensionPath("libtest_options_mongo_extension.so").string(),
        .extOptions = extOptions};
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load("test_options", config));

    std::vector<BSONObj> pipeline = {BSON("$optionA" << BSONObj())};
    auto parsedPipeline = Pipeline::parse(pipeline, expCtx);
    ASSERT_TRUE(parsedPipeline != nullptr);
    ASSERT_EQUALS(parsedPipeline->getSources().size(), 1U);

    auto stage = dynamic_cast<DocumentSourceExtension*>(parsedPipeline->getSources().front().get());
    ASSERT_TRUE(stage != nullptr);
    ASSERT_EQUALS(std::string(stage->getSourceName()), "$optionA");

    // Assert that $optionB is unavailable.
    pipeline = {BSON("$optionB" << BSONObj())};
    ASSERT_THROWS_CODE(Pipeline::parse(pipeline, expCtx), AssertionException, 40324);
}

TEST_F(LoadExtensionsTest, LoadExtensionParseWithExtensionOptions) {
    const auto extOptions = YAML::Load("checkMax: true\nmax: 10");
    const ExtensionConfig config = {
        .sharedLibraryPath =
            test_util::getExtensionPath("libparse_options_mongo_extension.so").string(),
        .extOptions = extOptions};
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load("parse_options", config));

    std::vector<BSONObj> pipeline = {BSON("$checkNum" << BSON("num" << 9))};
    auto parsedPipeline = Pipeline::parse(pipeline, expCtx);
    ASSERT_TRUE(parsedPipeline != nullptr);
    ASSERT_EQUALS(parsedPipeline->getSources().size(), 1U);

    auto stage = dynamic_cast<DocumentSourceExtension*>(parsedPipeline->getSources().front().get());
    ASSERT_TRUE(stage != nullptr);
    ASSERT_EQUALS(std::string(stage->getSourceName()), "$checkNum");

    // Assert that parsing fails when the provided num is greater than the specified max of 10.
    pipeline = {BSON("$checkNum" << BSON("num" << 11))};
    ASSERT_THROWS_CODE(Pipeline::parse(pipeline, expCtx), AssertionException, 10999106);
}

TEST_F(LoadExtensionsTest, LoadExtensionConfigErrors) {
    ASSERT_THROWS_CODE(ExtensionLoader::loadExtensionConfig(""), AssertionException, 11031700);
    ASSERT_THROWS_CODE(
        ExtensionLoader::loadExtensionConfig("path/with/separators"), AssertionException, 11031700);
    ASSERT_THROWS_CODE(ExtensionLoader::loadExtensionConfig("path\\with\\separators"),
                       AssertionException,
                       11031700);
    ASSERT_THROWS_CODE(
        ExtensionLoader::loadExtensionConfig("/beginning"), AssertionException, 11031700);
    ASSERT_THROWS_CODE(ExtensionLoader::loadExtensionConfig("end/"), AssertionException, 11031700);
    ASSERT_THROWS_CODE(
        ExtensionLoader::loadExtensionConfig("\\beginning"), AssertionException, 11031700);
    ASSERT_THROWS_CODE(ExtensionLoader::loadExtensionConfig("end\\"), AssertionException, 11031700);
}

TEST_F(LoadExtensionsTest, LoadStubParser) {
    // Register a parse for the "$stub" stage, a model of what could be added by an extension.
    const auto errorMsg =
        "The extension stage '$stub' is not available because the corresponding extension is not "
        "loaded.";
    registerStubParser("$stub", errorMsg);

    // Verify that attempting to parse a pipeline with the $stub stage fails with the proper error
    // message.
    std::vector<BSONObj> pipeline = {BSON("$stub" << BSONObj()), BSON("$limit" << 1)};
    ASSERT_THROWS_CODE(Pipeline::parse(pipeline, expCtx), AssertionException, 10918500);
    ASSERT_THROWS_WHAT(Pipeline::parse(pipeline, expCtx), AssertionException, errorMsg);
}

DEATH_TEST_F(LoadExtensionsTestDeathTest,
             LoadStubParserFailsIfPrimaryAlreadyRegistered,
             "11395100") {
    // Attempting to register a fallback stub parser for $match should fail because fallback parsers
    // must be registered before primary parsers.
    registerStubParser("$match", "This should fail since $match is already registered.");
}

/*
 * Test fixture that loads the "extension_errors" test extension so that tests can be run against
 * it.
 *
 * Note that this testing really would be better suited to an integration test, but we can't test
 * tasserts in a jstest without failing the suite, so we cover that here in a death test.
 */
class ExtensionErrorsTest : public LoadExtensionsTest {
public:
    void setUp() override {
        if (ExtensionLoader::isLoaded("extension_errors")) {
            return;
        }
        const auto config =
            test_util::makeEmptyExtensionConfig(kTestExtensionErrorsLibExtensionPath);
        ExtensionLoader::load("extension_errors", config);
    }

protected:
    static inline const std::string kTestExtensionErrorsLibExtensionPath =
        "libextension_errors_mongo_extension.so";
};

TEST_F(ExtensionErrorsTest, ExtensionUasserts) {
    std::vector<BSONObj> pipeline = {
        BSON("$assert" << BSON("errmsg" << "a new error" << "code" << 54321 << "assertionType"
                                        << "uassert"))};
    ASSERT_THROWS_CODE(Pipeline::parse(pipeline, expCtx), AssertionException, 54321);
    ASSERT_THROWS_WHAT(Pipeline::parse(pipeline, expCtx), AssertionException, "a new error");
}

using ExtensionErrorsTestDeathTest = ExtensionErrorsTest;
DEATH_TEST_REGEX_F(ExtensionErrorsTestDeathTest, ExtensionTasserts, "98765.*another new error") {
    std::vector<BSONObj> pipeline = {
        BSON("$assert" << BSON("errmsg" << "another new error" << "code" << 98765 << "assertionType"
                                        << "tassert"))};
    [[maybe_unused]] auto parsedPipeline = Pipeline::parse(pipeline, expCtx);
}
}  // namespace mongo::extension::host
