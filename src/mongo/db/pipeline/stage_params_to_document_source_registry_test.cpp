// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/stage_params_to_document_source_registry.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/document_source.h"
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
class UnregisteredTestLiteParsed final
    : public LiteParsedDocumentSourceDefault<UnregisteredTestLiteParsed> {
public:
    UnregisteredTestLiteParsed(const BSONElement& originalBson)
        : LiteParsedDocumentSourceDefault(originalBson) {}

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
 * Test that buildDocumentSource tasserts for stages that are not registered
 * in the StageParams to DocumentSource registry.
 */
DEATH_TEST(StageParamsToDocumentSourceRegistryDeathTest, UnregisteredStageReturnsNone, "11434300") {
    BSONObj spec = BSON("$unregisteredTestStage" << BSONObj());
    auto liteParsed = UnregisteredTestLiteParsed(spec.firstElement());
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    auto result = buildDocumentSource(liteParsed, expCtx);
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

    ASSERT_EQ(result.size(), 1);
    ASSERT_TRUE(result.front() != nullptr);

    // Verify it's actually a DocumentSourceLimit.
    auto* limitDS = dynamic_cast<DocumentSourceLimit*>(result.front().get());
    ASSERT_TRUE(limitDS != nullptr);
    ASSERT_EQ(limitDS->getLimit(), 10);
}

/**
 * Test that the StageParams-direct overload of buildDocumentSource dispatches correctly
 * for a registered stage, without going through LiteParsedDocumentSource.
 */
TEST(StageParamsToDocumentSourceRegistryTest, DirectStageParamsDispatchReturnsDocumentSource) {
    BSONObj spec = BSON("$limit" << 10);
    auto liteParsed = LimitLiteParsed(spec.firstElement());
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto stageParams = liteParsed.getStageParams();

    auto result = buildDocumentSource(stageParams, expCtx);

    ASSERT_EQ(result.size(), 1);
    auto* limitDS = dynamic_cast<DocumentSourceLimit*>(result.front().get());
    ASSERT_TRUE(limitDS != nullptr);
    ASSERT_EQ(limitDS->getLimit(), 10);
}

/**
 * Test that the StageParams-direct overload tasserts for unregistered stages.
 */
DEATH_TEST(StageParamsToDocumentSourceRegistryDeathTest,
           DirectStageParamsDispatchUnregisteredStage,
           "12788401") {
    std::unique_ptr<StageParams> stageParams =
        std::make_unique<UnregisteredTestStageParams>(BSONElement{});
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    buildDocumentSource(stageParams, expCtx);
}

/**
 * A test-only StageParams class used specifically for testing duplicate registration.
 */
DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(DuplicateRegistrationTest);
ALLOCATE_STAGE_PARAMS_ID(duplicateRegistrationTest, DuplicateRegistrationTestStageParams::id);

// A dummy mapping function for the duplicate registration test.
DocumentSourceContainer dummyMappingFn(const std::unique_ptr<StageParams>& stageParams,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return {};
}

// Register the first mapping for the duplicate registration test.
REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING(duplicateRegistrationTest,
                                                 DuplicateRegistrationTestStageParams::id,
                                                 dummyMappingFn);

/**
 * Test that registering a duplicate mapping function for the same StageParams::Id
 * triggers a tassert failure.
 *
 * This ensures that each stage can only have one registered mapping function,
 * preventing accidental overwrites or conflicting implementations.
 */
DEATH_TEST(StageParamsToDocumentSourceRegistryDeathTest, DuplicateRegistrationFails, "11458700") {
    // Attempt to register a second mapping function for the same StageParams::Id.
    // This should trigger a tassert.
    registerStageParamsToDocumentSourceFn(DuplicateRegistrationTestStageParams::id, dummyMappingFn);
}

}  // namespace
}  // namespace mongo
