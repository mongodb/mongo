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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/extension/host/aggregation_stage/executable_agg_stage.h"
#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host/extension_stage.h"
#include "mongo/db/extension/host/query_execution_context.h"
#include "mongo/db/extension/host_connector/adapter/executable_agg_stage_adapter.h"
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/host_connector/adapter/query_execution_context_adapter.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/distributed_plan_logic.h"
#include "mongo/db/extension/sdk/dpl_array_container.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/extension/shared/get_next_result.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/distributed_plan_logic.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/dpl_array_container.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"
#include "mongo/db/pipeline/document_source_documents.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo::extension::sdk {
namespace {

class AggStageDeathTest : public unittest::Test {
public:
    void setUp() override {
        // Initialize HostServices so that aggregation stages will be able to access member
        // functions, e.g. to run assertions.
        extension::sdk::HostServicesHandle::setHostServices(
            extension::host_connector::HostServicesAdapter::get());
        _execCtx = std::make_unique<host_connector::QueryExecutionContextAdapter>(
            std::make_unique<shared_test_stages::MockQueryExecutionContext>());
    }

    std::unique_ptr<host_connector::QueryExecutionContextAdapter> _execCtx;
};

class ParseNodeVTableDeathTest : public unittest::Test {
public:
    // This special handle class is only used within this fixture so that we can unit test the
    // assertVTableConstraints functionality of the handle.
    class TestParseNodeVTableHandle : public extension::AggStageParseNodeHandle {
    public:
        TestParseNodeVTableHandle(absl::Nonnull<::MongoExtensionAggStageParseNode*> parseNode)
            : extension::AggStageParseNodeHandle(parseNode) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

class AstNodeVTableDeathTest : public unittest::Test {
public:
    class TestAstNodeVTableHandle : public extension::AggStageAstNodeHandle {
    public:
        TestAstNodeVTableHandle(absl::Nonnull<::MongoExtensionAggStageAstNode*> astNode)
            : extension::AggStageAstNodeHandle(astNode) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

class InvalidExtensionExecAggStageAdvancedState : public shared_test_stages::TransformExecAggStage {
public:
    extension::ExtensionGetNextResult getNext(const QueryExecutionContextHandle& expCtx,
                                              MongoExtensionExecAggStage* execAggStage) override {
        return {.code = extension::GetNextCode::kAdvanced};
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    static inline std::unique_ptr<ExecAggStageBase> make() {
        return std::make_unique<InvalidExtensionExecAggStageAdvancedState>();
    }
};

class InvalidExtensionExecAggStagePauseExecutionState
    : public shared_test_stages::TransformExecAggStage {
public:
    extension::ExtensionGetNextResult getNext(const QueryExecutionContextHandle& expCtx,
                                              MongoExtensionExecAggStage* execAggStage) override {
        return {.code = extension::GetNextCode::kPauseExecution,
                .resultDocument =
                    ExtensionBSONObj::makeAsByteBuf(BSON("$dog" << "I should not exist"))};
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    static inline std::unique_ptr<ExecAggStageBase> make() {
        return std::make_unique<InvalidExtensionExecAggStagePauseExecutionState>();
    }
};

class InvalidExtensionExecAggStageEofState : public shared_test_stages::TransformExecAggStage {
public:
    extension::ExtensionGetNextResult getNext(const QueryExecutionContextHandle& expCtx,
                                              MongoExtensionExecAggStage* execAggStage) override {
        return {.code = extension::GetNextCode::kEOF,
                .resultDocument =
                    ExtensionBSONObj::makeAsByteBuf(BSON("$dog" << "I should not exist"))};
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    static inline std::unique_ptr<ExecAggStageBase> make() {
        return std::make_unique<InvalidExtensionExecAggStageEofState>();
    }
};

class InvalidExtensionExecAggStageGetNextCode : public shared_test_stages::TransformExecAggStage {
public:
    extension::ExtensionGetNextResult getNext(const QueryExecutionContextHandle& expCtx,
                                              MongoExtensionExecAggStage* execAggStage) override {
        return {.code = static_cast<const GetNextCode>(10)};
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    static inline std::unique_ptr<ExecAggStageBase> make() {
        return std::make_unique<InvalidExtensionExecAggStageGetNextCode>();
    }
};

class ExecAggStageVTableDeathTest : public unittest::Test {
public:
    // This special handle class is only used within this fixture so that we can unit test the
    // assertVTableConstraints functionality of the handle.
    class TestExecAggStageVTableHandle : public extension::ExecAggStageHandle {
    public:
        TestExecAggStageVTableHandle(absl::Nonnull<::MongoExtensionExecAggStage*> execAggStage)
            : extension::ExecAggStageHandle(execAggStage) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

DEATH_TEST_F(AggStageDeathTest, EmptyDesugarExpansionFails, "11113803") {
    auto emptyDesugarParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::DesugarToEmptyParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{emptyDesugarParseNode};

    [[maybe_unused]] auto expanded = handle.expand();
}

TEST_F(AggStageDeathTest, GetExpandedSizeLessThanActualExpansionSizeFails) {
    auto getExpandedSizeLessThanActualExpansionSizeParseNode = new ExtensionAggStageParseNode(
        shared_test_stages::GetExpandedSizeLessThanActualExpansionSizeParseNode::make());
    auto handle =
        extension::AggStageParseNodeHandle{getExpandedSizeLessThanActualExpansionSizeParseNode};

    ASSERT_THROWS_CODE(
        [&] {
            [[maybe_unused]] auto expanded = handle.expand();
        }(),
        DBException,
        11113802);
}

TEST_F(AggStageDeathTest, GetExpandedSizeGreaterThanActualExpansionSizeFails) {
    auto getExpandedSizeGreaterThanActualExpansionSizeParseNode = new ExtensionAggStageParseNode(
        shared_test_stages::GetExpandedSizeGreaterThanActualExpansionSizeParseNode::make());
    auto handle =
        extension::AggStageParseNodeHandle{getExpandedSizeGreaterThanActualExpansionSizeParseNode};

    ASSERT_THROWS_CODE(
        [&] {
            [[maybe_unused]] auto expanded = handle.expand();
        }(),
        DBException,
        11113802);
}

DEATH_TEST_F(AggStageDeathTest, DescriptorAndParseNodeNameMismatchFails, "11217602") {
    auto descriptor = std::make_unique<ExtensionAggStageDescriptor>(
        shared_test_stages::NameMismatchStageDescriptor::make());
    auto handle = extension::AggStageDescriptorHandle{descriptor.get()};

    BSONObj stageBson =
        BSON(shared_test_stages::NameMismatchStageDescriptor::kStageName << BSONObj());
    [[maybe_unused]] auto parseNodeHandle = handle.parse(stageBson);
}

DEATH_TEST_F(ParseNodeVTableDeathTest, InvalidParseNodeVTableFailsGetName, "11217600") {
    auto transformParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::TransformAggStageParseNode::make());
    auto handle = TestParseNodeVTableHandle{transformParseNode};

    auto vtable = handle.vtable();
    vtable.get_name = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ParseNodeVTableDeathTest, InvalidParseNodeVTableFailsGetQueryShape, "10977600") {
    auto transformParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::TransformAggStageParseNode::make());
    auto handle = TestParseNodeVTableHandle{transformParseNode};

    auto vtable = handle.vtable();
    vtable.get_query_shape = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ParseNodeVTableDeathTest, InvalidParseNodeVTableFailsGetExpandedSize, "11113800") {
    auto transformParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::TransformAggStageParseNode::make());
    auto handle = TestParseNodeVTableHandle{transformParseNode};

    auto vtable = handle.vtable();
    vtable.get_expanded_size = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ParseNodeVTableDeathTest, InvalidParseNodeVTableFailsExpand, "10977601") {
    auto transformParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::TransformAggStageParseNode::make());
    auto handle = TestParseNodeVTableHandle{transformParseNode};

    auto vtable = handle.vtable();
    vtable.expand = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(AstNodeVTableDeathTest, InvalidAstNodeVTableFailsGetName, "11217601") {
    auto transformAstNode =
        new ExtensionAggStageAstNode(shared_test_stages::TransformAggStageAstNode::make());
    auto handle = TestAstNodeVTableHandle{transformAstNode};

    auto vtable = handle.vtable();
    vtable.get_name = nullptr;
    handle.assertVTableConstraints(vtable);
}

DEATH_TEST_F(AstNodeVTableDeathTest, InvalidAstNodeVTableBind, "11113700") {
    auto transformAstNode =
        new ExtensionAggStageAstNode(shared_test_stages::TransformAggStageAstNode::make());
    auto handle = TestAstNodeVTableHandle{transformAstNode};

    auto vtable = handle.vtable();
    vtable.bind = nullptr;
    handle.assertVTableConstraints(vtable);
}

DEATH_TEST_F(AstNodeVTableDeathTest, InvalidAstNodeVTableGetProperties, "11347800") {
    auto transformAstNode =
        new ExtensionAggStageAstNode(shared_test_stages::TransformAggStageAstNode::make());
    auto handle = TestAstNodeVTableHandle{transformAstNode};

    auto vtable = handle.vtable();
    vtable.get_properties = nullptr;
    handle.assertVTableConstraints(vtable);
}

DEATH_TEST_F(ExecAggStageVTableDeathTest, InvalidExecAggStageVTableFailsGetNext, "10956800") {
    auto transformExecAggStage = new extension::sdk::ExtensionExecAggStage(
        shared_test_stages::TransformExecAggStage::make());
    auto handle = TestExecAggStageVTableHandle{transformExecAggStage};

    auto vtable = handle.vtable();
    vtable.get_next = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ExecAggStageVTableDeathTest, InvalidExecAggStageVTableFailsSetSource, "10957202") {
    auto transformExecAggStage = new extension::sdk::ExtensionExecAggStage(
        shared_test_stages::TransformExecAggStage::make());
    auto handle = TestExecAggStageVTableHandle{transformExecAggStage};

    auto vtable = handle.vtable();
    vtable.set_source = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ExecAggStageVTableDeathTest, InvalidExecAggStageVTableFailsOpen, "11216705") {
    auto transformExecAggStage = new extension::sdk::ExtensionExecAggStage(
        shared_test_stages::TransformExecAggStage::make());
    auto handle = TestExecAggStageVTableHandle{transformExecAggStage};

    auto vtable = handle.vtable();
    vtable.open = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ExecAggStageVTableDeathTest, InvalidExecAggStageVTableFailsReopen, "11216706") {
    auto transformExecAggStage = new extension::sdk::ExtensionExecAggStage(
        shared_test_stages::TransformExecAggStage::make());
    auto handle = TestExecAggStageVTableHandle{transformExecAggStage};

    auto vtable = handle.vtable();
    vtable.reopen = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ExecAggStageVTableDeathTest, InvalidExecAggStageVTableFailsClose, "11216707") {
    auto transformExecAggStage = new extension::sdk::ExtensionExecAggStage(
        shared_test_stages::TransformExecAggStage::make());
    auto handle = TestExecAggStageVTableHandle{transformExecAggStage};

    auto vtable = handle.vtable();
    vtable.close = nullptr;
    handle.assertVTableConstraints(vtable);
};


DEATH_TEST_F(AggStageDeathTest, InvalidExtensionGetNextResultAdvanced, "10956801") {
    auto invalidExtensionExecAggStageAdvancedState = new extension::sdk::ExtensionExecAggStage(
        InvalidExtensionExecAggStageAdvancedState::make());

    auto handle = extension::ExecAggStageHandle{invalidExtensionExecAggStageAdvancedState};
    [[maybe_unused]] auto getNext = handle.getNext(_execCtx.get());
};

DEATH_TEST_F(AggStageDeathTest, InvalidExtensionGetNextResultPauseExecution, "10956802") {
    auto invalidExtensionExecAggStagePauseExecutionState =
        new extension::sdk::ExtensionExecAggStage(
            InvalidExtensionExecAggStagePauseExecutionState::make());

    auto handle = extension::ExecAggStageHandle{invalidExtensionExecAggStagePauseExecutionState};
    [[maybe_unused]] auto getNext = handle.getNext(_execCtx.get());
};

DEATH_TEST_F(AggStageDeathTest, InvalidExtensionGetNextResultEOF, "10956805") {
    auto invalidExtensionExecAggStageEofState =
        new extension::sdk::ExtensionExecAggStage(InvalidExtensionExecAggStageEofState::make());

    auto handle = extension::ExecAggStageHandle{invalidExtensionExecAggStageEofState};
    [[maybe_unused]] auto getNext = handle.getNext(_execCtx.get());
};

DEATH_TEST_F(AggStageDeathTest, InvalidMongoExtensionGetNextResultCode, "10956803") {
    ::MongoExtensionGetNextResult result = {.code =
                                                static_cast<::MongoExtensionGetNextResultCode>(10),
                                            .resultDocument = createEmptyByteContainer()};
    [[maybe_unused]] auto converted = extension::ExtensionGetNextResult::makeFromApiResult(result);
};

DEATH_TEST_F(AggStageDeathTest, InvalidGetNextCode, "10956804") {
    auto invalidExtensionExecAggStageGetNextCode =
        new extension::sdk::ExtensionExecAggStage(InvalidExtensionExecAggStageGetNextCode::make());

    auto handle = extension::ExecAggStageHandle{invalidExtensionExecAggStageGetNextCode};
    [[maybe_unused]] auto getNext = handle.getNext(_execCtx.get());
};

class TestLogicalStageCompileWithInvalidExtensionExecAggStageAdvancedState
    : public shared_test_stages::TestLogicalStageCompile {
public:
    TestLogicalStageCompileWithInvalidExtensionExecAggStageAdvancedState() {}

    std::unique_ptr<ExecAggStageBase> compile() const override {
        return std::make_unique<InvalidExtensionExecAggStageAdvancedState>();
    }

    static inline std::unique_ptr<extension::sdk::LogicalAggStage> make() {
        return std::make_unique<
            TestLogicalStageCompileWithInvalidExtensionExecAggStageAdvancedState>();
    }
};

class TestLogicalStageCompileWithInvalidExtensionExecAggStagePauseExecutionState
    : public shared_test_stages::TestLogicalStageCompile {
public:
    TestLogicalStageCompileWithInvalidExtensionExecAggStagePauseExecutionState() {}

    std::unique_ptr<ExecAggStageBase> compile() const override {
        return std::make_unique<InvalidExtensionExecAggStagePauseExecutionState>();
    }

    static inline std::unique_ptr<extension::sdk::LogicalAggStage> make() {
        return std::make_unique<
            TestLogicalStageCompileWithInvalidExtensionExecAggStagePauseExecutionState>();
    }
};

class TestLogicalStageCompileWithInvalidExtensionExecAggStageEofState
    : public shared_test_stages::TestLogicalStageCompile {
public:
    TestLogicalStageCompileWithInvalidExtensionExecAggStageEofState() {}

    std::unique_ptr<ExecAggStageBase> compile() const override {
        return std::make_unique<InvalidExtensionExecAggStageEofState>();
    }

    static inline std::unique_ptr<extension::sdk::LogicalAggStage> make() {
        return std::make_unique<TestLogicalStageCompileWithInvalidExtensionExecAggStageEofState>();
    }
};

DEATH_TEST_F(AggStageDeathTest,
             InvalidExtensionGetNextResultAdvancedFromCompiledExecAggStage,
             "10956801") {
    auto logicalStage = new extension::sdk::ExtensionLogicalAggStage(
        TestLogicalStageCompileWithInvalidExtensionExecAggStageAdvancedState::make());
    auto handle = extension::LogicalAggStageHandle{logicalStage};

    auto compiledExecAggStageHandle = handle.compile();

    [[maybe_unused]] auto getNext = compiledExecAggStageHandle.getNext(_execCtx.get());
};

DEATH_TEST_F(AggStageDeathTest,
             InvalidExtensionGetNextResultPauseExecutionFromCompiledExecAggStage,
             "10956802") {
    auto logicalStage = new extension::sdk::ExtensionLogicalAggStage(
        TestLogicalStageCompileWithInvalidExtensionExecAggStagePauseExecutionState::make());
    auto handle = extension::LogicalAggStageHandle{logicalStage};

    auto compiledExecAggStageHandle = handle.compile();

    [[maybe_unused]] auto getNext = compiledExecAggStageHandle.getNext(_execCtx.get());
};

DEATH_TEST_F(AggStageDeathTest,
             InvalidExtensionGetNextResultEOFFromCompiledExecAggStage,
             "10956805") {
    auto logicalStage = new extension::sdk::ExtensionLogicalAggStage(
        TestLogicalStageCompileWithInvalidExtensionExecAggStageEofState::make());
    auto handle = extension::LogicalAggStageHandle{logicalStage};

    auto compiledExecAggStageHandle = handle.compile();

    [[maybe_unused]] auto getNext = compiledExecAggStageHandle.getNext(_execCtx.get());
};

class TestDPLArrayContainerHandle : public DPLArrayContainerHandle {
public:
    TestDPLArrayContainerHandle(::MongoExtensionDPLArrayContainer* container)
        : DPLArrayContainerHandle(container) {}

    void transferInternal(::MongoExtensionDPLArray& arr) {
        assertValid();
        _transferInternal(arr);
    }

    void assertVTableConstraints(const VTable_t& vtable) {
        _assertVTableConstraints(vtable);
    }
};

DEATH_TEST_F(AggStageDeathTest, InvalidDPLArrayContainerVTableFailsSize, "11368301") {
    auto handle = TestDPLArrayContainerHandle{new sdk::ExtensionDPLArrayContainerAdapter(
        sdk::DPLArrayContainer(std::vector<VariantDPLHandle>{}))};

    auto vtable = handle.vtable();
    vtable.size = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(AggStageDeathTest, InvalidDPLArrayContainerVTableFailsTransfer, "11368302") {
    auto handle = TestDPLArrayContainerHandle{new sdk::ExtensionDPLArrayContainerAdapter(
        sdk::DPLArrayContainer(std::vector<VariantDPLHandle>{}))};

    auto vtable = handle.vtable();
    vtable.transfer = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(AggStageDeathTest, DPLArrayContainerExtensionToHostWrongSizeFails, "11368303") {
    auto logicalStage =
        new sdk::ExtensionLogicalAggStage(shared_test_stages::CountingLogicalStage::make());
    auto parseNode = new sdk::ExtensionAggStageParseNode(shared_test_stages::CountingParse::make());

    std::vector<VariantDPLHandle> sdkElements;
    sdkElements.emplace_back(extension::LogicalAggStageHandle{logicalStage});
    sdkElements.emplace_back(extension::AggStageParseNodeHandle{parseNode});

    const auto size = sdkElements.size();

    auto sdkAdapter = std::make_unique<sdk::ExtensionDPLArrayContainerAdapter>(
        sdk::DPLArrayContainer(std::move(sdkElements)));

    TestDPLArrayContainerHandle arrayContainerHandle(sdkAdapter.release());

    // Pre-allocate array with incorrect size.
    std::vector<::MongoExtensionDPLArrayElement> sdkAbiArray{size + 1};
    ::MongoExtensionDPLArray targetArray{size + 1, sdkAbiArray.data()};

    // Transfer should fail due to incorrectly sized array.
    arrayContainerHandle.transferInternal(targetArray);
}

class DistributedPlanLogicVTableDeathTest : public unittest::Test {
public:
    class TestDistributedPlanLogicVTableHandle : public DistributedPlanLogicHandle {
    public:
        TestDistributedPlanLogicVTableHandle(
            ::MongoExtensionDistributedPlanLogic* distributedPlanLogic)
            : DistributedPlanLogicHandle(distributedPlanLogic) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

DEATH_TEST_F(DistributedPlanLogicVTableDeathTest, InvalidDPLVTableFailsGetShards, "11027300") {
    auto distributedPlanLogic =
        new sdk::ExtensionDistributedPlanLogicAdapter(sdk::DistributedPlanLogic{});
    auto handle = TestDistributedPlanLogicVTableHandle{distributedPlanLogic};

    auto vtable = handle.vtable();
    vtable.extract_shards_pipeline = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(DistributedPlanLogicVTableDeathTest, InvalidDPLVTableFailsGetMerging, "11027301") {
    auto distributedPlanLogic =
        new sdk::ExtensionDistributedPlanLogicAdapter(sdk::DistributedPlanLogic{});
    auto handle = TestDistributedPlanLogicVTableHandle{distributedPlanLogic};

    auto vtable = handle.vtable();
    vtable.extract_merging_pipeline = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(DistributedPlanLogicVTableDeathTest, InvalidDPLVTableFailsGetSortPattern, "11027302") {
    auto distributedPlanLogic =
        new sdk::ExtensionDistributedPlanLogicAdapter(sdk::DistributedPlanLogic{});
    auto handle = TestDistributedPlanLogicVTableHandle{distributedPlanLogic};

    auto vtable = handle.vtable();
    vtable.get_sort_pattern = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(AggStageDeathTest, SourceStageForTransformExtensionStageBecomesInvalid, "10957209") {
    // This test verifies that if the source stage becomes invalid or is deleted,
    // the extension stage will safely fail when trying to access it.
    QueryTestServiceContext testCtx;
    auto opCtx = testCtx.makeOperationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(
        opCtx.get(),
        NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd),
        SerializationContext());

    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::AddFruitsToDocumentsAggStageAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(expCtx, std::move(astHandle));

    auto docSourcesList = DocumentSourceDocuments::createFromBson(
        BSON("$documents" << BSON_ARRAY(BSON("sourceField" << 1))).firstElement(), expCtx);

    std::vector<boost::intrusive_ptr<exec::agg::Stage>> stages;
    for (auto& docSource : docSourcesList) {
        stages.push_back(exec::agg::buildStage(docSource));
    }

    // Stitch stages returned from DocumentSourceDocuments::createFromBson(...) together.
    for (size_t i = 1; i < stages.size(); ++i) {
        stages[i]->setSource(stages[i - 1].get());
    }

    // Set the last stage as source for the extension stage.
    boost::intrusive_ptr<exec::agg::Stage> extensionStage =
        exec::agg::buildStageAndStitch(optimizable, stages.back().get());

    // getNext() should work fine here because the source stage is still valid.
    auto result = extensionStage->getNext();
    ASSERT_TRUE(result.isAdvanced());

    extensionStage->setSource(nullptr);
    // Hits tassert because the stage provided was null, clearing out our predecessor.
    result = extensionStage->getNext();
};

DEATH_TEST_F(AggStageDeathTest, NoSourceStageForTransformStage, "10957209") {
    auto transformStage = shared_test_stages::AddFruitsToDocumentsExecAggStage::make();

    QueryTestServiceContext testCtx;
    auto opCtx = testCtx.makeOperationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(
        opCtx.get(),
        NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd),
        SerializationContext());

    host::QueryExecutionContext wrappedCtx(expCtx.get());
    host_connector::QueryExecutionContextAdapter ctxAdapter(
        std::make_unique<host::QueryExecutionContext>(expCtx.get()));
    // Call getNext() without setting a source.
    [[maybe_unused]] auto result = transformStage->getNext(&ctxAdapter, nullptr);
}

DEATH_TEST_F(AggStageDeathTest, HostExecAggStageAdapterNullStageAsserts, "10957207") {
    [[maybe_unused]] auto adapter = host_connector::HostExecAggStageAdapter{nullptr};
}

DEATH_TEST_F(AggStageDeathTest, SetSourceOnSourceStageFails, "10957210") {
    // Setting the source of a source stage should fail irrespective of the type of the stage being
    // set as the source.
    auto sourceHandle = extension::ExecAggStageHandle{new extension::sdk::ExtensionExecAggStage(
        shared_test_stages::AddFruitsToDocumentsExecAggStage::make())};
    // ValidExtensionExecAggStage is a source stage.
    auto handle = extension::ExecAggStageHandle{new extension::sdk::ExtensionExecAggStage(
        shared_test_stages::ValidExtensionExecAggStage::make())};

    // Calling setSource on a source stage should fail.
    handle.setSource(sourceHandle);
}

DEATH_TEST_F(AggStageDeathTest, GetSourceOnSourceStageFails, "10957208") {
    shared_test_stages::FruitsAsDocumentsExecAggStage sourceStage{"", BSONObj()};

    // Calling getSource on a source stage should fail.
    [[maybe_unused]] auto source = sourceStage._getSource();
}

DEATH_TEST_F(AggStageDeathTest, GetNameOnMovedHandleFails, "10596403") {
    auto sourceHandle = extension::ExecAggStageHandle{new extension::sdk::ExtensionExecAggStage(
        shared_test_stages::AddFruitsToDocumentsExecAggStage::make())};

    auto sourceHandle2 = std::move(sourceHandle);

    // Calling getName on a source handle should fail.
    [[maybe_unused]] auto source = sourceHandle.getName();
}

}  // namespace
}  // namespace mongo::extension::sdk
