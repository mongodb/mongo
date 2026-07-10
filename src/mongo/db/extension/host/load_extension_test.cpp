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

#include "mongo/db/extension/host/document_source_extension_for_query_shape.h"
#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host/load_extension_test_util.h"
#include "mongo/db/extension/host/load_stub_parsers.h"
#include "mongo/db/extension/host/signature_validator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/server_options.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

#include <filesystem>

// Disable LSan's at-exit leak check for this binary. The statically-linked, uninstrumented
// extension .so files cause the leak check to exceed the CI timeout (BF-41799). ASAN and UBSan
// coverage is otherwise unaffected.
extern "C" int __lsan_is_turned_off() {
    return 1;
}

namespace mongo::extension::host {

class LoadExtensionsTest : public unittest::Test {
protected:
    LoadExtensionsTest() : expCtx(make_intrusive<ExpressionContextForTest>()) {}

    static inline const std::string kTestFooStageName = "$testFoo";
    static inline const std::string kDesugarFooStageName = "$desugarFoo";
    static inline const std::string kTestFooLibExtensionPath = "libfoo_mongo_extension.so";

    /**
     * Tests loading of desugar extensions.
     */
    static inline const std::string kMatchTopNStageName = "$matchTopN";
    static inline const std::string kMatchTopNLibExtensionPath = "libmatch_topN_mongo_extension.so";

    void setUp() override {
        _previousExtensionsSignaturePublicKeyPath =
            serverGlobalParams.extensionsSignaturePublicKeyPath;
        serverGlobalParams.extensionsSignaturePublicKeyPath =
            mongo::extension::host::test_util::getPublicKeyPath();
    }
    void tearDown() override {
        if (!_previousExtensionsSignaturePublicKeyPath.empty()) {
            serverGlobalParams.extensionsSignaturePublicKeyPath =
                _previousExtensionsSignaturePublicKeyPath;
        }
        LiteParsedDocumentSource::unregisterParser_forTest(kTestFooStageName);
        LiteParsedDocumentSource::unregisterParser_forTest(kDesugarFooStageName);
        ExtensionLoader::unload_forTest("foo");
        LiteParsedDocumentSource::unregisterParser_forTest(kMatchTopNStageName);
        ExtensionLoader::unload_forTest("matchTopN");
        LiteParsedDocumentSource::unregisterParser_forTest("$optionA");
        ExtensionLoader::unload_forTest("test_options");
        LiteParsedDocumentSource::unregisterParser_forTest("$checkNum");
        ExtensionLoader::unload_forTest("parse_options");
    }

    ExtensionConfig makeTestFooConfig() {
        return test_util::makeEmptyExtensionConfig(kTestFooLibExtensionPath);
    }

    ExtensionConfig makeMatchTopNConfig() {
        return test_util::makeEmptyExtensionConfig(kMatchTopNLibExtensionPath);
    }

    /**
     * Copies a signed test extension and its detached signature into a temp directory we own, so
     * tests can control the on-disk file (permissions, etc.). Normalizes the copy to owner
     * read/write with no group/other write, so it passes the loader's permission gate by default;
     * individual tests loosen this to exercise rejection. Returns the path to the copied .so.
     */
    std::filesystem::path copySignedExtensionToTempDir(const std::string& libName) {
        namespace fs = std::filesystem;
        // Use a fresh subdirectory per call so repeated copies don't have to overwrite read-only
        // files left behind by a previous call (copied .so/.sig inherit the source's read-only
        // mode, which would make a subsequent copy_file fail with EACCES).
        const fs::path destDir = fs::path(_tempDir.path()) / std::to_string(_tempCopyCounter++);
        fs::create_directories(destDir);
        const fs::path dest = destDir / libName;
        const auto src = test_util::getExtensionPath(libName);
        fs::copy_file(src, dest, fs::copy_options::overwrite_existing);
        fs::copy_file(std::string{src} + ".sig",
                      std::string{dest} + ".sig",
                      fs::copy_options::overwrite_existing);
        fs::permissions(
            dest, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
        return dest;
    }

    static ExtensionConfig makeConfigForPath(const std::filesystem::path& path) {
        return ExtensionConfig{.sharedLibraryPath = path.string(),
                               .extOptions = YAML::Node(YAML::NodeType::Map)};
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;

    static inline NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "load_extension_test");

    unittest::TempDir _tempDir{"load_extension_test"};
    size_t _tempCopyCounter{0};

private:
    unittest::ServerParameterGuard _featureFlagExtensionsAPI{"featureFlagExtensionsAPI", true};
    unittest::ServerParameterGuard _featureFlagExtensionsApiSignatureValidation{
        "featureFlagExtensionsApiSignatureValidation", true};
    std::string _previousExtensionsSignaturePublicKeyPath{""};
};

TEST_F(LoadExtensionsTest, LoadExtensionErrorCases) {
    // Ensure that the RUNFILES_DIR environment variable is set, which is required for this test.
    // This variable is typically set by the Bazel build system, so this test must be run using
    // Bazel.
    ASSERT(!test_util::runFilesDir.empty());

    // Test that various non-existent extension cases fail with the proper error code.
    ASSERT_THROWS_CODE(ExtensionLoader::load("src", test_util::makeEmptyExtensionConfig("src/")),
                       AssertionException,
                       11528800);
    ASSERT_THROWS_CODE(ExtensionLoader::load("notanextension",
                                             test_util::makeEmptyExtensionConfig("notanextension")),
                       AssertionException,
                       11528800);
    ASSERT_THROWS_CODE(
        ExtensionLoader::load(
            "extension", test_util::makeEmptyExtensionConfig("path/to/nonexistent/extension.so")),
        AssertionException,
        11528800);

    ASSERT_THROWS_CODE(
        ExtensionLoader::load("notanextension",
                              test_util::makeEmptyExtensionConfig("libnotanextension.so")),
        AssertionException,
        11528800);

    // no_symbol_bad_extension does not define get_mongodb_extension_versions.
    ASSERT_THROWS_CODE(
        ExtensionLoader::load("no_symbol_bad_extension",
                              test_util::makeEmptyExtensionConfig("libno_symbol_bad_extension.so")),
        AssertionException,
        10615501);

    // no_get_extension_symbol_bad_extension defines get_mongodb_extension_versions but does not
    // define get_mongodb_extension.
    ASSERT_THROWS_CODE(ExtensionLoader::load("no_get_extension_symbol_bad_extension",
                                             test_util::makeEmptyExtensionConfig(
                                                 "libno_get_extension_symbol_bad_extension.so")),
                       AssertionException,
                       12688600);

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

    // duplicate_version_bad_extension registers the same version twice, which triggers
    // sdk_uassert during get_mongodb_extension's version negotiation.
    ASSERT_THROWS_CODE(ExtensionLoader::load("duplicate_version_bad_extension",
                                             test_util::makeEmptyExtensionConfig(
                                                 "libduplicate_version_bad_extension.so")),
                       AssertionException,
                       10930201);
}

using LoadExtensionsTestDeathTest = LoadExtensionsTest;

// null_initialize_function_bad_extension has a null initialization function.
DEATH_TEST_F(LoadExtensionsTestDeathTest, LoadExtensionNullInitialize, "517") {
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

    auto extensionStage =
        dynamic_cast<DocumentSourceExtensionForQueryShape*>(sourceList.front().get());
    ASSERT_TRUE(extensionStage != nullptr);
    ASSERT_EQUALS(std::string(extensionStage->getSourceName()), kTestFooStageName);

    // Verify the stage can be used in a pipeline with other existing stages.
    std::vector<BSONObj> pipeline = {BSON(kTestFooStageName << BSONObj()), BSON("$limit" << 1)};

    auto parsedPipeline =
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal);
    ASSERT_TRUE(parsedPipeline != nullptr);
    ASSERT_EQUALS(parsedPipeline->getSources().size(), 2U);

    auto firstStage = dynamic_cast<DocumentSourceExtensionForQueryShape*>(
        parsedPipeline->getSources().front().get());
    ASSERT_TRUE(firstStage != nullptr);
    ASSERT_EQUALS(std::string(firstStage->getSourceName()), std::string(kTestFooStageName));
}

// The loader pins the extension to a file descriptor and verifies/loads it through
// "/proc/self/fd/N", and (when signature validation is enabled) refuses to load a file that an
// untrusted party could mutate underneath us. A read-only, owner-owned copy must still load.
TEST_F(LoadExtensionsTest, LoadExtensionAcceptsOwnerOwnedNonWritableFile) {
    const auto path = copySignedExtensionToTempDir(kTestFooLibExtensionPath);
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load("foo", makeConfigForPath(path)));
    ASSERT_TRUE(ExtensionLoader::isLoaded("foo"));
}

// A group- or other-writable extension file is rejected: such a file could be overwritten in place
// by another user between signature verification and dlopen.
TEST_F(LoadExtensionsTest, LoadExtensionRejectsGroupOrOtherWritableFile) {
    namespace fs = std::filesystem;
    for (const auto writeBit : {fs::perms::group_write, fs::perms::others_write}) {
        const auto path = copySignedExtensionToTempDir(kTestFooLibExtensionPath);
        fs::permissions(path, writeBit, fs::perm_options::add);
        ASSERT_THROWS_CODE(
            ExtensionLoader::load("foo", makeConfigForPath(path)), AssertionException, 10929854);
        ASSERT_FALSE(ExtensionLoader::isLoaded("foo"));
    }
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
            dynamic_cast<DocumentSourceExtensionOptimizable::LiteParsedExpandable*>(
                liteParsed.get());
        ASSERT_TRUE(liteParsedExpandable != nullptr);
        const auto& expanded = liteParsedExpandable->getExpandedPipeline();
        ASSERT_EQ(expanded.size(), 3U);

        auto* first = expanded[0].get();
        ASSERT(first);
        ASSERT_EQ(first->getParseTimeName(), DocumentSourceMatch::kStageName);

        auto* second = expanded[1].get();
        ASSERT(second);
        ASSERT_EQ(second->getParseTimeName(), DocumentSourceSort::kStageName);

        auto* third = expanded[2].get();
        ASSERT(third);
        ASSERT_EQ(third->getParseTimeName(), DocumentSourceLimit::kStageName);
    }

    auto sourceList = DocumentSource::parse(expCtx, stageSpec);
    ASSERT_EQUALS(sourceList.size(), 1U);

    auto extensionStage =
        dynamic_cast<DocumentSourceExtensionForQueryShape*>(sourceList.front().get());
    ASSERT(extensionStage);
    ASSERT_EQUALS(std::string(extensionStage->getSourceName()), kMatchTopNStageName);

    // Verify the stage can be used in a pipeline with other existing stages.
    std::vector<BSONObj> pipeline = {stageSpec, BSON("$skip" << 1)};

    auto parsedPipeline =
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal);
    ASSERT(parsedPipeline);
    ASSERT_EQUALS(parsedPipeline->getSources().size(), 2U);

    auto firstStage = dynamic_cast<DocumentSourceExtensionForQueryShape*>(
        parsedPipeline->getSources().front().get());
    ASSERT(firstStage);
    ASSERT_EQUALS(std::string(firstStage->getSourceName()), kMatchTopNStageName);

    // Full Parse expansion
    {
        auto parsedPipeline =
            pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kDesugarOnly);
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

    auto extensionStage =
        dynamic_cast<DocumentSourceExtensionForQueryShape*>(sourceList.front().get());
    ASSERT_TRUE(extensionStage != nullptr);
    ASSERT_EQUALS(std::string(extensionStage->getSourceName()), std::string(kTestFooStageName));
}

TEST_F(LoadExtensionsTest, LoadExtensionHostVersionParameterSucceeds) {
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load(
        "hostVersionSucceeds",
        test_util::makeEmptyExtensionConfig("libhost_version_succeeds_mongo_extension.so")));
}

TEST_F(LoadExtensionsTest, LoadExtensionHostVersionParameterFails) {
    // Extension publishes two versions: one with an incompatible major, one whose minor exceeds
    // the host's max for the matching major.
    ASSERT_THROWS_CODE(ExtensionLoader::load("host_version_fails_bad_extension",
                                             test_util::makeEmptyExtensionConfig(
                                                 "libhost_version_fails_bad_extension.so")),
                       AssertionException,
                       10615505);
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
    auto parsedPipeline =
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal);
    ASSERT_TRUE(parsedPipeline != nullptr);
    ASSERT_EQUALS(parsedPipeline->getSources().size(), 2U);

    auto fooStage = dynamic_cast<DocumentSourceExtensionForQueryShape*>(
        parsedPipeline->getSources().front().get());
    auto barStage = dynamic_cast<DocumentSourceExtensionForQueryShape*>(
        parsedPipeline->getSources().back().get());

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
    auto parsedPipeline =
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal);
    ASSERT_TRUE(parsedPipeline != nullptr);
    ASSERT_EQUALS(parsedPipeline->getSources().size(), 1U);

    auto extensionStage = dynamic_cast<DocumentSourceExtensionForQueryShape*>(
        parsedPipeline->getSources().front().get());

    ASSERT_TRUE(extensionStage != nullptr);
    ASSERT_EQUALS(std::string(extensionStage->getSourceName()), "$extensionV1");

    // Assert that the other extension versions registered aren't available.
    pipeline = {BSON("$extensionV2" << BSONObj())};
    ASSERT_THROWS_CODE(
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal),
        AssertionException,
        40324);
    pipeline = {BSON("$extensionV3" << BSONObj())};
    ASSERT_THROWS_CODE(
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal),
        AssertionException,
        40324);
}

TEST_F(LoadExtensionsTest, LoadExtensionBothOptionsSucceed) {
    const auto extOptions = YAML::Load("optionA: true\n");
    const ExtensionConfig config = {
        .sharedLibraryPath =
            test_util::getExtensionPath("libtest_options_mongo_extension.so").string(),
        .extOptions = extOptions};
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load("test_options", config));

    std::vector<BSONObj> pipeline = {BSON("$optionA" << BSONObj())};
    auto parsedPipeline =
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal);
    ASSERT_TRUE(parsedPipeline != nullptr);
    ASSERT_EQUALS(parsedPipeline->getSources().size(), 1U);

    auto stage = dynamic_cast<DocumentSourceExtensionForQueryShape*>(
        parsedPipeline->getSources().front().get());
    ASSERT_TRUE(stage != nullptr);
    ASSERT_EQUALS(std::string(stage->getSourceName()), "$optionA");

    // Assert that $optionB is unavailable.
    pipeline = {BSON("$optionB" << BSONObj())};
    ASSERT_THROWS_CODE(
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal),
        AssertionException,
        40324);
}

TEST_F(LoadExtensionsTest, LoadExtensionParseWithExtensionOptions) {
    const auto extOptions = YAML::Load("checkMax: true\nmax: 10");
    const ExtensionConfig config = {
        .sharedLibraryPath =
            test_util::getExtensionPath("libparse_options_mongo_extension.so").string(),
        .extOptions = extOptions};
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load("parse_options", config));

    std::vector<BSONObj> pipeline = {BSON("$checkNum" << BSON("num" << 9))};
    auto parsedPipeline =
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal);
    ASSERT_TRUE(parsedPipeline != nullptr);
    ASSERT_EQUALS(parsedPipeline->getSources().size(), 1U);

    auto stage = dynamic_cast<DocumentSourceExtensionForQueryShape*>(
        parsedPipeline->getSources().front().get());
    ASSERT_TRUE(stage != nullptr);
    ASSERT_EQUALS(std::string(stage->getSourceName()), "$checkNum");

    // Assert that parsing fails when the provided num is greater than the specified max of 10.
    pipeline = {BSON("$checkNum" << BSON("num" << 11))};
    ASSERT_THROWS_CODE(
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal),
        AssertionException,
        10999106);
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
    ASSERT_THROWS_CODE(
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal),
        AssertionException,
        10918500);
    ASSERT_THROWS_WHAT(
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal),
        AssertionException,
        errorMsg);
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
    ASSERT_THROWS_CODE(
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal),
        AssertionException,
        54321);
    ASSERT_THROWS_WHAT(
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal),
        AssertionException,
        "a new error");
}

using ExtensionErrorsTestDeathTest = ExtensionErrorsTest;
DEATH_TEST_REGEX_F(ExtensionErrorsTestDeathTest, ExtensionTasserts, "98765.*another new error") {
    std::vector<BSONObj> pipeline = {
        BSON("$assert" << BSON("errmsg" << "another new error" << "code" << 98765 << "assertionType"
                                        << "tassert"))};
    [[maybe_unused]] auto parsedPipeline =
        pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal);
}

// -----------------------------------------------------------------------------
// selectCompatibleVersion tests.
//
// These exercise the host-side negotiation function in isolation, without going through the full
// extension-loading machinery.
// -----------------------------------------------------------------------------

namespace {

::MongoExtensionAPIVersionVector makeVec(const std::vector<::MongoExtensionAPIVersion>& storage) {
    return {.len = storage.size(), .versions = storage.data()};
}

}  // namespace

TEST(SelectCompatibleVersionTest, NullExtensionVersionsListFiresAssertion) {
    const std::vector<::MongoExtensionAPIVersion> host = {{1, 5}};
    auto hostVec = makeVec(host);
    ::MongoExtensionAPIVersionVector extVec{.len = 0, .versions = nullptr};
    ASSERT_THROWS_CODE(selectCompatibleVersion(hostVec, extVec), AssertionException, 12688601);
}

TEST(SelectCompatibleVersionTest, EmptyExtensionVersionsListFiresAssertion) {
    const std::vector<::MongoExtensionAPIVersion> host = {{1, 5}};
    auto hostVec = makeVec(host);
    // A non-null buffer with len == 0 must still be rejected by the length guard, distinct from the
    // null-pointer guard above.
    const std::vector<::MongoExtensionAPIVersion> storage = {{1, 0}};
    ::MongoExtensionAPIVersionVector extVec{.len = 0, .versions = storage.data()};
    ASSERT_THROWS_CODE(selectCompatibleVersion(hostVec, extVec), AssertionException, 12688602);
}

TEST(SelectCompatibleVersionTest, ExactMatchReturned) {
    const std::vector<::MongoExtensionAPIVersion> host = {{1, 5}};
    const std::vector<::MongoExtensionAPIVersion> ext = {{1, 5}};
    auto hostVec = makeVec(host);
    auto extVec = makeVec(ext);
    auto chosen = selectCompatibleVersion(hostVec, extVec);
    ASSERT_EQ(chosen.major, 1u);
    ASSERT_EQ(chosen.minor, 5u);
}

TEST(SelectCompatibleVersionTest, HostMinorAheadReturnsExtensionVersion) {
    // Host supports up to {1, 5}; extension only knows {1, 3}. Forward-compat path: extension's
    // version is what gets returned (the lower bound).
    const std::vector<::MongoExtensionAPIVersion> host = {{1, 5}};
    const std::vector<::MongoExtensionAPIVersion> ext = {{1, 3}};
    auto hostVec = makeVec(host);
    auto extVec = makeVec(ext);
    auto chosen = selectCompatibleVersion(hostVec, extVec);
    ASSERT_EQ(chosen.major, 1u);
    ASSERT_EQ(chosen.minor, 3u);
}

TEST(SelectCompatibleVersionTest, ExtensionMajorTooHighFiresNoMatchingMajor) {
    const std::vector<::MongoExtensionAPIVersion> host = {{1, 5}};
    const std::vector<::MongoExtensionAPIVersion> ext = {{2, 0}};
    auto hostVec = makeVec(host);
    auto extVec = makeVec(ext);
    ASSERT_THROWS_CODE(selectCompatibleVersion(hostVec, extVec), AssertionException, 10615504);
}

TEST(SelectCompatibleVersionTest, ExtensionMajorTooLowFiresNoMatchingMajor) {
    const std::vector<::MongoExtensionAPIVersion> host = {{2, 0}};
    const std::vector<::MongoExtensionAPIVersion> ext = {{1, 0}};
    auto hostVec = makeVec(host);
    auto extVec = makeVec(ext);
    ASSERT_THROWS_CODE(selectCompatibleVersion(hostVec, extVec), AssertionException, 10615504);
}

TEST(SelectCompatibleVersionTest, ExtensionMinorAheadFiresIncompatibleMinor) {
    const std::vector<::MongoExtensionAPIVersion> host = {{1, 3}};
    const std::vector<::MongoExtensionAPIVersion> ext = {{1, 5}};
    auto hostVec = makeVec(host);
    auto extVec = makeVec(ext);
    ASSERT_THROWS_CODE(selectCompatibleVersion(hostVec, extVec), AssertionException, 10615505);
}

TEST(SelectCompatibleVersionTest, MultipleVersionsPicksTheCompatibleOne) {
    const std::vector<::MongoExtensionAPIVersion> host = {{1, 5}};
    // First entry incompatible (wrong major), second compatible.
    const std::vector<::MongoExtensionAPIVersion> ext = {{2, 0}, {1, 3}};
    auto hostVec = makeVec(host);
    auto extVec = makeVec(ext);
    auto chosen = selectCompatibleVersion(hostVec, extVec);
    ASSERT_EQ(chosen.major, 1u);
    ASSERT_EQ(chosen.minor, 3u);
}

TEST(SelectCompatibleVersionTest, PicksHighestCompatibleEvenWhenInputIsAscending) {
    // Verify the sort actually does work: feed in ascending order, expect highest returned.
    const std::vector<::MongoExtensionAPIVersion> host = {{1, 5}};
    const std::vector<::MongoExtensionAPIVersion> ext = {{1, 0}, {1, 2}, {1, 4}};
    auto hostVec = makeVec(host);
    auto extVec = makeVec(ext);
    auto chosen = selectCompatibleVersion(hostVec, extVec);
    ASSERT_EQ(chosen.major, 1u);
    ASSERT_EQ(chosen.minor, 4u);
}

TEST(SelectCompatibleVersionTest, AllMatchingMajorsButMinorsTooHighFiresIncompatibleMinor) {
    const std::vector<::MongoExtensionAPIVersion> host = {{1, 3}};
    const std::vector<::MongoExtensionAPIVersion> ext = {{1, 5}, {1, 7}, {1, 4}};
    auto hostVec = makeVec(host);
    auto extVec = makeVec(ext);
    ASSERT_THROWS_CODE(selectCompatibleVersion(hostVec, extVec), AssertionException, 10615505);
}

TEST(SelectCompatibleVersionTest, MixedNoMatchAndMinorTooHighFiresIncompatibleMinor) {
    // Even with a non-matching major in the mix, the matching-major-but-minor-too-high case wins
    // the diagnostic because at least one major did match.
    const std::vector<::MongoExtensionAPIVersion> host = {{1, 3}};
    const std::vector<::MongoExtensionAPIVersion> ext = {{2, 0}, {1, 5}};
    auto hostVec = makeVec(host);
    auto extVec = makeVec(ext);
    ASSERT_THROWS_CODE(selectCompatibleVersion(hostVec, extVec), AssertionException, 10615505);
}

TEST(SelectCompatibleVersionTest, MultipleHostMajorsPicksHighestCompatible) {
    // Forward-looking: when the host advertises two majors simultaneously, the extension's
    // highest-major-compatible version should win.
    const std::vector<::MongoExtensionAPIVersion> host = {{1, 5}, {2, 3}};
    const std::vector<::MongoExtensionAPIVersion> ext = {{1, 2}, {2, 1}};
    auto hostVec = makeVec(host);
    auto extVec = makeVec(ext);
    auto chosen = selectCompatibleVersion(hostVec, extVec);
    ASSERT_EQ(chosen.major, 2u);
    ASSERT_EQ(chosen.minor, 1u);
}

TEST_F(LoadExtensionsTest, LoadExtensionConfigFailsWhenConfigPathEmpty) {
    const auto previousExtensionsConfigPath = serverGlobalParams.extensionsConfigPath;
    ON_BLOCK_EXIT([&] { serverGlobalParams.extensionsConfigPath = previousExtensionsConfigPath; });

    serverGlobalParams.extensionsConfigPath = "";
    ASSERT_THROWS_CODE(ExtensionLoader::loadExtensionConfig("foo"), AssertionException, 12773200);
}

}  // namespace mongo::extension::host
