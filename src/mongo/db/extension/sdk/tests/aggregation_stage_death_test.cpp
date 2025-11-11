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
#include "mongo/db/extension/host/query_execution_context.h"
#include "mongo/db/extension/host_connector/host_services_adapter.h"
#include "mongo/db/extension/host_connector/query_execution_context_adapter.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/extension/shared/get_next_result.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"
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
        _execCtx = std::make_unique<host_connector::QueryExecutionContextAdapter>(nullptr);
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

class InvalidExtensionExecAggStageAdvancedState
    : public shared_test_stages::NoOpExtensionExecAggStage {
public:
    extension::ExtensionGetNextResult getNext(
        const QueryExecutionContextHandle& expCtx,
        const MongoExtensionExecAggStage* execAggStage) override {
        return {.code = extension::GetNextCode::kAdvanced, .res = boost::none};
    }

    static inline std::unique_ptr<extension::sdk::ExecAggStage> make() {
        return std::make_unique<InvalidExtensionExecAggStageAdvancedState>();
    }
};

class InvalidExtensionExecAggStagePauseExecutionState
    : public shared_test_stages::NoOpExtensionExecAggStage {
public:
    extension::ExtensionGetNextResult getNext(
        const QueryExecutionContextHandle& expCtx,
        const MongoExtensionExecAggStage* execAggStage) override {
        return {.code = extension::GetNextCode::kPauseExecution,
                .res = boost::make_optional(BSON("$dog" << "I should not exist"))};
    }

    static inline std::unique_ptr<extension::sdk::ExecAggStage> make() {
        return std::make_unique<InvalidExtensionExecAggStagePauseExecutionState>();
    }
};

class InvalidExtensionExecAggStageEofState : public shared_test_stages::NoOpExtensionExecAggStage {
public:
    extension::ExtensionGetNextResult getNext(
        const QueryExecutionContextHandle& expCtx,
        const MongoExtensionExecAggStage* execAggStage) override {
        return {.code = extension::GetNextCode::kEOF,
                .res = boost::make_optional(BSON("$dog" << "I should not exist"))};
    }

    static inline std::unique_ptr<extension::sdk::ExecAggStage> make() {
        return std::make_unique<InvalidExtensionExecAggStageEofState>();
    }
};

class InvalidExtensionExecAggStageGetNextCode
    : public shared_test_stages::NoOpExtensionExecAggStage {
public:
    extension::ExtensionGetNextResult getNext(
        const QueryExecutionContextHandle& expCtx,
        const MongoExtensionExecAggStage* execAggStage) override {
        return {.code = static_cast<const GetNextCode>(10), .res = boost::none};
    }

    static inline std::unique_ptr<extension::sdk::ExecAggStage> make() {
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
    auto noOpParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::NoOpAggStageParseNode::make());
    auto handle = TestParseNodeVTableHandle{noOpParseNode};

    auto vtable = handle.vtable();
    vtable.get_name = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ParseNodeVTableDeathTest, InvalidParseNodeVTableFailsGetQueryShape, "10977600") {
    auto noOpParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::NoOpAggStageParseNode::make());
    auto handle = TestParseNodeVTableHandle{noOpParseNode};

    auto vtable = handle.vtable();
    vtable.get_query_shape = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ParseNodeVTableDeathTest, InvalidParseNodeVTableFailsGetExpandedSize, "11113800") {
    auto noOpParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::NoOpAggStageParseNode::make());
    auto handle = TestParseNodeVTableHandle{noOpParseNode};

    auto vtable = handle.vtable();
    vtable.get_expanded_size = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ParseNodeVTableDeathTest, InvalidParseNodeVTableFailsExpand, "10977601") {
    auto noOpParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::NoOpAggStageParseNode::make());
    auto handle = TestParseNodeVTableHandle{noOpParseNode};

    auto vtable = handle.vtable();
    vtable.expand = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(AstNodeVTableDeathTest, InvalidAstNodeVTableFailsGetName, "11217601") {
    auto noOpAstNode =
        new ExtensionAggStageAstNode(shared_test_stages::NoOpAggStageAstNode::make());
    auto handle = TestAstNodeVTableHandle{noOpAstNode};

    auto vtable = handle.vtable();
    vtable.get_name = nullptr;
    handle.assertVTableConstraints(vtable);
}

DEATH_TEST_F(AstNodeVTableDeathTest, InvalidAstNodeVTableBind, "11113700") {
    auto noOpAstNode =
        new ExtensionAggStageAstNode(shared_test_stages::NoOpAggStageAstNode::make());
    auto handle = TestAstNodeVTableHandle{noOpAstNode};

    auto vtable = handle.vtable();
    vtable.bind = nullptr;
    handle.assertVTableConstraints(vtable);
}

DEATH_TEST_F(AstNodeVTableDeathTest, InvalidAstNodeVTableGetProperties, "11347800") {
    auto noOpAstNode =
        new ExtensionAggStageAstNode(shared_test_stages::NoOpAggStageAstNode::make());
    auto handle = TestAstNodeVTableHandle{noOpAstNode};

    auto vtable = handle.vtable();
    vtable.get_properties = nullptr;
    handle.assertVTableConstraints(vtable);
}

DEATH_TEST_F(ExecAggStageVTableDeathTest, InvalidExecAggStageVTableFailsGetNext, "10956800") {
    auto noOpExecAggStage = new extension::sdk::ExtensionExecAggStage(
        shared_test_stages::NoOpExtensionExecAggStage::make());
    auto handle = TestExecAggStageVTableHandle{noOpExecAggStage};

    auto vtable = handle.vtable();
    vtable.get_next = nullptr;
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
    ::MongoExtensionGetNextResult result = {
        .code = static_cast<::MongoExtensionGetNextResultCode>(10), .result = nullptr};
    [[maybe_unused]] auto converted = extension::convertCRepresentationToGetNextResult(&result);
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

    std::unique_ptr<ExecAggStage> compile() const override {
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

    std::unique_ptr<ExecAggStage> compile() const override {
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

    std::unique_ptr<ExecAggStage> compile() const override {
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

}  // namespace

}  // namespace mongo::extension::sdk
