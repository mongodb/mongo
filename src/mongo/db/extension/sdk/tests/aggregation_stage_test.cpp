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

#include "mongo/db/extension/sdk/aggregation_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/host/query_execution_context.h"
#include "mongo/db/extension/host_connector/host_services_adapter.h"
#include "mongo/db/extension/host_connector/query_execution_context_adapter.h"
#include "mongo/db/extension/host_connector/query_shape_opts_adapter.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/query_shape_opts_handle.h"
#include "mongo/db/extension/sdk/raii_vector_to_abi_array.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/extension/shared/array/abi_array_to_raii_vector.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/get_next_result.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::extension::sdk {

namespace {

template <class Variant>
const extension::AggStageAstNodeHandle& asAst(const Variant& v) {
    ASSERT_TRUE(std::holds_alternative<extension::AggStageAstNodeHandle>(v));
    return std::get<extension::AggStageAstNodeHandle>(v);
}

template <class Variant>
const extension::AggStageParseNodeHandle& asParse(const Variant& v) {
    ASSERT_TRUE(std::holds_alternative<extension::AggStageParseNodeHandle>(v));
    return std::get<extension::AggStageParseNodeHandle>(v);
}

class AggStageTest : public unittest::Test {
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

class ExpandToIdLookupNode : public extension::sdk::AggStageParseNode {
public:
    ExpandToIdLookupNode() : extension::sdk::AggStageParseNode("expandToIdLookup") {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        auto spec = BSON("$_internalSearchIdLookup" << BSONObj());
        expanded.emplace_back(
            extension::sdk::HostServicesHandle::getHostServices()->createIdLookup(spec));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<extension::sdk::AggStageParseNode> make() {
        return std::make_unique<ExpandToIdLookupNode>();
    }
};

TEST_F(AggStageTest, CountingParseExpansionSucceedsTest) {
    auto countingParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::CountingParse::make());
    auto handle = extension::AggStageParseNodeHandle{countingParseNode};

    auto expanded = handle.expand();
    ASSERT_EQUALS(expanded.size(), 1);

    const auto& astHandle = asAst(expanded[0]);
    ASSERT_EQ(astHandle.getName(), stringViewToStringData(shared_test_stages::kCountingName));
}

TEST_F(AggStageTest, NestedExpansionSucceedsTest) {
    auto nestedDesugarParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::NestedDesugaringParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{nestedDesugarParseNode};

    auto expanded = handle.expand();
    ASSERT_EQUALS(expanded.size(), 2);

    // First element is the AstNode.
    const auto& firstAstHandle = asAst(expanded[0]);
    ASSERT_EQ(firstAstHandle.getName(), stringViewToStringData(shared_test_stages::kCountingName));

    // Second element is the nested ParseNode.
    const auto& nestedParseHandle = asParse(expanded[1]);
    ASSERT_EQ(nestedParseHandle.getName(),
              stringViewToStringData(shared_test_stages::kCountingName));

    // Expand the nested node.
    auto nestedExpanded = nestedParseHandle.expand();
    ASSERT_EQUALS(nestedExpanded.size(), 1);

    const auto& nestedAstHandle = asAst(nestedExpanded[0]);
    ASSERT_EQ(nestedAstHandle.getName(), stringViewToStringData(shared_test_stages::kCountingName));
}

TEST_F(AggStageTest, ExpansionToHostParseNodeSucceeds) {
    auto expandToHostParseNode = std::make_unique<ExtensionAggStageParseNode>(
        shared_test_stages::ExpandToHostParseNode::make());

    // Transfer ownership from the SDK-style unique_ptr to the OwnedHandle.
    auto handle = extension::AggStageParseNodeHandle{expandToHostParseNode.release()};

    auto expanded = handle.expand();
    ASSERT_EQUALS(expanded.size(), 1);

    ASSERT_TRUE(std::holds_alternative<extension::AggStageParseNodeHandle>(expanded[0]));

    const auto& expandHandle = asParse(expanded[0]);
    ASSERT_EQUALS(expandHandle.getName(),
                  stringViewToStringData(shared_test_stages::kExpandToHostName));
}

TEST_F(AggStageTest, ExpansionToIdLookupSucceeds) {
    auto expandToIdLookupAstNode =
        std::make_unique<ExtensionAggStageParseNode>(ExpandToIdLookupNode::make());

    // Transfer ownership from the SDK-style unique_ptr to the OwnedHandle.
    auto handle = extension::AggStageParseNodeHandle{expandToIdLookupAstNode.release()};

    auto expanded = handle.expand();
    ASSERT_EQUALS(expanded.size(), 1);

    ASSERT_TRUE(std::holds_alternative<extension::AggStageAstNodeHandle>(expanded[0]));

    const auto& expandHandle = asAst(expanded[0]);
    ASSERT_EQUALS(expandHandle.getName(), "$_internalSearchIdLookup");
}

TEST_F(AggStageTest, HandlesPreventMemoryLeaksOnSuccess) {
    shared_test_stages::CountingAst::alive = 0;
    shared_test_stages::CountingParse::alive = 0;

    auto nestedDesugarParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::NestedDesugaringParseNode::make());

    {
        auto handle = extension::AggStageParseNodeHandle{nestedDesugarParseNode};

        [[maybe_unused]] auto expanded = handle.expand();
        ASSERT_EQUALS(shared_test_stages::CountingAst::alive, 1);
        ASSERT_EQUALS(shared_test_stages::CountingParse::alive, 1);
    }

    // Assert that the result of expand(), a vector of VariantNodeHandles, is properly cleaned up
    // once it goes out of scope.
    ASSERT_EQUALS(shared_test_stages::CountingAst::alive, 0);
    ASSERT_EQUALS(shared_test_stages::CountingParse::alive, 0);
}

TEST_F(AggStageTest, HandlesPreventMemoryLeaksOnFailure) {
    shared_test_stages::CountingAst::alive = 0;
    shared_test_stages::CountingParse::alive = 0;

    auto nestedDesugarParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::NestedDesugaringParseNode::make());

    auto handle = extension::AggStageParseNodeHandle{nestedDesugarParseNode};

    auto failExpand = globalFailPointRegistry().find("failExtensionExpand");
    failExpand->setMode(FailPoint::skip, 1);

    ASSERT_THROWS_CODE(
        [&] {
            [[maybe_unused]] auto expanded = handle.expand();
        }(),
        DBException,
        11113805);

    // Assert that the result of expand(), a vector of VariantNodeHandles, is properly cleaned up
    // after an exception is thrown.
    ASSERT_EQUALS(shared_test_stages::CountingAst::alive, 0);
    ASSERT_EQUALS(shared_test_stages::CountingParse::alive, 0);

    failExpand->setMode(FailPoint::off, 0);
}

TEST_F(AggStageTest, ExtExpandPreventsMemoryLeaksOnFailure) {
    shared_test_stages::CountingAst::alive = 0;
    shared_test_stages::CountingParse::alive = 0;

    auto nestedDesugarParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::NestedDesugaringParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{nestedDesugarParseNode};

    auto failExpand = globalFailPointRegistry().find("failVariantNodeConversion");
    failExpand->setMode(FailPoint::skip, 1);

    ASSERT_THROWS_CODE(
        [&] {
            [[maybe_unused]] auto expanded = handle.expand();
        }(),
        DBException,
        11197200);

    ASSERT_EQUALS(shared_test_stages::CountingAst::alive, 0);
    ASSERT_EQUALS(shared_test_stages::CountingParse::alive, 0);

    failExpand->setMode(FailPoint::off, 0);
}

TEST_F(AggStageTest, NoOpAstNodeTest) {
    auto noOpAggStageAstNode =
        new ExtensionAggStageAstNode(shared_test_stages::NoOpAggStageAstNode::make());
    auto handle = extension::AggStageAstNodeHandle{noOpAggStageAstNode};

    [[maybe_unused]] auto logicalStageHandle = handle.bind();
}

TEST_F(AggStageTest, NoOpAstNodeWithDefaultGetPropertiesSucceeds) {
    auto astNode =
        new sdk::ExtensionAggStageAstNode(sdk::shared_test_stages::NoOpAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};
    auto props = handle.getProperties();
    ASSERT_EQ(props.getPosition(), MongoExtensionPositionRequirementEnum::kNone);
    ASSERT_EQ(props.getHostType(), MongoExtensionHostTypeRequirementEnum::kNone);
    ASSERT_EQ(props.getRequiresInputDocSource(), true);
}

TEST_F(AggStageTest, NonePosAstNodeSucceeds) {
    auto astNode =
        new sdk::ExtensionAggStageAstNode(sdk::shared_test_stages::NonePosAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};
    auto props = handle.getProperties();
    ASSERT_EQ(props.getPosition(), MongoExtensionPositionRequirementEnum::kNone);
}

TEST_F(AggStageTest, FirstPosAstNodeSucceeds) {
    auto astNode =
        new sdk::ExtensionAggStageAstNode(sdk::shared_test_stages::FirstPosAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};
    auto props = handle.getProperties();
    ASSERT_EQ(props.getPosition(), MongoExtensionPositionRequirementEnum::kFirst);
}

TEST_F(AggStageTest, LastPosAstNodeSucceeds) {
    auto astNode =
        new sdk::ExtensionAggStageAstNode(sdk::shared_test_stages::LastPosAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};
    auto props = handle.getProperties();
    ASSERT_EQ(props.getPosition(), MongoExtensionPositionRequirementEnum::kLast);
}

TEST_F(AggStageTest, BadPosAstNodeFails) {
    auto astNode =
        new sdk::ExtensionAggStageAstNode(sdk::shared_test_stages::BadPosAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};
    ASSERT_THROWS_CODE(handle.getProperties(), DBException, ErrorCodes::BadValue);
}

TEST_F(AggStageTest, BadPosTypeAstNodeFails) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::BadPosTypeAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};
    ASSERT_THROWS_CODE(handle.getProperties(), DBException, ErrorCodes::TypeMismatch);
}

TEST_F(AggStageTest, UnknownPropertyAstNodeIsIgnored) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::UnknownPropertyAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};
    auto props = handle.getProperties();
    ASSERT_EQ(props.getPosition(), MongoExtensionPositionRequirementEnum::kNone);
}

TEST_F(AggStageTest, TransformAggStageAstNodeSucceeds) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::TransformAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};
    auto props = handle.getProperties();
    ASSERT_EQ(props.getRequiresInputDocSource(), true);
}

TEST_F(AggStageTest, SearchLikeSourceAggStageAstNodeSucceeds) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::SearchLikeSourceAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};
    auto props = handle.getProperties();
    ASSERT_EQ(props.getPosition(), MongoExtensionPositionRequirementEnum::kFirst);
    ASSERT_EQ(props.getHostType(), MongoExtensionHostTypeRequirementEnum::kAnyShard);
    ASSERT_EQ(props.getRequiresInputDocSource(), false);
}

TEST_F(AggStageTest, BadRequiresInputDocSourceTypeAggStageAstNodeFails) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::BadRequiresInputDocSourceTypeAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};
    ASSERT_THROWS_CODE(handle.getProperties(), DBException, ErrorCodes::TypeMismatch);
}

class InvalidResourcePatternRequiredPrivilegesAggStageAstNode : public sdk::AggStageAstNode {
public:
    static constexpr std::string_view kName = "$invalidResourcePattern";

    InvalidResourcePatternRequiredPrivilegesAggStageAstNode() : sdk::AggStageAstNode(kName) {}

    BSONObj getProperties() const override {
        return BSON("requiredPrivileges" << BSON_ARRAY(BSON(
                        "resourcePattern" << "database"
                                          << "actions" << BSON_ARRAY(BSON("action" << "find")))));
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<sdk::shared_test_stages::NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<InvalidResourcePatternRequiredPrivilegesAggStageAstNode>();
    }
};

class InvalidActionTypeRequiredPrivilegesAggStageAstNode : public sdk::AggStageAstNode {
public:
    static constexpr std::string_view kName = "$invalidActionType";

    InvalidActionTypeRequiredPrivilegesAggStageAstNode() : sdk::AggStageAstNode(kName) {}

    BSONObj getProperties() const override {
        return BSON("requiredPrivileges" << BSON_ARRAY(BSON(
                        "resourcePattern" << "namespace"
                                          << "actions" << BSON_ARRAY(BSON("action" << "update")))));
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<sdk::shared_test_stages::NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<InvalidActionTypeRequiredPrivilegesAggStageAstNode>();
    }
};

class BadTypeRequiredPrivilegesAstNode : public sdk::AggStageAstNode {
public:
    static constexpr std::string_view kName = "$badTypeRequiredPrivileges";

    BadTypeRequiredPrivilegesAstNode() : sdk::AggStageAstNode(kName) {}

    BSONObj getProperties() const override {
        return BSON("requiredPrivileges" << BSON(
                        "resourcePattern" << "namespace"
                                          << "actions" << BSON_ARRAY(BSON("action" << "find"))));
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<sdk::shared_test_stages::NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<BadTypeRequiredPrivilegesAstNode>();
    }
};

TEST_F(AggStageTest, InvalidResourcePatternRequiredPrivilegesAggStageAstNodeFails) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        InvalidResourcePatternRequiredPrivilegesAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};
    ASSERT_THROWS_CODE(handle.getProperties(), DBException, ErrorCodes::BadValue);
}

TEST_F(AggStageTest, InvalidActionTypeRequiredPrivilegesAggStageAstNodeFails) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        InvalidActionTypeRequiredPrivilegesAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};
    ASSERT_THROWS_CODE(handle.getProperties(), DBException, ErrorCodes::BadValue);
}

TEST_F(AggStageTest, BadTypeRequiredPrivilegesAstNodeFails) {
    auto astNode = new sdk::ExtensionAggStageAstNode(BadTypeRequiredPrivilegesAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};
    ASSERT_THROWS_CODE(handle.getProperties(), DBException, ErrorCodes::TypeMismatch);
}

class SimpleSerializationLogicalStage : public LogicalAggStage {
public:
    static constexpr StringData kStageName = "$simpleSerialization";
    static constexpr StringData kStageSpec = "mongodb";

    SimpleSerializationLogicalStage() {}

    BSONObj serialize() const override {
        return BSON(kStageName << kStageSpec);
    }

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON(kStageName << verbosity);
    }

    // TODO (SERVER-112395): Implement this function for testing.
    std::unique_ptr<ExecAggStage> compile() const override {
        return nullptr;
    }

    static inline std::unique_ptr<extension::sdk::LogicalAggStage> make() {
        return std::make_unique<SimpleSerializationLogicalStage>();
    }
};

TEST(AggregationStageTest, SimpleSerializationSucceeds) {
    auto logicalStage =
        new extension::sdk::ExtensionLogicalAggStage(SimpleSerializationLogicalStage::make());
    auto handle = extension::LogicalAggStageHandle{logicalStage};

    auto serialized = handle.serialize();
    ASSERT_BSONOBJ_EQ(BSON(SimpleSerializationLogicalStage::kStageName
                           << SimpleSerializationLogicalStage::kStageSpec),
                      serialized);
}

TEST(AggregationStageTest, Explain) {
    auto logicalStage =
        new extension::sdk::ExtensionLogicalAggStage(SimpleSerializationLogicalStage::make());
    auto handle = extension::LogicalAggStageHandle{logicalStage};

    // Test that different verbosity levels can be passed through to the extension implementation
    // correctly.
    {
        auto output = handle.explain(ExplainOptions::Verbosity::kQueryPlanner);
        ASSERT_BSONOBJ_EQ(BSON(SimpleSerializationLogicalStage::kStageName
                               << ::MongoExtensionExplainVerbosity::kQueryPlanner),
                          output);
    }

    {
        auto output = handle.explain(ExplainOptions::Verbosity::kExecStats);
        ASSERT_BSONOBJ_EQ(BSON(SimpleSerializationLogicalStage::kStageName
                               << ::MongoExtensionExplainVerbosity::kExecStats),
                          output);
    }

    {
        auto output = handle.explain(ExplainOptions::Verbosity::kExecAllPlans);
        ASSERT_BSONOBJ_EQ(BSON(SimpleSerializationLogicalStage::kStageName
                               << ::MongoExtensionExplainVerbosity::kExecAllPlans),
                          output);
    }
}

class SimpleQueryShapeParseNode : public sdk::AggStageParseNode {
public:
    static constexpr StringData kStageName = "$simpleQueryShape";
    static constexpr StringData kStageSpec = "mongodb";

    SimpleQueryShapeParseNode() : sdk::AggStageParseNode(toStdStringViewForInterop(kStageName)) {}

    size_t getExpandedSize() const override {
        return 0;
    }

    std::vector<VariantNodeHandle> expand() const override {
        return {};
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSON(kStageName << kStageSpec);
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<SimpleQueryShapeParseNode>();
    }
};

TEST_F(AggStageTest, SimpleComputeQueryShapeSucceeds) {
    auto parseNode =
        new extension::sdk::ExtensionAggStageParseNode(SimpleQueryShapeParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts{};
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);
    ASSERT_BSONOBJ_EQ(
        BSON(SimpleQueryShapeParseNode::kStageName << SimpleQueryShapeParseNode::kStageSpec),
        queryShape);
}

class IdentifierQueryShapeParseNode : public sdk::AggStageParseNode {
public:
    static constexpr StringData kStageName = "$identifierQueryShape";
    static constexpr StringData kIndexFieldName = "index";
    static constexpr StringData kIndexValue = "identifier";

    IdentifierQueryShapeParseNode()
        : sdk::AggStageParseNode(toStdStringViewForInterop(kStageName)) {}

    size_t getExpandedSize() const override {
        return 0;
    }

    std::vector<VariantNodeHandle> expand() const override {
        return {};
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        sdk::QueryShapeOptsHandle ctxHandle(ctx);
        BSONObjBuilder builder;

        builder.append(kIndexFieldName, ctxHandle.serializeIdentifier(std::string(kIndexValue)));

        return BSON(kStageName << builder.obj());
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<IdentifierQueryShapeParseNode>();
    }

    static std::string applyHmacForTest(StringData sd) {
        return "REDACT_" + std::string{sd};
    }
};

TEST_F(AggStageTest, SerializingIdentifierQueryShapeSucceedsWithNoTransformation) {
    auto parseNode = new ExtensionAggStageParseNode(IdentifierQueryShapeParseNode::make());

    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts{};
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);
    ASSERT_BSONOBJ_EQ(BSON(IdentifierQueryShapeParseNode::kStageName
                           << BSON(IdentifierQueryShapeParseNode::kIndexFieldName
                                   << IdentifierQueryShapeParseNode::kIndexValue)),
                      queryShape);
}

TEST_F(AggStageTest, SerializingIdentifierQueryShapeSucceedsWithTransformation) {
    auto parseNode = new ExtensionAggStageParseNode(IdentifierQueryShapeParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
    opts.transformIdentifiers = true;
    opts.transformIdentifiersCallback = IdentifierQueryShapeParseNode::applyHmacForTest;

    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);
    ASSERT_BSONOBJ_EQ(BSON(IdentifierQueryShapeParseNode::kStageName
                           << BSON(IdentifierQueryShapeParseNode::kIndexFieldName
                                   << IdentifierQueryShapeParseNode::applyHmacForTest(
                                          IdentifierQueryShapeParseNode::kIndexValue))),
                      queryShape);
}

TEST_F(AggStageTest, DesugarToEmptyDescriptorParseTest) {
    auto descriptor = std::make_unique<ExtensionAggStageDescriptor>(
        shared_test_stages::NoOpAggStageDescriptor::make());
    auto handle = extension::AggStageDescriptorHandle{descriptor.get()};

    BSONObj stageBson =
        BSON(shared_test_stages::NoOpAggStageDescriptor::kStageName << BSON("foo" << true));
    auto parseNodeHandle = handle.parse(stageBson);

    auto expanded = parseNodeHandle.expand();

    ASSERT_EQUALS(expanded.size(), 1);
    ASSERT_TRUE(std::holds_alternative<extension::AggStageAstNodeHandle>(expanded[0]));
}

TEST_F(AggStageTest, SourceStageParseTest) {
    auto descriptor = std::make_unique<ExtensionAggStageDescriptor>(
        shared_test_stages::SourceAggStageDescriptor::make());
    auto handle = extension::AggStageDescriptorHandle{descriptor.get()};

    BSONObj stageBson =
        BSON(shared_test_stages::SourceAggStageDescriptor::kStageName << BSON("foo" << true));
    auto parseNodeHandle = handle.parse(stageBson);
    ASSERT_EQ(shared_test_stages::SourceAggStageDescriptor::kStageName, handle.getName());
}

class FieldPathQueryShapeParseNode : public sdk::AggStageParseNode {
public:
    static constexpr StringData kStageName = "$fieldPathQueryShape";
    static constexpr StringData kSingleFieldPath = "simpleField";
    static constexpr StringData kNestedFieldPath = "nested.Field.Path";

    FieldPathQueryShapeParseNode()
        : sdk::AggStageParseNode(toStdStringViewForInterop(kStageName)) {}

    size_t getExpandedSize() const override {
        return 0;
    }

    std::vector<VariantNodeHandle> expand() const override {
        return {};
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        sdk::QueryShapeOptsHandle ctxHandle(ctx);
        BSONObjBuilder builder;

        builder.append(kSingleFieldPath,
                       ctxHandle.serializeFieldPath(std::string(kSingleFieldPath)));
        builder.append(kNestedFieldPath,
                       ctxHandle.serializeFieldPath(std::string(kNestedFieldPath)));

        return BSON(kStageName << builder.obj());
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<FieldPathQueryShapeParseNode>();
    }

    static std::string applyHmacForTest(StringData sd) {
        return "REDACT_" + std::string{sd};
    }
};

TEST_F(AggStageTest, SerializingFieldPathQueryShapeSucceedsWithNoTransformation) {
    auto parseNode = new ExtensionAggStageParseNode(FieldPathQueryShapeParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts{};
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);

    ASSERT_BSONOBJ_EQ(BSON(FieldPathQueryShapeParseNode::kStageName
                           << BSON(FieldPathQueryShapeParseNode::kSingleFieldPath
                                   << FieldPathQueryShapeParseNode::kSingleFieldPath
                                   << FieldPathQueryShapeParseNode::kNestedFieldPath
                                   << FieldPathQueryShapeParseNode::kNestedFieldPath)),
                      queryShape);
}

TEST_F(AggStageTest, SerializingFieldPathQueryShapeSucceedsWithTransformation) {
    auto parseNode = new ExtensionAggStageParseNode(FieldPathQueryShapeParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts{};
    opts.transformIdentifiers = true;
    opts.transformIdentifiersCallback = FieldPathQueryShapeParseNode::applyHmacForTest;

    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);

    auto transformedSingleField =
        opts.transformIdentifiersCallback(FieldPathQueryShapeParseNode::kSingleFieldPath);

    std::stringstream ss;
    ss << opts.transformIdentifiersCallback("nested") << "."
       << opts.transformIdentifiersCallback("Field") << "."
       << opts.transformIdentifiersCallback("Path");
    auto transformedNestedField = ss.str();

    ASSERT_BSONOBJ_EQ(
        BSON(FieldPathQueryShapeParseNode::kStageName
             << BSON(FieldPathQueryShapeParseNode::kSingleFieldPath
                     << transformedSingleField << FieldPathQueryShapeParseNode::kNestedFieldPath
                     << transformedNestedField)),
        queryShape);
}

class LiteralQueryShapeParseNode : public sdk::AggStageParseNode {
public:
    static constexpr StringData kStageName = "$literalQueryShape";
    static constexpr StringData kStringField = "str";
    static constexpr StringData kStringValue = "mongodb";
    static constexpr StringData kNumberField = "num";
    static constexpr int kNumberValue = 5;
    static constexpr StringData kObjectField = "obj";
    static const BSONObj kObjectValue;
    static constexpr StringData kDateField = "date";
    static const Date_t kDateValue;

    LiteralQueryShapeParseNode() : sdk::AggStageParseNode(toStdStringViewForInterop(kStageName)) {}

    size_t getExpandedSize() const override {
        return 0;
    }

    std::vector<VariantNodeHandle> expand() const override {
        return {};
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        sdk::QueryShapeOptsHandle ctxHandle(ctx);

        // Build a BSON object for the spec and keep the memory in scope across the calls to
        // serialize literal.
        BSONObjBuilder specBuilder;
        specBuilder.append(kStringField, kStringValue);
        specBuilder.append(kNumberField, kNumberValue);
        specBuilder.append(kObjectField, kObjectValue);
        specBuilder.append(kDateField, kDateValue);
        auto spec = specBuilder.obj();

        // Build the query shape.
        BSONObjBuilder builder;
        ctxHandle.appendLiteral(builder, kStringField, spec[kStringField]);
        ctxHandle.appendLiteral(builder, kNumberField, spec[kNumberField]);
        ctxHandle.appendLiteral(builder, kObjectField, spec[kObjectField]);
        ctxHandle.appendLiteral(builder, kDateField, spec[kDateField]);
        return BSON(kStageName << builder.obj());
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<LiteralQueryShapeParseNode>();
    }

    static std::string applyHmacForTest(StringData sd) {
        return "REDACT_" + std::string{sd};
    }
};

inline const BSONObj LiteralQueryShapeParseNode::kObjectValue = BSON("hi" << "mongodb");
inline const Date_t LiteralQueryShapeParseNode::kDateValue = Date_t::fromMillisSinceEpoch(1000);

TEST_F(AggStageTest, SerializingLiteralQueryShapeSucceedsWithNoTransformation) {
    auto parseNode = new ExtensionAggStageParseNode(LiteralQueryShapeParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts{};
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);

    BSONObjBuilder specBuilder;
    specBuilder.append(LiteralQueryShapeParseNode::kStringField,
                       LiteralQueryShapeParseNode::kStringValue);
    specBuilder.append(LiteralQueryShapeParseNode::kNumberField,
                       LiteralQueryShapeParseNode::kNumberValue);
    specBuilder.append(LiteralQueryShapeParseNode::kObjectField,
                       LiteralQueryShapeParseNode::kObjectValue);
    specBuilder.append(LiteralQueryShapeParseNode::kDateField,
                       LiteralQueryShapeParseNode::kDateValue);
    auto spec = specBuilder.obj();

    ASSERT_BSONOBJ_EQ(BSON(LiteralQueryShapeParseNode::kStageName << spec), queryShape);
}

TEST_F(AggStageTest, SerializingLiteralQueryShapeSucceedsWithDebugShape) {
    auto parseNode = new ExtensionAggStageParseNode(LiteralQueryShapeParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);

    BSONObjBuilder specBuilder;
    specBuilder.append(LiteralQueryShapeParseNode::kStringField, "?string");
    specBuilder.append(LiteralQueryShapeParseNode::kNumberField, "?number");
    specBuilder.append(LiteralQueryShapeParseNode::kObjectField, "?object");
    specBuilder.append(LiteralQueryShapeParseNode::kDateField, "?date");
    auto spec = specBuilder.obj();

    ASSERT_BSONOBJ_EQ(BSON(LiteralQueryShapeParseNode::kStageName << spec), queryShape);
}

TEST_F(AggStageTest, SerializingLiteralQueryShapeSucceedsWithRepresentativeValues) {
    auto parseNode = new ExtensionAggStageParseNode(LiteralQueryShapeParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts = SerializationOptions::kRepresentativeQueryShapeSerializeOptions;
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);

    BSONObjBuilder specBuilder;
    specBuilder.append(LiteralQueryShapeParseNode::kStringField, "?");
    specBuilder.append(LiteralQueryShapeParseNode::kNumberField, 1);
    specBuilder.append(LiteralQueryShapeParseNode::kObjectField, BSON("?" << "?"));
    specBuilder.append(LiteralQueryShapeParseNode::kDateField, Date_t::fromMillisSinceEpoch(0));
    auto spec = specBuilder.obj();

    ASSERT_BSONOBJ_EQ(BSON(LiteralQueryShapeParseNode::kStageName << spec), queryShape);
}

class ValidExtensionExecAggStage : public extension::sdk::ExecAggStage {
public:
    ValidExtensionExecAggStage(std::string stageName) : extension::sdk::ExecAggStage(stageName) {}

    extension::ExtensionGetNextResult getNext(
        const QueryExecutionContextHandle& expCtx,
        const MongoExtensionExecAggStage* execAggStage) override {
        if (_results.empty()) {
            return extension::ExtensionGetNextResult::eof();
        }
        if (_results.size() == 2) {
            // The result at the front of the queue is removed so that the size doesn't stay at 2.
            // This needs to be done so that the EOF case can be tested. Note that the behavior of
            // removing from the results queue for a "pause execution" state does not accurately
            // represent a "paused execution" state in a getNext() function.
            _results.pop_front();
            return extension::ExtensionGetNextResult::pauseExecution();
        } else {
            auto result = extension::ExtensionGetNextResult::advanced(_results.front());
            _results.pop_front();
            return result;
        }
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    static inline std::unique_ptr<extension::sdk::ExecAggStage> make() {
        return std::make_unique<ValidExtensionExecAggStage>("$noOp");
    }

private:
    std::deque<BSONObj> _results = {
        BSON("meow" << "adithi"), BSON("meow" << "josh"), BSON("meow" << "cedric")};
};

/**
 * Test class that tracks resource allocation and cleanup for lifecycle method testing.
 * open() allocates a resource, close() cleans it up, and reopen() reinitializes without cleanup.
 */
class ResourceTrackingExecAggStage : public ExecAggStage {
public:
    ResourceTrackingExecAggStage(std::string_view stageName) : ExecAggStage(stageName) {}

    ExtensionGetNextResult getNext(const QueryExecutionContextHandle& execCtx,
                                   const MongoExtensionExecAggStage* execAggStage) override {
        return extension::ExtensionGetNextResult::eof();
    }

    void open() override {
        _resourceAllocated = true;
        _initialized = true;
    }

    void reopen() override {
        _initialized = true;
    }

    void close() override {
        _resourceAllocated = false;
        _initialized = false;
    }

    bool isResourceAllocated() const {
        return _resourceAllocated;
    }

    bool isInitialized() const {
        return _initialized;
    }

    static inline std::unique_ptr<ExecAggStage> make() {
        return std::make_unique<ResourceTrackingExecAggStage>("$resourceTracking");
    }

private:
    bool _resourceAllocated = false;
    bool _initialized = false;
};

TEST(AggregationStageTest, ValidExecAggStageVTableGetNextSucceeds) {
    auto validExecAggStage = new extension::sdk::ExtensionExecAggStage(
        shared_test_stages::ValidExtensionExecAggStage::make());
    auto handle = extension::ExecAggStageHandle{validExecAggStage};

    auto nullExecCtx = host_connector::QueryExecutionContextAdapter(nullptr);

    auto getNext = handle.getNext(&nullExecCtx);
    ASSERT_EQUALS(extension::GetNextCode::kAdvanced, getNext.code);
    ASSERT_BSONOBJ_EQ(BSON("meow" << "adithi"), getNext.res.get());

    getNext = handle.getNext(&nullExecCtx);
    ASSERT_EQUALS(extension::GetNextCode::kPauseExecution, getNext.code);
    ASSERT_EQ(boost::none, getNext.res);

    getNext = handle.getNext(&nullExecCtx);
    ASSERT_EQUALS(extension::GetNextCode::kAdvanced, getNext.code);
    ASSERT_BSONOBJ_EQ(BSON("meow" << "cedric"), getNext.res.get());

    getNext = handle.getNext(&nullExecCtx);
    ASSERT_EQUALS(extension::GetNextCode::kEOF, getNext.code);
    ASSERT_EQ(boost::none, getNext.res);
};

TEST_F(AggStageTest, ValidateStructStateAfterConvertingStructToGetNextResult) {
    ::MongoExtensionGetNextResult result = {
        .code = static_cast<::MongoExtensionGetNextResultCode>(10), .result = nullptr};
    ASSERT_THROWS_WITH_CHECK(
        [&] {
            [[maybe_unused]] auto converted = convertCRepresentationToGetNextResult(&result);
        }(),
        AssertionException,
        [](const AssertionException& ex) {
            ASSERT_EQ(ex.code(), 10956803);
            ASSERT_STRING_CONTAINS(
                ex.reason(), str::stream() << "Invalid MongoExtensionGetNextResultCode: " << 10);
            assertionCount.tripwire.subtractAndFetch(1);
        });
    ASSERT_EQ(static_cast<::MongoExtensionGetNextResultCode>(10), result.code);
    ASSERT_EQ(nullptr, result.result);
}

class GetMetricsExtensionOperationMetrics : public OperationMetricsBase {
public:
    BSONObj serialize() const override {
        return BSON("counter" << _counter);
    }

    void update(MongoExtensionByteView) override {
        _counter++;
    }

private:
    int _counter = 0;
};

class GetMetricsExtensionExecAggStage
    : public extension::sdk::ExecAggStage,
      std::enable_shared_from_this<GetMetricsExtensionExecAggStage> {
public:
    GetMetricsExtensionExecAggStage(std::string stageName)
        : extension::sdk::ExecAggStage(stageName) {}

    extension::ExtensionGetNextResult getNext(
        const extension::sdk::QueryExecutionContextHandle& execCtx,
        const MongoExtensionExecAggStage* execAggStage) override {

        auto metrics = execCtx.getMetrics(execAggStage);
        metrics.update(MongoExtensionByteView{nullptr, 0});

        auto metricsBson = metrics.serialize();
        auto counterVal = metricsBson["counter"].Int();
        if (counterVal == 1) {
            return extension::ExtensionGetNextResult::advanced(BSON("hi" << "finley"));
        } else if (counterVal == 2) {
            return extension::ExtensionGetNextResult::eof();
        }

        tasserted(11213508, "counterVal can only be 1 or 2 at this point");
    }

    std::unique_ptr<OperationMetricsBase> createMetrics() const override {
        return std::make_unique<GetMetricsExtensionOperationMetrics>();
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    static inline std::unique_ptr<extension::sdk::ExecAggStage> make() {
        return std::make_unique<GetMetricsExtensionExecAggStage>("$getMetrics");
    }
};

TEST(AggregationStageTest, GetMetricsExtensionExecAggStageSucceeds) {
    QueryTestServiceContext testCtx;
    auto opCtx = testCtx.makeOperationContext();

    auto getMetricsExecAggStage =
        new extension::sdk::ExtensionExecAggStage(GetMetricsExtensionExecAggStage::make());
    auto handle = ExecAggStageHandle{getMetricsExecAggStage};

    // Create a test expression context that can be wrapped by QueryExecutionContextAdapter.
    auto expCtx = make_intrusive<ExpressionContextForTest>(
        opCtx.get(),
        NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd),
        SerializationContext());
    std::unique_ptr<host::QueryExecutionContext> wrappedCtx =
        std::make_unique<host::QueryExecutionContext>(expCtx.get());
    host_connector::QueryExecutionContextAdapter adapter(std::move(wrappedCtx));

    // Call getNext which triggers the getMetrics call logic.
    auto getNext = handle.getNext(&adapter);
    ASSERT_EQUALS(extension::GetNextCode::kAdvanced,
                  getNext.code);  // should return Advanced, since the metrics counter should be 1.

    // Call getNext again, which should build on the existing metrics from the last call.
    getNext = handle.getNext(&adapter);
    ASSERT_EQUALS(extension::GetNextCode::kEOF,
                  getNext.code);  // should return EOF, since the metrics counter should be 2.

    // Now switch out the OpCtx and make sure that the metrics also get reset.
    QueryTestServiceContext newTestCtx;
    auto newOpCtx = newTestCtx.makeOperationContext();
    expCtx->setOperationContext(newOpCtx.get());

    // Call getNext which triggers the getMetrics call logic.
    getNext = handle.getNext(&adapter);
    ASSERT_EQUALS(extension::GetNextCode::kAdvanced,
                  getNext.code);  // should return Advanced, since the metrics counter should be 1.
}

TEST_F(AggStageTest, TestValidExecAggStageFromCompiledLogicalAggStage) {
    auto logicalStage = new extension::sdk::ExtensionLogicalAggStage(
        shared_test_stages::TestLogicalStageCompile::make());
    auto handle = extension::LogicalAggStageHandle{logicalStage};

    auto compiledExecAggStageHandle = handle.compile();

    // Test getNext() on compiled ExecAggStage from LogicalStage.
    {
        auto getNext = compiledExecAggStageHandle.getNext(_execCtx.get());
        ASSERT_EQUALS(extension::GetNextCode::kAdvanced, getNext.code);
        ASSERT_BSONOBJ_EQ(BSON("meow" << "adithi"), getNext.res.get());
    }

    {
        auto getNext = compiledExecAggStageHandle.getNext(_execCtx.get());
        ASSERT_EQUALS(extension::GetNextCode::kPauseExecution, getNext.code);
        ASSERT_EQ(boost::none, getNext.res);
    }

    {
        auto getNext = compiledExecAggStageHandle.getNext(_execCtx.get());
        ASSERT_EQUALS(extension::GetNextCode::kAdvanced, getNext.code);
        ASSERT_BSONOBJ_EQ(BSON("meow" << "cedric"), getNext.res.get());
    }

    {
        auto getNext = compiledExecAggStageHandle.getNext(_execCtx.get());
        ASSERT_EQUALS(extension::GetNextCode::kEOF, getNext.code);
        ASSERT_EQ(boost::none, getNext.res);
    }
}

class TestSourceLogicalAggStage : public shared_test_stages::SourceLogicalAggStage {
public:
    static inline std::unique_ptr<extension::sdk::LogicalAggStage> make() {
        return std::make_unique<TestSourceLogicalAggStage>();
    }
};

TEST_F(AggStageTest, TestValidExecAggStageFromCompiledSourceLogicalAggStage) {
    auto logicalStage =
        new extension::sdk::ExtensionLogicalAggStage(TestSourceLogicalAggStage::make());
    auto handle = extension::LogicalAggStageHandle{logicalStage};

    auto compiledExecAggStageHandle = handle.compile();

    // Test getNext() on compiled ExecAggStage from LogicalStage.
    {
        auto getNext = compiledExecAggStageHandle.getNext(_execCtx.get());
        ASSERT_EQUALS(extension::GetNextCode::kAdvanced, getNext.code);
        ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "apples" << "red"), getNext.res.get());
    }

    {
        auto getNext = compiledExecAggStageHandle.getNext(_execCtx.get());
        ASSERT_EQUALS(extension::GetNextCode::kAdvanced, getNext.code);
        ASSERT_BSONOBJ_EQ(BSON("_id" << 2 << "oranges" << 5), getNext.res.get());
    }

    {
        auto getNext = compiledExecAggStageHandle.getNext(_execCtx.get());
        ASSERT_EQUALS(extension::GetNextCode::kAdvanced, getNext.code);
        ASSERT_BSONOBJ_EQ(BSON("_id" << 3 << "bananas" << false), getNext.res.get());
    }

    {
        auto getNext = compiledExecAggStageHandle.getNext(_execCtx.get());
        ASSERT_EQUALS(extension::GetNextCode::kAdvanced, getNext.code);
        ASSERT_BSONOBJ_EQ(
            BSON("_id" << 4 << "tropical fruits" << BSON_ARRAY("rambutan" << "durian" << "lychee")),
            getNext.res.get());
    }

    {
        auto getNext = compiledExecAggStageHandle.getNext(_execCtx.get());
        ASSERT_EQUALS(extension::GetNextCode::kAdvanced, getNext.code);
        ASSERT_BSONOBJ_EQ(BSON("_id" << 5 << "pie" << 3.14159), getNext.res.get());
    }

    {
        auto getNext = compiledExecAggStageHandle.getNext(_execCtx.get());
        ASSERT_EQUALS(extension::GetNextCode::kEOF, getNext.code);
        ASSERT_EQ(boost::none, getNext.res);
    }
}

TEST_F(AggStageTest, ValidateExecAggStageLifecycleFunctions) {
    auto trackingExecAggStageImpl = ResourceTrackingExecAggStage::make();
    auto* trackingExecAggStageImplPtr =
        static_cast<ResourceTrackingExecAggStage*>(trackingExecAggStageImpl.get());
    auto trackingExecAggStage =
        new extension::sdk::ExtensionExecAggStage(std::move(trackingExecAggStageImpl));
    auto handle = ExecAggStageHandle{trackingExecAggStage};

    // Open allocates resources.
    handle.open();
    ASSERT_TRUE(trackingExecAggStageImplPtr->isResourceAllocated());
    ASSERT_TRUE(trackingExecAggStageImplPtr->isInitialized());

    // Reopen reinitializes the stage without cleaning up resources.
    handle.reopen();
    ASSERT_TRUE(trackingExecAggStageImplPtr->isResourceAllocated());
    ASSERT_TRUE(trackingExecAggStageImplPtr->isInitialized());

    // Close cleans up all resources.
    handle.close();
    ASSERT_FALSE(trackingExecAggStageImplPtr->isResourceAllocated());
    ASSERT_FALSE(trackingExecAggStageImplPtr->isInitialized());
}

TEST_F(AggStageTest, TestDPLRaiiVecToAbiArrayRoundTrip) {
    shared_test_stages::CountingLogicalStage::alive = 0;
    shared_test_stages::CountingParse::alive = 0;

    auto logicalStage =
        new sdk::ExtensionLogicalAggStage(shared_test_stages::CountingLogicalStage::make());
    auto parseNode = new sdk::ExtensionAggStageParseNode(shared_test_stages::CountingParse::make());

    ASSERT_EQ(shared_test_stages::CountingLogicalStage::alive, 1);
    ASSERT_EQ(shared_test_stages::CountingParse::alive, 1);

    // Convert vector of RAII handles to an ABI array.
    std::vector<extension::VariantDPLHandle> originalVector;
    originalVector.emplace_back(extension::LogicalAggStageHandle{logicalStage});
    originalVector.emplace_back(extension::AggStageParseNodeHandle{parseNode});

    std::vector<::MongoExtensionDPLArrayElement> abiArray{originalVector.size()};
    ::MongoExtensionDPLArray abiArr{originalVector.size(), abiArray.data()};

    sdk::raiiVectorToAbiArray(std::move(originalVector), abiArr);

    // Verify ABI array is correctly populated.
    ASSERT_EQ(abiArray[0].type, ::MongoExtensionDPLArrayElementType::kLogical);
    ASSERT_NE(abiArray[0].element.logicalStage, nullptr);

    ASSERT_EQ(abiArray[1].type, ::MongoExtensionDPLArrayElementType::kParse);
    ASSERT_NE(abiArray[1].element.parseNode, nullptr);

    // Convert ABI array to vector of RAII handles (round-trip)
    auto roundTripVector = extension::dplArrayToRaiiVector(abiArr);

    // Verify handle vector is correctly populated.
    ASSERT_EQ(roundTripVector.size(), 2U);

    ASSERT_TRUE(std::holds_alternative<extension::LogicalAggStageHandle>(roundTripVector[0]));
    auto& logicalHandle = std::get<extension::LogicalAggStageHandle>(roundTripVector[0]);
    ASSERT_EQ(logicalHandle.get(), logicalStage);

    ASSERT_TRUE(std::holds_alternative<extension::AggStageParseNodeHandle>(roundTripVector[1]));
    auto& parseHandle = std::get<extension::AggStageParseNodeHandle>(roundTripVector[1]);
    ASSERT_EQ(parseHandle.get(), parseNode);

    // Verify that the ABI array elements' ownership has been transferred, but the instances are
    // still alive.
    ASSERT_EQ(abiArray[0].element.logicalStage, nullptr);
    ASSERT_EQ(abiArray[1].element.parseNode, nullptr);
    ASSERT_EQ(shared_test_stages::CountingLogicalStage::alive, 1);
    ASSERT_EQ(shared_test_stages::CountingParse::alive, 1);
}

TEST_F(AggStageTest, TestDPLRaiiVecToAbiArrayWithFailPoint) {
    shared_test_stages::CountingLogicalStage::alive = 0;
    shared_test_stages::CountingParse::alive = 0;

    auto logicalStage =
        new sdk::ExtensionLogicalAggStage(shared_test_stages::CountingLogicalStage::make());
    auto parseNode = new sdk::ExtensionAggStageParseNode(shared_test_stages::CountingParse::make());

    ASSERT_EQ(shared_test_stages::CountingLogicalStage::alive, 1);
    ASSERT_EQ(shared_test_stages::CountingParse::alive, 1);

    std::vector<extension::VariantDPLHandle> dplVec;
    dplVec.emplace_back(extension::LogicalAggStageHandle{logicalStage});
    dplVec.emplace_back(extension::AggStageParseNodeHandle{parseNode});

    std::vector<::MongoExtensionDPLArrayElement> abiArray{dplVec.size()};
    ::MongoExtensionDPLArray abiArr{dplVec.size(), abiArray.data()};

    // Enable the fail point to test error handling.
    auto failDPLConversion = globalFailPointRegistry().find("failVariantDPLConversion");
    failDPLConversion->setMode(FailPoint::skip, 1);

    ASSERT_THROWS_CODE(
        [&] {
            extension::sdk::raiiVectorToAbiArray(std::move(dplVec), abiArr);
        }(),
        DBException,
        11365501);

    failDPLConversion->setMode(FailPoint::off, 0);

    // Verify all instances are destroyed even after exception.
    ASSERT_EQ(shared_test_stages::CountingLogicalStage::alive, 0);
    ASSERT_EQ(shared_test_stages::CountingParse::alive, 0);
}

}  // namespace

}  // namespace mongo::extension::sdk
