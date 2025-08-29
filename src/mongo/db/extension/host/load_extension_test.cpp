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
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <filesystem>

namespace mongo::extension::host {

static const std::filesystem::path runFilesDir = std::getenv("RUNFILES_DIR");
static const std::filesystem::path kExtensionDirectory =
    "_main/src/mongo/db/extension/test_examples";

static std::filesystem::path getExtensionPath(const std::string& extensionName) {
    return runFilesDir / kExtensionDirectory / extensionName;
}

class LoadExtensionsTest : public unittest::Test {
protected:
    static inline const std::string kTestFooStageName = "$testFoo";
    static inline const std::string kTestFooLibExtensionPath = "libfoo_mongo_extension.so";
    static inline const std::string kTestBarLibExtensionPath = "libbar_mongo_extension.so";
    // TODO SERVER-109108: Remove this when we can use only libfoo_mongo_extension.so.
    static inline const std::string kTestBuzzLibExtensionPath = "libbuzz_mongo_extension.so";

private:
    RAIIServerParameterControllerForTest _featureFlag{"featureFlagExtensionsAPI", true};
};

TEST_F(LoadExtensionsTest, LoadExtensionErrorCases) {
    // Ensure that the RUNFILES_DIR environment variable is set, which is required for this test.
    // This variable is typically set by the Bazel build system, so this test must be run using
    // Bazel.
    ASSERT(!runFilesDir.empty());

    // Test that various non-existent extension cases fail with the proper error code.
    ASSERT_THROWS_CODE(ExtensionLoader::load("src/"), AssertionException, 10615500);
    ASSERT_THROWS_CODE(ExtensionLoader::load("notanextension"), AssertionException, 10615500);
    ASSERT_THROWS_CODE(
        ExtensionLoader::load("path/to/nonexistent/extension.so"), AssertionException, 10615500);
    ASSERT_THROWS_CODE(ExtensionLoader::load(getExtensionPath("libnotanextension.so")),
                       AssertionException,
                       10615500);

    // no_symbol_bad_extension is missing the get_mongodb_extension symbol definition.
    ASSERT_THROWS_CODE(ExtensionLoader::load(getExtensionPath("libno_symbol_bad_extension.so")),
                       AssertionException,
                       10615501);

    // null_mongo_extension_bad_extension returns null from get_mongodb_extension.
    ASSERT_THROWS_CODE(
        ExtensionLoader::load(getExtensionPath("libnull_mongo_extension_bad_extension.so")),
        AssertionException,
        10615503);

    // major_version_too_high_bad_extension has an incompatible major version (plus 1).
    ASSERT_THROWS_CODE(
        ExtensionLoader::load(getExtensionPath("libmajor_version_too_high_bad_extension.so")),
        AssertionException,
        10615504);

    // major_version_too_low_bad_extension has an incompatible major version (minus 1).
    ASSERT_THROWS_CODE(
        ExtensionLoader::load(getExtensionPath("libmajor_version_too_low_bad_extension.so")),
        AssertionException,
        10615504);

    // minor_version_too_high_bad_extension has an incompatible minor version.
    ASSERT_THROWS_CODE(
        ExtensionLoader::load(getExtensionPath("libminor_version_too_high_bad_extension.so")),
        AssertionException,
        10615505);

    // major_version_max_int_bad_extension has the maximum uint32_t value as its major version.
    ASSERT_THROWS_CODE(
        ExtensionLoader::load(getExtensionPath("libmajor_version_max_int_bad_extension.so")),
        AssertionException,
        10615504);

    // duplicate_stage_descriptor_bad_extension attempts to register the same stage descriptor
    // multiple times.
    ASSERT_THROWS_CODE(
        ExtensionLoader::load(getExtensionPath("libduplicate_stage_descriptor_bad_extension.so")),
        AssertionException,
        10696402);

    ASSERT_THROWS_CODE(
        ExtensionLoader::load(getExtensionPath("libduplicate_version_bad_extension.so")),
        AssertionException,
        10930201);

    ASSERT_THROWS_CODE(
        ExtensionLoader::load(getExtensionPath("libno_compatible_version_bad_extension.so")),
        AssertionException,
        10930202);
}

// null_initialize_function_bad_extension has a null initialization function.
DEATH_TEST_F(LoadExtensionsTest, LoadExtensionNullInitialize, "10930101") {
    ExtensionLoader::load(getExtensionPath("libnull_initialize_function_bad_extension.so"));
}

// TODO SERVER-109108: Switch this to use the foo extension once we can reset state in between
// tests.
TEST_F(LoadExtensionsTest, RepetitivelyLoadingTheSameExtensionFails) {
    // We should be able to load the extension once.
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load(getExtensionPath(kTestBuzzLibExtensionPath)));

    // We should not be able to load the extension twice.
    ASSERT_THROWS_CODE(ExtensionLoader::load(getExtensionPath(kTestBuzzLibExtensionPath)),
                       AssertionException,
                       10845400);
}

// TODO SERVER-109108: Switch this to use the foo extension once we can reset state in between
// tests.
TEST_F(LoadExtensionsTest, RepetitivelyLoadingTheSameExtensionViaLoadExtensionsFails) {
    // The following should return failure when trying to load the extension the second time.
    ASSERT_FALSE(loadExtensions(
        std::vector<std::string>{kTestBarLibExtensionPath, kTestBarLibExtensionPath}));
}

// Tests successful extension loading and verifies stage registration works in pipelines.
// The libfoo_mongo_extension.so adds a "$testFoo" stage for testing.
TEST_F(LoadExtensionsTest, LoadExtensionSucceeds) {
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load(getExtensionPath(kTestFooLibExtensionPath)));

    // Verify the initialization function registered a stage.
    BSONObj stageSpec = BSON(kTestFooStageName << BSONObj());
    auto expCtx = make_intrusive<ExpressionContextForTest>();

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

// Tests that extension initialization properly populates the parser map before and after loading.
TEST_F(LoadExtensionsTest, InitializationFunctionPopulatesParserMap) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    BSONObj stageSpec = BSON(kTestFooStageName << BSONObj());

    // Since the extension might already be loaded by previous tests, just verify it works.
    ASSERT_DOES_NOT_THROW({
        auto sourceList = DocumentSource::parse(expCtx, stageSpec);
        ASSERT_EQUALS(sourceList.size(), 1U);

        auto extensionStage = dynamic_cast<DocumentSourceExtension*>(sourceList.front().get());
        ASSERT_TRUE(extensionStage != nullptr);
        ASSERT_EQUALS(std::string(extensionStage->getSourceName()), std::string(kTestFooStageName));
    });
}

TEST_F(LoadExtensionsTest, LoadExtensionHostVersionParameterSucceeds) {
    ASSERT_DOES_NOT_THROW(
        ExtensionLoader::load(getExtensionPath("libhostVersionSucceeds_mongo_extension.so")));
}

TEST_F(LoadExtensionsTest, LoadExtensionHostVersionParameterFails) {
    ASSERT_THROWS_CODE(
        ExtensionLoader::load(getExtensionPath("libhost_version_fails_bad_extension.so")),
        AssertionException,
        10615503);
}

TEST_F(LoadExtensionsTest, LoadExtensioninitialize_version_fails) {
    ASSERT_THROWS_CODE(
        ExtensionLoader::load(getExtensionPath("libinitialize_version_fails_bad_extension.so")),
        AssertionException,
        10726600);
}

DEATH_TEST_F(LoadExtensionsTest, LoadExtensionnull_stage_descriptor, "10596400") {
    ExtensionLoader::load(getExtensionPath("libnull_stage_descriptor_bad_extension.so"));
}

TEST(LoadExtensionTest, LoadExtensionTwoStagesSucceeds) {
    ASSERT_DOES_NOT_THROW(
        ExtensionLoader::load(getExtensionPath("libloadTwoStages_mongo_extension.so")));
    auto expCtx = make_intrusive<ExpressionContextForTest>();

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

TEST(LoadExtensionTest, LoadHighestCompatibleVersionSucceeds) {
    ASSERT_DOES_NOT_THROW(ExtensionLoader::load(
        getExtensionPath("libloadHighestCompatibleVersion_mongo_extension.so")));
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    std::vector<BSONObj> pipeline = {BSON("$extensionV3" << BSONObj())};
    auto parsedPipeline = Pipeline::parse(pipeline, expCtx);
    ASSERT_TRUE(parsedPipeline != nullptr);
    ASSERT_EQUALS(parsedPipeline->getSources().size(), 1U);

    auto extensionStage =
        dynamic_cast<DocumentSourceExtension*>(parsedPipeline->getSources().front().get());

    ASSERT_TRUE(extensionStage != nullptr);
    ASSERT_EQUALS(std::string(extensionStage->getSourceName()), "$extensionV3");

    // Assert that the other extension versions registered aren't available.
    pipeline = {BSON("$extensionV1" << BSONObj())};
    ASSERT_THROWS_CODE(Pipeline::parse(pipeline, expCtx), AssertionException, 16436);
    pipeline = {BSON("$extensionV2" << BSONObj())};
    ASSERT_THROWS_CODE(Pipeline::parse(pipeline, expCtx), AssertionException, 16436);
    pipeline = {BSON("$extensionV4" << BSONObj())};
    ASSERT_THROWS_CODE(Pipeline::parse(pipeline, expCtx), AssertionException, 16436);
}
}  // namespace mongo::extension::host
