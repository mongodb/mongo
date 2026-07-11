// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/extension/host/aggregation_stage/executable_agg_stage.h"
#include "mongo/db/extension/host_connector/adapter/executable_agg_stage_adapter.h"
#include "mongo/db/extension/host_connector/adapter/query_execution_context_adapter.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/extension/shared/get_next_result.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo::extension {

namespace {

class ExecAggStageTest : public unittest::Test {
public:
    void setUp() override {
        _execCtx = std::make_unique<host_connector::QueryExecutionContextAdapter>(
            std::make_unique<sdk::shared_test_stages::MockQueryExecutionContext>());
    }
    std::unique_ptr<host_connector::QueryExecutionContextAdapter> _execCtx;
};

TEST_F(ExecAggStageTest, GetNextResult) {
    using ReturnStatus = exec::agg::GetNextResult::ReturnStatus;
    boost::intrusive_ptr<DocumentSourceMatch> matchDocSourceStage =
        DocumentSourceMatch::create(BSON("a" << 1), make_intrusive<ExpressionContextForTest>());

    auto mock =
        DocumentSourceMock::createForTest({DocumentSource::GetNextResult::makePauseExecution(),
                                           DocumentSource::GetNextResult::makePauseExecution(),
                                           Document{{"a", 1}},
                                           Document{{"a", 2}},
                                           Document{{"a", 2}}},
                                          make_intrusive<ExpressionContextForTest>());
    exec::agg::StagePtr mockStage = exec::agg::buildStage(mock);
    exec::agg::StagePtr matchExecAggStage =
        exec::agg::buildStageAndStitch(matchDocSourceStage, mockStage);

    std::unique_ptr<host::ExecAggStage> execAggStage =
        host::ExecAggStage::make(matchExecAggStage.get());

    // Test ExecAggStage::getNext() which calls the stored exec agg stage's getNext().
    auto getNextResult = execAggStage->getNext();
    ASSERT_EQ(ReturnStatus::kPauseExecution, getNextResult.getStatus());

    // Test getNext() via ExecAggStageHandle.
    auto hostExecAggStage = new host_connector::HostExecAggStageAdapter(std::move(execAggStage));
    auto handle = ExecAggStageHandle{hostExecAggStage};

    auto hostGetNextResult = handle->getNext(_execCtx.get());
    ASSERT_EQ(extension::GetNextCode::kPauseExecution, hostGetNextResult.code);
    ASSERT_EQ(boost::none, hostGetNextResult.resultDocument);

    hostGetNextResult = handle->getNext(_execCtx.get());
    ASSERT_EQ(extension::GetNextCode::kAdvanced, hostGetNextResult.code);
    ASSERT_TRUE(hostGetNextResult.resultDocument.has_value());
    ASSERT_BSONOBJ_EQ(BSON("a" << 1), hostGetNextResult.resultDocument->getUnownedBSONObj());

    // Note that the match clause is "a": 1 so the documents where "a": 2 will be passed over.
    hostGetNextResult = handle->getNext(_execCtx.get());
    ASSERT_EQ(extension::GetNextCode::kEOF, hostGetNextResult.code);
    ASSERT_EQ(boost::none, hostGetNextResult.resultDocument);

    // Confirm the ownership of the result was correctly passed to the caller. To do that, create a
    // dummy host_connector::HostExecAggStageAdapter so the initial underlying
    // ::MongoExtensionExecAggStage gets deleted.
    exec::agg::StagePtr duplicateMatchExecAggStage = exec::agg::buildStage(
        DocumentSourceMatch::create(BSON("a" << 2), make_intrusive<ExpressionContextForTest>()));
    std::unique_ptr<host::ExecAggStage> duplicateExecAgg =
        host::ExecAggStage::make(duplicateMatchExecAggStage.get());

    auto duplicateHostExecAggStage =
        new host_connector::HostExecAggStageAdapter(std::move(duplicateExecAgg));
    handle = ExecAggStageHandle{duplicateHostExecAggStage};

    ASSERT_EQ(extension::GetNextCode::kEOF, hostGetNextResult.code);
    ASSERT_EQ(boost::none, hostGetNextResult.resultDocument);
}

TEST_F(ExecAggStageTest, GetNextResultEdgeCaseEof) {
    using ReturnStatus = exec::agg::GetNextResult::ReturnStatus;
    boost::intrusive_ptr<DocumentSourceMatch> matchDocSourceStage =
        DocumentSourceMatch::create(BSON("a" << 1), make_intrusive<ExpressionContextForTest>());

    auto mock = DocumentSourceMock::createForTest({}, make_intrusive<ExpressionContextForTest>());
    exec::agg::StagePtr mockStage = exec::agg::buildStage(mock);
    exec::agg::StagePtr matchExecAggStage =
        exec::agg::buildStageAndStitch(matchDocSourceStage, mockStage);

    std::unique_ptr<host::ExecAggStage> execAggStage =
        host::ExecAggStage::make(matchExecAggStage.get());

    // Test getNext() via ExecAggStageHandle.
    auto hostExecAggStage = new host_connector::HostExecAggStageAdapter(std::move(execAggStage));
    auto handle = ExecAggStageHandle{hostExecAggStage};

    auto hostGetNextResult = handle->getNext(_execCtx.get());
    ASSERT_EQ(extension::GetNextCode::kEOF, hostGetNextResult.code);
    ASSERT_EQ(boost::none, hostGetNextResult.resultDocument);
}

using ExecAggStageTestDeathTest = ExecAggStageTest;
DEATH_TEST_F(ExecAggStageTestDeathTest, InvalidReturnStatusCode, "11019500") {
    using ReturnStatus = exec::agg::GetNextResult::ReturnStatus;
    // Need to set this metadata, so we don't hit the dassert in makeAdvancedControlDocument(...).
    MutableDocument doc(Document{BSON("a" << 1)});
    doc.metadata().setChangeStreamControlEvent();

    boost::intrusive_ptr<DocumentSourceMatch> matchDocSourceStage =
        DocumentSourceMatch::create(BSON("a" << 1), make_intrusive<ExpressionContextForTest>());

    auto mock = DocumentSourceMock::createForTest(
        {
            DocumentSource::GetNextResult::makeAdvancedControlDocument(doc.freeze()),
        },
        make_intrusive<ExpressionContextForTest>());
    exec::agg::StagePtr mockStage = exec::agg::buildStage(mock);
    exec::agg::StagePtr matchExecAggStage =
        exec::agg::buildStageAndStitch(matchDocSourceStage, mockStage);

    std::unique_ptr<host::ExecAggStage> execAggStage =
        host::ExecAggStage::make(matchExecAggStage.get());

    auto hostExecAggStage = new host_connector::HostExecAggStageAdapter(std::move(execAggStage));
    auto handle = ExecAggStageHandle{hostExecAggStage};

    // This getNext() call should hit the tassert because the C API doesn't have a
    // kAdvancedControlDocument value for GetNextCode.
    handle->getNext(_execCtx.get());
}

TEST_F(ExecAggStageTest, IsHostAllocated) {
    boost::intrusive_ptr<DocumentSourceMatch> matchDocSourceStage =
        DocumentSourceMatch::create(BSON("a" << 1), make_intrusive<ExpressionContextForTest>());
    exec::agg::StagePtr matchExecAggStage = exec::agg::buildStage(matchDocSourceStage);
    auto hostExecAggStage = new host_connector::HostExecAggStageAdapter(
        host::ExecAggStage::make(matchExecAggStage.get()));
    auto handle = ExecAggStageHandle{hostExecAggStage};

    ASSERT_TRUE(host_connector::HostExecAggStageAdapter::isHostAllocated(*handle.get()));
}

TEST_F(ExecAggStageTest, IsNotHostAllocated) {
    auto transformExecAggStage = new sdk::ExtensionExecAggStageAdapter(
        sdk::shared_test_stages::TransformExecAggStage::make());
    auto handle = ExecAggStageHandle{transformExecAggStage};

    ASSERT_FALSE(host_connector::HostExecAggStageAdapter::isHostAllocated(*handle.get()));
}

TEST_F(ExecAggStageTest, GetNameFromExtensionStage) {
    auto transformExecAggStage = new sdk::ExtensionExecAggStageAdapter(
        sdk::shared_test_stages::TransformExecAggStage::make());
    auto handle = ExecAggStageHandle{transformExecAggStage};

    ASSERT_EQ(handle->getName(), sdk::shared_test_stages::kTransformName);
}

TEST_F(ExecAggStageTest, GetNameFromHostStage) {
    boost::intrusive_ptr<DocumentSourceMatch> matchDocSourceStage =
        DocumentSourceMatch::create(BSON("a" << 1), make_intrusive<ExpressionContextForTest>());
    auto mock = DocumentSourceMock::createForTest({}, make_intrusive<ExpressionContextForTest>());
    exec::agg::StagePtr mockStage = exec::agg::buildStage(mock);
    exec::agg::StagePtr matchExecAggStage =
        exec::agg::buildStageAndStitch(matchDocSourceStage, mockStage);
    std::unique_ptr<host::ExecAggStage> execAggStage =
        host::ExecAggStage::make(matchExecAggStage.get());

    auto hostExecAggStage = new host_connector::HostExecAggStageAdapter(std::move(execAggStage));
    auto handle = ExecAggStageHandle{hostExecAggStage};

    ASSERT_EQ(handle->getName(), "$match");
}

TEST_F(ExecAggStageTest, DeletedCopyAndMoveConstructors) {
    static_assert(!std::is_copy_constructible_v<host_connector::HostExecAggStageAdapter>,
                  "HostExecAggStageAdapter should not be copy constructible");
    static_assert(!std::is_move_constructible_v<host_connector::HostExecAggStageAdapter>,
                  "HostExecAggStageAdapter should not be move constructible");
    static_assert(!std::is_copy_assignable_v<host_connector::HostExecAggStageAdapter>,
                  "HostExecAggStageAdapter should not be copy assignable");
    static_assert(!std::is_move_assignable_v<host_connector::HostExecAggStageAdapter>,
                  "HostExecAggStageAdapter should not be move assignable");
}

TEST(HostExecAggStageTest, ValidateCachedGetNextResultRequestDocument) {
    auto bsonResult = BSON("meow" << "santiago");
    exec::agg::GetNextResult hostResult{Document{bsonResult}};
    host_connector::CachedGetNextResult cachedResult(std::move(hostResult));
    ::MongoExtensionGetNextResult extensionGetNext = createDefaultExtensionGetNext();
    cachedResult.getAsExtensionNextResult(extensionGetNext);
    ASSERT_EQ(kByteView, extensionGetNext.resultDocument.type);
    ASSERT_BSONOBJ_EQ(bsonResult, bsonObjFromByteView(extensionGetNext.resultDocument.bytes.view));
}

}  // namespace

}  // namespace mongo::extension
