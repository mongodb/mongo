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

#include "mongo/db/pipeline/stage_params_to_document_source_registry.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

/**
 * A test-only StageParams class used for testing unregistered stages.
 */
DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(UnregisteredTest);
ALLOCATE_STAGE_PARAMS_ID(unregisteredTest, UnregisteredTestStageParams::id);

/**
 * A test-only LiteParsedDocumentSource that is intentionally NOT registered
 * in the StageParams to DocumentSource registry.
 */
class UnregisteredTestLiteParsed final : public LiteParsedDocumentSource {
public:
    UnregisteredTestLiteParsed(const BSONElement& originalBson)
        : LiteParsedDocumentSource(originalBson) {}

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return stdx::unordered_set<NamespaceString>();
    }

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return {};
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<UnregisteredTestStageParams>(_originalBson);
    }
};

/**
 * Test that buildDocumentSource returns boost::none for stages that are not registered
 * in the StageParams to DocumentSource registry.
 *
 * This behavior allows the system to fall back to the traditional BSON-based parsing
 * path for stages that haven't been migrated yet.
 */
TEST(StageParamsToDocumentSourceRegistryTest, UnregisteredStageReturnsNone) {
    BSONObj spec = BSON("$unregisteredTestStage" << BSONObj());
    auto liteParsed = UnregisteredTestLiteParsed(spec.firstElement());
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    auto result = buildDocumentSource(liteParsed, expCtx);

    ASSERT_FALSE(result.has_value());
}

/**
 * Test that buildDocumentSource returns a valid DocumentSource for stages that ARE
 * registered in the StageParams to DocumentSource registry.
 *
 * This test uses $limit as an example since it's one of the stages that has been
 * migrated to use the new registry.
 */
TEST(StageParamsToDocumentSourceRegistryTest, RegisteredStageReturnsDocumentSource) {
    BSONObj spec = BSON("$limit" << 10);
    auto liteParsed = LimitLiteParsed(spec.firstElement());
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    auto result = buildDocumentSource(liteParsed, expCtx);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result.get() != nullptr);

    // Verify it's actually a DocumentSourceLimit.
    auto* limitDS = dynamic_cast<DocumentSourceLimit*>(result.get().get());
    ASSERT_TRUE(limitDS != nullptr);
    ASSERT_EQ(limitDS->getLimit(), 10);
}

/**
 * A test-only StageParams class used specifically for testing duplicate registration.
 */
DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(DuplicateRegistrationTest);
ALLOCATE_STAGE_PARAMS_ID(duplicateRegistrationTest, DuplicateRegistrationTestStageParams::id);

// A dummy mapping function for the duplicate registration test.
boost::intrusive_ptr<DocumentSource> dummyMappingFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return nullptr;
}

// Register the first mapping for the duplicate registration test.
REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING(duplicateRegistrationTest,
                                                 "$duplicateRegistrationTestStage",
                                                 DuplicateRegistrationTestStageParams::id,
                                                 dummyMappingFn);

/**
 * Test that registering a duplicate mapping function for the same StageParams::Id
 * triggers a tassert failure.
 *
 * This ensures that each stage can only have one registered mapping function,
 * preventing accidental overwrites or conflicting implementations.
 */
DEATH_TEST(StageParamsToDocumentSourceRegistryTest, DuplicateRegistrationFails, "11458700") {
    // Attempt to register a second mapping function for the same StageParams::Id.
    // This should trigger a tassert.
    registerStageParamsToDocumentSourceFn("$duplicateRegistrationTestStage",
                                          DuplicateRegistrationTestStageParams::id,
                                          dummyMappingFn);
}

/**
 * A test-only StageParams class used for testing overlapping registration.
 */
DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(OverlappingRegistrationTest);
ALLOCATE_STAGE_PARAMS_ID(overlappingRegistrationTest, OverlappingRegistrationTestStageParams::id);

/**
 * Test that registering a stage that is already in the old parserMap triggers a tassert failure.
 *
 * This validates that stages cannot be registered in both the new StageParams->DocumentSource
 * registry and the old parserMap simultaneously.
 */
DEATH_TEST(StageParamsToDocumentSourceRegistryTest,
           RegistrationFailsForStageInParserMap,
           "11458701") {
    // $match is registered in the old parserMap, so registering it in the new registry
    // should trigger a tassert.
    registerStageParamsToDocumentSourceFn(
        "$match", OverlappingRegistrationTestStageParams::id, dummyMappingFn);
}

}  // namespace
}  // namespace mongo

